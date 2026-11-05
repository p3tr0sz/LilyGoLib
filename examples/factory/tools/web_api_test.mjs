#!/usr/bin/env node
// Checks a server against the web API v1 P1 routes of docs/web-api.md: the mock watch (web_mock_device.mjs)
// or the real watch. Uses its own test folder (/sd/.api-test or /flash/.api-test) and removes it at the end.
//
// Usage:
//   node web_api_test.mjs --url http://localhost:5620 --dev-pin       (mock: reads the PIN from /api/_dev/pin)
//   node web_api_test.mjs --url http://192.168.1.50 --pin 123456     (watch: PIN from its screen)
//   --root flash   test file routes on the internal flash instead of the SD card
// Needs Node 22+ (global WebSocket). Exit code 1 when a check fails.

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const BASE = opt('--url', 'http://localhost:5620').replace(/\/$/, '');
const ROOT = '/' + opt('--root', 'sd');
const T = `${ROOT}/.api-test`;
let PIN = opt('--pin', '');
let cookie = '';
let passed = 0, failed = 0;

async function call(method, path, body, { raw = false, headers = {} } = {}) {
  const h = { ...headers };
  if (cookie) h.cookie = cookie;
  let b = body;
  if (body !== undefined && !raw) { h['content-type'] = 'application/json'; b = JSON.stringify(body); }
  const r = await fetch(BASE + path, { method, headers: h, body: b, redirect: 'manual' });
  const text = await r.text();
  let json = null; try { json = JSON.parse(text); } catch {}
  return { status: r.status, json, text, headers: r.headers };
}
function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  ok    ${name}`); }
  else { failed++; console.log(`  FAIL  ${name}${detail !== undefined ? `  -> ${typeof detail === 'string' ? detail : JSON.stringify(detail)}` : ''}`); }
}
const q = encodeURIComponent;

// Wait for the server (the simulator takes a few seconds to start).
for (let i = 0; ; i++) {
  try { if ((await fetch(`${BASE}/api/caps`)).ok) break; } catch {}
  if (i >= 60) { console.log(`no server at ${BASE}`); process.exit(2); }
  await new Promise((x) => setTimeout(x, 500));
}
console.log(`web API v1 test: ${BASE}  (files on ${ROOT})`);

// ---- public ----
console.log('public');
let r = await call('GET', '/api/caps');
check('GET /api/caps', r.status === 200 && r.json?.api === 1 && Array.isArray(r.json.caps) && typeof r.json.board === 'string', r.json);
const caps = r.json?.caps || [];
for (const c of ['files', 'sd', 'settings', 'time']) check(`caps has ${c}`, caps.includes(c), caps);
r = await call('GET', '/api/device');
check('GET /api/device unpaired: small view', r.status === 200 && r.json?.battery && !r.json.net && !r.json.mem, r.json);
r = await call('GET', '/api/auth');
check('GET /api/auth unpaired', r.status === 200 && r.json?.paired === false && r.json.method === 'pin', r.json);
for (const [m, p] of [['GET', '/api/settings'], ['GET', `/api/files?path=${q(ROOT)}`], ['GET', '/api/sd']]) {
  r = await call(m, p);
  check(`${m} ${p.split('?')[0]} needs pairing`, r.status === 401 && r.json?.error === 'unpaired', r.status);
}
r = await call('POST', '/api/auth/pair', { pin: 'xxxxxx' });
check('wrong PIN refused', r.status === 401 && r.json?.error === 'bad_pin', r.json);

// ---- pair ----
if (!PIN && args.includes('--dev-pin')) PIN = (await call('GET', '/api/_dev/pin')).json?.pin;
if (!PIN) { console.log('no PIN: pass --pin <pin> (from the watch) or --dev-pin (mock)'); process.exit(2); }
r = await call('POST', '/api/auth/pair', { pin: PIN });
const setCookie = r.headers.get('set-cookie') || '';
check('POST /api/auth/pair', r.status === 200 && r.json?.ok === true && /session=/.test(setCookie), r.json);
check('session cookie is HttpOnly', /HttpOnly/i.test(setCookie), setCookie);
cookie = setCookie.split(';')[0];

console.log('system');
r = await call('GET', '/api/auth');
check('GET /api/auth paired', r.json?.paired === true, r.json);
r = await call('GET', '/api/device');
check('GET /api/device paired: full view', r.status === 200 && r.json?.net && r.json.time && r.json.mem && r.json.sd, r.json);
const now = Math.floor(Date.now() / 1000);
r = await call('POST', '/api/time', { epoch: now, tz: 'Europe/Warsaw', offset_min: 60 });
check('POST /api/time', r.status === 200, r.json);
r = await call('GET', '/api/device');
check('time set', Math.abs((r.json?.time?.epoch || 0) - now) <= 5 && r.json.time.synced === true && r.json.time.tz === 'Europe/Warsaw' && r.json.time.offset_min === 60, r.json?.time);
r = await call('GET', '/api/settings');
const before = r.json;
check('GET /api/settings', r.status === 200 && typeof before?.brightness === 'number', before);
r = await call('PATCH', '/api/settings', { brightness: 42 });
check('PATCH /api/settings', r.status === 200 && Math.abs(r.json?.brightness - 42) <= 1, r.json);
r = await call('PATCH', '/api/settings', { brightness: 'x' });
check('PATCH /api/settings rejects a bad value', r.status === 400, r.status);
r = await call('PATCH', '/api/settings', { brightness: 101 });
check('PATCH /api/settings rejects out of range', r.status === 400, r.status);
if (before) await call('PATCH', '/api/settings', { brightness: before.brightness });

console.log('sd');
r = await call('GET', '/api/sd');
check('GET /api/sd', r.status === 200 && typeof r.json?.present === 'boolean' && 'error' in (r.json || {}), r.json);
const sdUp = !!r.json?.mounted;
if (ROOT === '/sd' && !sdUp) { console.log('SD card not mounted: insert one, or use --root flash'); process.exit(1); }

// ---- events ----
console.log('events');
const wsUrl = BASE.replace(/^http/, 'ws') + '/api/events';
const msgs = [];
const ws = new WebSocket(wsUrl, { headers: { cookie } });
const opened = await new Promise((done) => { ws.onopen = () => done(true); ws.onerror = () => done(false); setTimeout(() => done(false), 3000); });
ws.onmessage = (e) => { try { msgs.push(JSON.parse(e.data)); } catch {} };
check('GET /api/events opens', opened);
await new Promise((x) => setTimeout(x, 300));
check('first event is hello', msgs[0]?.t === 'hello' && msgs[0].d?.api === 1, msgs[0]);

// ---- files ----
console.log(`files (${T})`);
await call('DELETE', `/api/files?path=${q(T)}&recursive=1`);
r = await call('PUT', `/api/files/content?path=${q(T + '/a/b/hello.txt')}`, 'hello watch', { raw: true });
check('PUT creates parent folders', r.status === 200 && r.json?.size === 11, r.json);
r = await call('GET', `/api/files/content?path=${q(T + '/a/b/hello.txt')}`);
check('GET content', r.status === 200 && r.text === 'hello watch', r.text);
r = await call('GET', `/api/files/content?path=${q(T + '/a/b/hello.txt')}`, undefined, { headers: { range: 'bytes=6-' } });
check('GET content with Range', r.status === 206 && r.text === 'watch', `${r.status} ${r.text}`);
r = await call('PUT', `/api/files/content?path=${q(T + '/a/b/hello.txt')}&offset=11`, '!', { raw: true });
check('PUT append at the right offset', r.status === 200 && r.json?.size === 12, r.json);
r = await call('PUT', `/api/files/content?path=${q(T + '/a/b/hello.txt')}&offset=3`, '!', { raw: true });
check('PUT append at a wrong offset: 409 with size', r.status === 409 && r.json?.size === 12, r.json);
r = await call('GET', `/api/files?path=${q(T + '/a/b')}`);
check('list', r.status === 200 && r.json?.entries?.length === 1 && r.json.entries[0].name === 'hello.txt' && r.json.entries[0].dir === false, r.json);
r = await call('POST', '/api/files/mkdir', { path: T + '/c' });
check('mkdir', r.status === 200, r.json);
r = await call('POST', '/api/files/mkdir', { path: T + '/c' });
check('mkdir existing is fine', r.status === 200, r.json);
r = await call('GET', `/api/files?path=${q(T)}`);
check('list: folders first', r.json?.entries?.map((e) => e.name).join() === 'a,c', r.json);
r = await call('POST', '/api/files/move', { from: T + '/a/b/hello.txt', to: T + '/c/hi.txt' });
check('move', r.status === 200, r.json);
r = await call('PUT', `/api/files/content?path=${q(T + '/c/x.txt')}`, 'x', { raw: true });
r = await call('POST', '/api/files/move', { from: T + '/c/x.txt', to: T + '/c/hi.txt' });
check('move onto existing without overwrite: 409', r.status === 409, r.status);
r = await call('POST', '/api/files/move', { from: T + '/c/x.txt', to: T + '/c/hi.txt', overwrite: true });
check('move with overwrite', r.status === 200, r.json);
r = await call('GET', `/api/files/content?path=${q(T + '/nope.txt')}`);
check('missing file: 404', r.status === 404 && r.json?.error === 'not_found', r.status);
r = await call('GET', `/api/files?path=${q(T + '/../')}`);
check('".." refused: 400', r.status === 400, r.status);
r = await call('GET', `/api/files?path=${q('/etc')}`);
check('unknown root refused: 400', r.status === 400, r.status);
r = await call('DELETE', `/api/files?path=${q(T + '/c')}`);
check('delete non-empty folder without recursive: 409', r.status === 409, r.status);
r = await call('DELETE', `/api/files?path=${q(T)}&recursive=1`);
check('delete recursive', r.status === 200, r.json);
r = await call('GET', `/api/files?path=${q(T)}`);
check('deleted', r.status === 404, r.status);

await new Promise((x) => setTimeout(x, 300));
const ops = msgs.filter((m) => m.t === 'files.changed').map((m) => m.d.op);
check('files.changed events', ['write', 'mkdir', 'move', 'delete'].every((o) => ops.includes(o)), ops);
check('events seq increases', msgs.slice(1).every((m, i) => m.seq > msgs[i].seq), msgs.map((m) => m.seq));

// ---- one desktop at a time ----
const ws2 = new WebSocket(wsUrl, { headers: { cookie } });
await new Promise((done) => { ws2.onopen = done; ws2.onerror = done; setTimeout(done, 3000); });
await new Promise((x) => setTimeout(x, 300));
check('a second desktop takes the seat (seat.lost)', msgs.some((m) => m.t === 'seat.lost'), msgs.map((m) => m.t));
ws.close(); ws2.close();

// ---- SD removal (mock only) ----
if (args.includes('--dev-pin') && ROOT === '/sd') {
  console.log('sd removal (mock)');
  await call('POST', '/api/_dev/sd', { present: false });
  r = await call('GET', `/api/files?path=${q('/sd')}`);
  check('no card: 503 sd_missing', r.status === 503 && r.json?.error === 'sd_missing', r.json);
  r = await call('GET', `/api/files?path=${q('/flash')}`);
  check('flash still works without a card', r.status === 200, r.status);
  await call('POST', '/api/_dev/sd', { present: true });
  r = await call('GET', '/api/sd');
  check('card back', r.json?.mounted === true, r.json);
}

r = await call('POST', '/api/auth/logout', {});
check('logout', r.status === 200 && r.json?.revoked === 1, r.json);
r = await call('GET', '/api/settings');
check('logged out: 401', r.status === 401, r.status);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
