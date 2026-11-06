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
for (const c of ['files', 'sd', 'settings', 'time', 'screen', 'input', 'display']) check(`caps has ${c}`, caps.includes(c), caps);
r = await call('GET', '/api/device');
check('GET /api/device unpaired: small view', r.status === 200 && r.json?.battery && !r.json.net && !r.json.mem, r.json);
check('device reports the watch theme', /^#[0-9a-f]{6}$/.test(r.json?.theme?.accent || '') && typeof r.json.theme.name === 'string', r.json?.theme);
r = await call('GET', '/api/auth');
check('GET /api/auth unpaired', r.status === 200 && r.json?.paired === false && r.json.method === 'pin', r.json);
for (const [m, p] of [['GET', '/api/settings'], ['GET', `/api/files?path=${q(ROOT)}`], ['GET', '/api/sd']]) {
  r = await call(m, p);
  check(`${m} ${p.split('?')[0]} needs pairing`, r.status === 401 && r.json?.error === 'unpaired', r.status);
}
// ---- pair (on the watch, Settings > Web desktop must be open: it shows the PIN and opens pairing)
const DEV = args.includes('--dev-pin');
if (!PIN && DEV) PIN = (await call('GET', '/api/_dev/pin')).json?.pin;
if (!PIN) { console.log('no PIN: pass --pin <pin> (from the watch) or --dev-pin (mock, simulator)'); process.exit(2); }
r = await call('POST', '/api/auth/pair', { pin: PIN === '000000' ? '111111' : '000000' });
check('wrong PIN refused', r.status === 401 && r.json?.error === 'bad_pin', r.json);
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
// The offset of this machine, as the desktop sends it (the simulator's clock follows the host).
const hostOffset = -new Date().getTimezoneOffset();
const hostTz = Intl.DateTimeFormat().resolvedOptions().timeZone;
r = await call('POST', '/api/time', { epoch: now, tz: hostTz, offset_min: hostOffset });
check('POST /api/time', r.status === 200, r.json);
r = await call('GET', '/api/device');
check('time set', Math.abs((r.json?.time?.epoch || 0) - now) <= 5 && r.json.time.synced === true && r.json.time.tz === hostTz && r.json.time.offset_min === hostOffset, r.json?.time);
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

if (caps.includes('desktop.install')) {
  r = await call('GET', '/api/desktop');
  check('GET /api/desktop', r.status === 200 && typeof r.json?.wanted === 'string' && typeof r.json.run === 'number' && 'zip_on_card' in r.json, r.json);
  r = await call('POST', '/api/desktop/install', { source: 'floppy' });
  check('desktop install: unknown source 400', r.status === 400, r.status);
}

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

// ---- screen, input, display
console.log('screen');
const decodeRle = (buf) => { let n = 0; for (let i = 0; i + 3 < buf.length; i += 4) n += buf.readUInt16LE(i); return n; };
let sr = await fetch(`${BASE}/api/screen`, { headers: { cookie } });
let sw = +sr.headers.get('x-width'), sh = +sr.headers.get('x-height');
let sbuf = Buffer.from(await sr.arrayBuffer());
const etag1 = sr.headers.get('etag');
check('GET /api/screen (rle565)', sr.status === 200 && sw > 0 && sh > 0 && decodeRle(sbuf) === sw * sh, { status: sr.status, sw, sh, px: decodeRle(sbuf) });
sr = await fetch(`${BASE}/api/screen`, { headers: { cookie, 'if-none-match': etag1 } });
check('screen ETag: 304, or a new frame with a new ETag', sr.status === 304 || (sr.status === 200 && sr.headers.get('etag') !== etag1), sr.status);
sr = await fetch(`${BASE}/api/screen?format=rgb565`, { headers: { cookie } });
sbuf = Buffer.from(await sr.arrayBuffer());
check('GET /api/screen?format=rgb565', sr.status === 200 && sbuf.length === sw * sh * 2, sbuf.length);
r = await call('GET', '/api/screen?format=png');
check('unknown screen format: 400', r.status === 400, r.status);
r = await call('POST', '/api/input', { type: 'tap', x: sw + 5, y: 10 });
check('tap outside the screen: 400', r.status === 400, r.status);
r = await call('POST', '/api/input', { type: 'pinch' });
check('unknown input type: 400', r.status === 400, r.status);
r = await call('POST', '/api/input', { type: 'button', name: 'wake' });
check('POST /api/input button wake', r.status === 200, r.json);
if (DEV) {
  r = await call('POST', '/api/input', { type: 'tap', x: 3, y: 4 });
  const last = (await call('GET', '/api/_dev/input')).json;
  check('POST /api/input tap reaches the watch', r.status === 200 && last?.type === 'tap' && last.x === 3 && last.y === 4, last);
}
r = await call('POST', '/api/display', { brightness: 'x' });
check('display: bad brightness 400', r.status === 400, r.status);
r = await call('POST', '/api/display', { on: true });
check('POST /api/display on', r.status === 200, r.json);

// ---- tools (P4): each block runs only when the watch lists the capability
const sleep = (ms) => new Promise((x) => setTimeout(x, ms));
const waitMsg = async (pred, ms = 4000) => { const end = Date.now() + ms; while (Date.now() < end) { const m = msgs.find(pred); if (m) return m; await sleep(100); } return null; };
const busyCheck = async (name, app, method, path, body) => {
  if (!DEV) return;
  await call('POST', '/api/_dev/app', { name: app });
  const b = await call(method, path, body);
  await call('POST', '/api/_dev/app', { name: '' });
  check(`${name}: 503 busy while ${app} is open on the watch`, b.status === 503 && b.json?.error === 'busy' && b.json.app === app, b.json);
};
const b64 = (s) => Buffer.from(s).toString('base64');

if (caps.includes('monitor')) {
  console.log('monitor');
  r = await call('GET', '/api/monitor');
  check('GET /api/monitor', r.status === 200 && typeof r.json?.uptime_s === 'number' && typeof r.json.mem?.heap_free === 'number' &&
    typeof r.json.battery?.percent === 'number' && typeof r.json.app === 'string', r.json);
  r = await call('GET', '/api/logs');
  check('GET /api/logs', r.status === 200 && Array.isArray(r.json?.lines) && typeof r.json.next === 'number', r.json);
  const next = r.json?.next ?? 0;
  r = await call('GET', `/api/logs?after=${next}`);
  check('logs after next: only newer lines', r.status === 200 && r.json.lines.every((l) => l.n > next), r.json);
}

if (caps.includes('notify')) {
  console.log('notify');
  r = await call('POST', '/api/notify', { title: '' });
  check('notify: empty title 400', r.status === 400, r.status);
  r = await call('POST', '/api/notify', { title: 'x', level: 'loud' });
  check('notify: unknown level 400', r.status === 400, r.status);
  r = await call('POST', '/api/notify', { id: 'api-test', title: 'API test', body: 'hello from the test', level: 'info', vibrate: false });
  check('POST /api/notify', r.status === 200 && r.json?.ok && r.json.id === 'api-test', r.json);
  const ev = await waitMsg((m) => m.t === 'notify' && m.d.id === 'api-test');
  check('notify event', ev && ev.d.title === 'API test' && typeof ev.d.ts === 'number', ev);
  r = await call('POST', '/api/notify', { title: 'no id' });
  check('notify without id gets one', r.status === 200 && typeof r.json?.id === 'string' && r.json.id.length > 0, r.json);
  r = await call('GET', '/api/notify');
  check('GET /api/notify: newest first', r.status === 200 && r.json?.items?.[0]?.title === 'no id' && r.json.items.some((n) => n.id === 'api-test'), r.json);
}

if (caps.includes('sensors.imu')) {
  console.log('sensors');
  r = await call('GET', '/api/sensors');
  check('GET /api/sensors', r.status === 200 && typeof r.json?.imu?.roll === 'number' && typeof r.json.imu.heading === 'number' &&
    'accel' in r.json.imu && 'steps' in r.json && typeof r.json.seq === 'number', r.json);
  await sleep(600);
  const r2 = await call('GET', '/api/sensors');
  check('sensors: new samples arrive', r2.json?.seq > r.json?.seq, [r.json?.seq, r2.json?.seq]);
}

if (caps.includes('gnss')) {
  console.log('gnss');
  r = await call('GET', '/api/gnss');
  const g = r.json;
  check('GET /api/gnss', r.status === 200 && typeof g?.fix === 'boolean' && Array.isArray(g.satellites) && typeof g.sats === 'number' &&
    ['ok', 'open', 'short', 'unknown'].includes(g.antenna), g);
  check('gnss: position only with a fix', g?.fix ? typeof g.lat === 'number' && typeof g.lon === 'number' : g?.lat === null, g);
  if (DEV) {
    const ev = await waitMsg((m) => m.t === 'gnss.fix', 3000);
    check('gnss.fix event while polled', ev && typeof ev.d.fix === 'boolean' && 'lat' in ev.d && 'sats' in ev.d, ev);
  }
}

if (caps.includes('lora')) {
  console.log('lora');
  r = await call('GET', '/api/lora/config');
  const cfg = r.json;
  check('GET /api/lora/config', r.status === 200 && typeof cfg?.freq === 'number' && typeof cfg.sf === 'number' && typeof cfg.listen === 'boolean', cfg);
  for (const [k, v] of [['sf', 13], ['bw', 99], ['freq', 1200], ['cr', 9], ['power', 30], ['sync_word', 300]]) {
    r = await call('PATCH', '/api/lora/config', { [k]: v });
    check(`lora config: bad ${k} 400`, r.status === 400, r.status);
  }
  r = await call('PATCH', '/api/lora/config', { sf: 9, bw: 250, listen: true });
  check('PATCH /api/lora/config', r.status === 200 && r.json?.sf === 9 && r.json.bw === 250 && r.json.listen === true && r.json.freq === cfg?.freq, r.json);
  r = await call('POST', '/api/lora/send', { text: '' });
  check('lora send: empty 400', r.status === 400, r.status);
  r = await call('POST', '/api/lora/send', { text: 'x'.repeat(256) });
  check('lora send: over 255 bytes 400', r.status === 400, r.status);
  r = await call('POST', '/api/lora/send', { text: 'hello lora' });
  check('POST /api/lora/send text', r.status === 200 && r.json?.len === 10, r.json);
  r = await call('POST', '/api/lora/send', { data_b64: b64('\x01\x02\x03') });
  check('POST /api/lora/send data_b64', r.status === 200 && r.json?.len === 3, r.json);
  if (DEV) {
    await call('POST', '/api/_dev/lora', { text: 'from afar', rssi: -88, snr: 7 });
    const ev = await waitMsg((m) => m.t === 'lora.rx' && Buffer.from(m.d.data_b64 || '', 'base64').toString() === 'from afar');
    check('lora.rx event', ev && ev.d.dir === 'rx' && ev.d.rssi === -88 && ev.d.len === 9, ev);
  }
  r = await call('GET', '/api/lora/log');
  const pk = r.json?.packets || [];
  check('GET /api/lora/log', r.status === 200 && pk.some((p) => p.dir === 'tx' && Buffer.from(p.data_b64, 'base64').toString() === 'hello lora' && p.rssi === null), pk.slice(-4));
  check('lora log: oldest first', pk.every((p, i) => !i || p.n > pk[i - 1].n), pk.map((p) => p.n));
  await busyCheck('lora', 'LoRa', 'POST', '/api/lora/send', { text: 'x' });
  if (cfg) await call('PATCH', '/api/lora/config', { sf: cfg.sf, bw: cfg.bw, listen: cfg.listen });
}

if (caps.includes('nfc')) {
  console.log('nfc');
  r = await call('POST', '/api/nfc/scan', { seconds: 0 });
  check('nfc scan: bad seconds 400', r.status === 400, r.status);
  await busyCheck('nfc', 'NFC', 'POST', '/api/nfc/scan', { seconds: 5 });
  r = await call('POST', '/api/nfc/scan', { seconds: 10 });
  check('POST /api/nfc/scan', r.status === 200 && r.json?.ok, r.json);
  const st = await waitMsg((m) => m.t === 'nfc.state' && m.d.scanning === true);
  check('nfc.state scanning', !!st, st);
  r = await call('GET', '/api/nfc');
  check('GET /api/nfc scanning', r.status === 200 && r.json?.scanning === true && 'last' in r.json, r.json);
  if (DEV) {
    await call('POST', '/api/_dev/nfc', { uid: '04:a1:b2:c3:d4:e5:f6', type: 'NTAG215', text: 'hi tag' });
    const ev = await waitMsg((m) => m.t === 'nfc.tag' && m.d.uid === '04:a1:b2:c3:d4:e5:f6');
    check('nfc.tag event', ev && ev.d.type === 'NTAG215' && ev.d.ndef?.some((x) => x.kind === 'text' && x.value === 'hi tag'), ev);
  }
  r = await call('POST', '/api/nfc/stop', {});
  check('POST /api/nfc/stop', r.status === 200, r.json);
  r = await call('GET', '/api/nfc');
  check('nfc stopped', r.json?.scanning === false, r.json);
}

if (caps.includes('recorder')) {
  console.log('recorder');
  const RP = `${T}-rec/a.wav`;
  r = await call('POST', '/api/recorder/start', { path: `${T}-rec/a.mp3` });
  check('recorder: path must be .wav', r.status === 400, r.status);
  r = await call('POST', '/api/recorder/stop', {});
  check('recorder: stop when idle 409', r.status === 409, r.status);
  await busyCheck('recorder', 'Recorder', 'POST', '/api/recorder/start', { path: RP });
  r = await call('POST', '/api/recorder/start', { path: RP, max_s: 5 });
  check('POST /api/recorder/start', r.status === 200 && r.json?.path === RP, r.json);
  r = await call('POST', '/api/recorder/start', { path: RP });
  check('recorder: start twice 409', r.status === 409, r.status);
  await sleep(1200);
  r = await call('GET', '/api/recorder');
  check('GET /api/recorder recording', r.json?.state === 'recording' && r.json.elapsed_ms > 500 && typeof r.json.level === 'number', r.json);
  check('recorder events', !!(await waitMsg((m) => m.t === 'recorder' && m.d.state === 'recording')), msgs.filter((m) => m.t === 'recorder').length);
  r = await call('POST', '/api/recorder/stop', {});
  check('POST /api/recorder/stop', r.status === 200 && r.json?.duration_ms >= 1000 && r.json.size > 44, r.json);
  const wav = Buffer.from(await (await fetch(`${BASE}/api/files/content?path=${q(RP)}`, { headers: { cookie } })).arrayBuffer());
  const pcm = wav.length - 44;
  check('recording is a 16 kHz mono WAV of that length', wav.toString('ascii', 0, 4) === 'RIFF' && wav.toString('ascii', 8, 12) === 'WAVE' &&
    wav.readUInt32LE(24) === 16000 && wav.readUInt16LE(22) === 1 && wav.readUInt32LE(40) === pcm && Math.abs(pcm / 32 - (r.json?.duration_ms || 0)) < 300,
    { len: wav.length, rate: wav.length > 28 && wav.readUInt32LE(24), dur: r.json?.duration_ms });
  if (caps.includes('audio.play')) {
    r = await call('POST', '/api/audio/play', { path: `${T}-rec/none.wav` });
    check('audio play: missing 404', r.status === 404, r.status);
    r = await call('POST', '/api/audio/play', { path: RP });
    check('POST /api/audio/play', r.status === 200, r.json);
    r = await call('GET', '/api/audio');
    check('GET /api/audio playing', r.json?.playing === true && r.json.path === RP && typeof r.json.volume === 'number', r.json);
    const vol = r.json?.volume;
    r = await call('PATCH', '/api/audio', { volume: 101 });
    check('audio volume out of range 400', r.status === 400, r.status);
    r = await call('PATCH', '/api/audio', { volume: 40 });
    check('PATCH /api/audio volume', r.status === 200 && Math.abs(r.json?.volume - 40) <= 1, r.json);
    if (typeof vol === 'number') await call('PATCH', '/api/audio', { volume: vol });
    r = await call('POST', '/api/audio/stop', {});
    check('POST /api/audio/stop', r.status === 200, r.json);
    r = await call('GET', '/api/audio');
    check('audio stopped', r.json?.playing === false, r.json);
  }
  await call('DELETE', `/api/files?path=${q(T + '-rec')}&recursive=1`);
}

if (caps.includes('wifi.scan')) {
  console.log('wifi scan');
  r = await call('GET', '/api/wifi/scan');
  check('GET /api/wifi/scan', r.status === 200 && typeof r.json?.scanning === 'boolean' && Array.isArray(r.json.networks) && 'ts' in r.json, r.json);
  r = await call('POST', '/api/wifi/scan', {});
  check('POST /api/wifi/scan: 202', r.status === 202 && r.json?.ok, r.json);
  const ev = await waitMsg((m) => m.t === 'wifi.scan' && m.d.scanning === false, 15000);
  const n = ev?.d?.networks || [];
  check('wifi.scan event with networks', ev && typeof ev.d.ts === 'number' && n.length > 0 &&
    n.every((x) => typeof x.ssid === 'string' && typeof x.rssi === 'number' && typeof x.auth === 'string' && typeof x.channel === 'number'), ev?.d);
  check('wifi: strongest first', n.every((x, i) => !i || x.rssi <= n[i - 1].rssi), n.map((x) => x.rssi));
  r = await call('GET', '/api/wifi/scan');
  check('GET /api/wifi/scan keeps the result', r.json?.scanning === false && r.json.networks.length === n.length, r.json);
}

if (caps.includes('ble.scan')) {
  console.log('ble scan');
  r = await call('POST', '/api/ble/scan', { seconds: 99 });
  check('ble scan: bad seconds 400', r.status === 400, r.status);
  await busyCheck('ble', 'BLE Scan', 'POST', '/api/ble/scan', { seconds: 1 });
  r = await call('POST', '/api/ble/scan', { seconds: 2 });
  check('POST /api/ble/scan: 202', r.status === 202 && r.json?.ok, r.json);
  r = await call('GET', '/api/ble/scan');
  check('GET /api/ble/scan while scanning', r.status === 200 && r.json?.scanning === true, r.json);
  const ev = await waitMsg((m) => m.t === 'ble.scan' && m.d.scanning === false, 15000);
  const d = ev?.d?.devices || [];
  check('ble.scan event with devices', ev && typeof ev.d.ts === 'number' &&
    d.every((x) => typeof x.addr === 'string' && typeof x.rssi === 'number' && Array.isArray(x.services) && typeof x.connectable === 'boolean'), ev?.d);
  check('ble: strongest first', d.every((x, i) => !i || x.rssi <= d[i - 1].rssi), d.map((x) => x.rssi));
}

if (caps.includes('link.espnow')) {
  console.log('link (esp-now)');
  r = await call('GET', '/api/link');
  check('GET /api/link', r.status === 200 && typeof r.json?.enabled === 'boolean' && Array.isArray(r.json.peers) &&
    typeof r.json.mac === 'string' && 'transfer' in r.json, r.json);
  r = await call('POST', '/api/link/enable', { on: true });
  check('POST /api/link/enable', r.status === 200 && r.json?.enabled === true, r.json);
  let peerMac = null;
  if (DEV) {
    await call('POST', '/api/_dev/link', { peer: 'T-Test-01' });
    const ev = await waitMsg((m) => m.t === 'link' && m.d.peers?.some((p) => p.name === 'T-Test-01'), 3000);
    peerMac = ev?.d?.peers?.find((p) => p.name === 'T-Test-01')?.mac
      || (await call('GET', '/api/link')).json?.peers?.find((p) => p.name === 'T-Test-01')?.mac;
    check('a peer appears after a hello', !!peerMac, ev?.d?.peers);
  }
  r = await call('POST', '/api/link/send', { path: '/sd/nope/x.bin', to_mac: 'aa:bb:cc:dd:ee:00' });
  check('link send to unknown peer: 404', r.status === 404 || r.status === 503, r.status);
  if (DEV && peerMac) {
    await call('PUT', `/api/files/content?path=${q(T + '-link/gift.bin')}`, 'x'.repeat(2000), { raw: true });
    r = await call('POST', '/api/link/send', { path: T + '-link/gift.bin', to_mac: peerMac });
    check('POST /api/link/send', r.status === 202 && r.json?.ok, r.json);
    const done = await waitMsg((m) => m.t === 'link' && m.d.transfer?.dir === 'tx' && m.d.transfer.state === 'done', 6000);
    check('link transfer reaches done', !!done, done?.d?.transfer);
    await call('DELETE', `/api/files?path=${q(T + '-link')}&recursive=1`);
    // incoming file
    await call('POST', '/api/_dev/link', { rx: { name: 'from-afar.bin', size: 1500 } });
    const rx = await waitMsg((m) => m.t === 'files.changed' && /\/sd\/data\/link\//.test(m.d.path || ''), 3000);
    check('an incoming file lands in /sd/data/link and fires files.changed', !!rx, rx?.d);
  }
  await call('POST', '/api/link/enable', { on: false });
}

if (caps.includes('usb.hid')) {
  console.log('usb hid');
  if (DEV) await call('POST', '/api/_dev/hid', { available: false });
  r = await call('GET', '/api/hid');
  check('GET /api/hid', r.status === 200 && typeof r.json?.available === 'boolean' && r.json.state === 'idle', r.json);
  r = await call('POST', '/api/hid/type', { text: 'hello' });
  check('hid type with no host: 503 hid_unavailable', r.status === 503 && r.json?.error === 'hid_unavailable', r.json);
  if (DEV) {
    await call('POST', '/api/_dev/hid', { available: true });
    r = await call('GET', '/api/hid');
    check('hid becomes available', r.json?.available === true, r.json);
    await busyCheck('hid', 'BadUSB', 'POST', '/api/hid/type', { text: 'x' });
    r = await call('POST', '/api/hid/type', { text: '' });
    check('hid type empty: 400', r.status === 400, r.status);
    r = await call('POST', '/api/hid/type', { text: 'typed over USB' });
    check('POST /api/hid/type', r.status === 200, r.json);
    r = await call('POST', '/api/hid/run', { text: 'STRING hello world\nENTER\nDELAY 20\nGUI r' });
    check('POST /api/hid/run', r.status === 202, r.json);
    check('hid goes to armed/running', !!(await waitMsg((m) => m.t === 'hid' && (m.d.state === 'armed' || m.d.state === 'running'), 3000)), null);
    r = await call('GET', '/api/hid');
    check('hid reports the script lines', r.json?.lines === 4 && (r.json.state === 'armed' || r.json.state === 'running'), r.json);
    r = await call('POST', '/api/hid/run', { text: 'STRING again' });
    check('hid run while running: 409', r.status === 409, r.status);
    r = await call('POST', '/api/hid/stop', {});
    check('POST /api/hid/stop', r.status === 200, r.json);
    check('hid returns to idle', !!(await waitMsg((m) => m.t === 'hid' && m.d.state === 'idle', 3000)), null);
  }
}

if (caps.includes('wifi.tools')) {
  console.log('wifi tools');
  r = await call('GET', '/api/net');
  check('GET /api/net', r.status === 200 && typeof r.json?.connected === 'boolean' && typeof r.json.mac === 'string', r.json);
  r = await call('POST', '/api/net/resolve', { host: 'example.com' });
  check('POST /api/net/resolve', r.status === 200 && r.json?.ok === true && typeof r.json.ip === 'string' && typeof r.json.ms === 'number', r.json);
  r = await call('POST', '/api/net/resolve', { host: 'definitelynotahost' });
  check('resolve a bad name: ok false', r.status === 200 && r.json?.ok === false, r.json);
  r = await call('POST', '/api/net/tcp', { host: 'example.com', port: 70000 });
  check('tcp bad port: 400', r.status === 400, r.status);
  r = await call('POST', '/api/net/tcp', { host: 'example.com', port: 80 });
  check('POST /api/net/tcp', r.status === 200 && typeof r.json?.ok === 'boolean' && typeof r.json.ms === 'number', r.json);
  await busyCheck('net', 'Wi-Fi Analyzer', 'GET', '/api/net/channels');
  r = await call('GET', '/api/net/channels');
  check('GET /api/net/channels', r.status === 200 && typeof r.json?.best === 'number' && Array.isArray(r.json.channels) &&
    r.json.channels.length >= 11 && r.json.channels.every((c) => typeof c.channel === 'number' && typeof c.count === 'number' && typeof c.score === 'number') &&
    Array.isArray(r.json.aps), r.json);
}

if (caps.includes('ota')) {
  console.log('ota');
  r = await call('GET', '/api/ota');
  check('GET /api/ota', r.status === 200 && typeof r.json?.slot === 'string' && typeof r.json.other_slot === 'string' &&
    r.json.state === 'idle' && typeof r.json.can_rollback === 'boolean' && r.json.progress, r.json);
  r = await call('POST', '/api/ota/commit', {});
  check('ota commit with nothing uploaded: 409', r.status === 409, r.status);
  const fw = Buffer.alloc(4096, 0xA5);
  r = await call('POST', '/api/ota/upload', fw, { raw: true });
  check('POST /api/ota/upload', r.status === 200 && r.json?.size === 4096, r.json);
  r = await call('GET', '/api/ota');
  check('ota ready after upload', r.json?.state === 'ready', r.json);
  r = await call('POST', '/api/ota/upload', Buffer.alloc(3), { raw: true });
  check('ota upload a too-small image: 400 bad_image', r.status === 400 && r.json?.error === 'bad_image', r.json);
  // commit: the mock/sim must not reboot; it logs and resets to idle
  r = await call('POST', '/api/ota/upload', fw, { raw: true });
  check('ota re-upload to ready', r.status === 200, r.json);
  r = await call('POST', '/api/ota/commit', {});
  check('POST /api/ota/commit', r.status === 200 && r.json?.ok, r.json);
  r = await call('POST', '/api/ota/rollback', {});
  check('POST /api/ota/rollback', r.status === 200 || r.status === 409, r.status);
}

// ---- P9 security tools (simulator/mock dev hooks drive deterministic hardware fakes) ----
if (DEV) {
  console.log('P9 security tools');
  // Start from a clean slate even if an earlier interrupted run left a tool active.
  for (const p of ['/api/sniffer/stop', '/api/airspace/stop', '/api/sentinel/stop', '/api/presence/stop', '/api/presence/csi/stop', '/api/evilportal/stop', '/api/deauthflood/stop', '/api/beaconspam/stop']) await call('POST', p, {});

  console.log('sniffer');
  r = await call('POST', '/api/sniffer/start', { channel: 0 });
  check('sniffer: invalid channel 400', r.status === 400 && r.json?.error === 'bad_request', r.json);
  r = await call('POST', '/api/sniffer/start', { channel: -1, max_bytes: 200 });
  check('sniffer starts on SD', r.status === 200 && r.json?.ok === true, r.json);
  r = await call('GET', '/api/sniffer');
  const capturePath = r.json?.file;
  check('sniffer reports active capture and path', r.json?.active === true && /^\/sd\/.*\.pcap$/.test(capturePath || ''), r.json);
  r = await call('POST', '/api/_dev/sniffer', { count: 3 });
  check('sniffer dev hook is reachable', r.status === 200, r.json);
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer stops at max_bytes with a useful error', r.json?.active === false && /size limit/i.test(r.json?.error || ''), r.json);
  check('sniffer max_bytes event', msgs.some(m => m.t === 'sniffer' && m.d?.active === false && m.d?.reason === 'max_bytes'), msgs.filter(m => m.t === 'sniffer').slice(-3));
  const capResp = await fetch(BASE + `/api/files/content?path=${q(capturePath)}`, { headers: { cookie } });
  const capBytes = Buffer.from(await capResp.arrayBuffer());
  check('PCAP file is readable', capResp.status === 200 && capBytes.length >= 24 + 16 + 13 + 36, { status: capResp.status, size: capBytes.length });
  check('PCAP global header and radiotap fields valid', capBytes.readUInt32LE(0) === 0xa1b2c3d4 && capBytes.readUInt32LE(20) === 127 &&
    capBytes[42] === 13 && capBytes[44] === 0x28 && capBytes.readUInt16LE(48) === 2412,
    capBytes.length >= 50 ? { magic: capBytes.readUInt32LE(0).toString(16), link: capBytes.readUInt32LE(20), rtLen: capBytes[42], present: capBytes[44], freq: capBytes.readUInt16LE(48) } : null);
  r = await call('POST', '/api/sniffer/stop', {});
  check('sniffer stop after auto-stop is a clean conflict', r.status === 409 && r.json?.error === 'conflict', r.json);

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'bogus' });
  check('sniffer rejects an unknown filter', r.status === 400 && r.json?.error === 'bad_request', r.json);
  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'beacons' });
  check('sniffer beacon filter starts', r.status === 200 && r.json?.ok === true, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'probe', count: 2 });
  await call('POST', '/api/_dev/sniffer', { kind: 'data', count: 1 });
  await call('POST', '/api/_dev/sniffer', { kind: 'beacon', count: 3 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer filter saves only selected frame types', r.json?.filter === 'beacons' && r.json.packets === 3 && r.json.seen === 6, r.json);
  check('sniffer breaks down management and data counters', r.json.management === 5 && r.json.data === 1 && r.json.probes === 2 && r.json.beacons === 3, r.json);
  check('sniffer lists AP metadata', r.json.aps?.length === 1 && r.json.aps[0].ssid === 'TestNet' && r.json.aps[0].bssid === '02:00:00:00:00:01' && r.json.aps[0].beacons === 3, r.json.aps);
  await call('POST', '/api/sniffer/stop', {});

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'eapol' });
  check('sniffer EAPOL filter starts', r.status === 200, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'beacon', count: 2 });
  for (const msg of [1, 2, 3, 4]) await call('POST', '/api/_dev/sniffer', { kind: 'eapol', msg, count: 1 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer EAPOL-only capture', r.json.filter === 'eapol' && r.json.packets === 4 && r.json.eapol === 4 && r.json.seen === 6, r.json);
  check('sniffer detects a complete four-way handshake', r.json.handshakes === 1, r.json);
  await call('POST', '/api/sniffer/stop', {});

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'probes' });
  check('sniffer probe filter starts', r.status === 200, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'beacon', count: 1 });
  await call('POST', '/api/_dev/sniffer', { kind: 'probe', count: 4 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer probes-only capture', r.json.filter === 'probes' && r.json.packets === 4 && r.json.probes === 4 && r.json.seen === 5, r.json);
  await call('POST', '/api/sniffer/stop', {});

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'management' });
  check('sniffer management filter starts', r.status === 200, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'data', count: 1 });
  await call('POST', '/api/_dev/sniffer', { kind: 'deauth', count: 2 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer management-only capture', r.json.filter === 'management' && r.json.packets === 2 && r.json.deauth === 2 && r.json.seen === 3, r.json);
  await call('POST', '/api/sniffer/stop', {});

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'data' });
  check('sniffer data filter starts', r.status === 200, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'beacon', count: 2 });
  await call('POST', '/api/_dev/sniffer', { kind: 'data', count: 3 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer data-only capture', r.json.filter === 'data' && r.json.packets === 3 && r.json.data === 3 && r.json.seen === 5, r.json);
  await call('POST', '/api/sniffer/stop', {});

  r = await call('POST', '/api/sniffer/start', { channel: 1, filter: 'all' });
  check('sniffer all-frame filter starts', r.status === 200, r.json);
  await call('POST', '/api/_dev/sniffer', { kind: 'beacon', count: 1 });
  await call('POST', '/api/_dev/sniffer', { kind: 'data', count: 1 });
  await new Promise(x => setTimeout(x, 150));
  r = await call('GET', '/api/sniffer');
  check('sniffer all-frame capture', r.json.filter === 'all' && r.json.packets === 2 && r.json.seen === 2, r.json);
  await call('POST', '/api/sniffer/stop', {});

  console.log('airspace');
  r = await call('POST', '/api/airspace/start', {});
  check('airspace starts', r.status === 200, r.json);
  if (caps.includes('desktop.install')) {
    r = await call('POST', '/api/sniffer/start', { channel: 1 });
    check('radio owner rejects sniffer while Airspace is active', r.status === 503 && r.json?.error === 'busy' && /Airspace/.test(r.json?.message || ''), r.json);
  }
  r = await call('POST', '/api/_dev/airspace', { type: 'deauth_flood', detail: 'Lab flood', bssid: '02:00:00:00:00:01', rssi: -40 });
  check('airspace dev alert accepted', r.status === 200, r.json);
  r = await call('GET', '/api/airspace');
  check('airspace status uses documented counters', r.json?.active === true && r.json.deauth_floods === 1 && r.json.deauth_total >= 50 && typeof r.json.beacon_total === 'number', r.json);
  check('airspace alert event has detail', msgs.some(m => m.t === 'airspace' && m.d?.type === 'deauth_flood' && m.d?.detail === 'Lab flood'), msgs.filter(m => m.t === 'airspace').slice(-2));
  r = await call('POST', '/api/airspace/stop', {});
  check('airspace stops', r.status === 200, r.json);

  console.log('sentinel and nearby peers');
  await call('POST', '/api/sentinel/start', {});
  r = await call('POST', '/api/_dev/sentinel', { addr: '02:00:00:00:00:0a', kind: 'Find My', rssi: -55, age_s: 300, sightings: 4 });
  check('sentinel dev hook is reachable', r.status === 200, r.json);
  await new Promise(x => setTimeout(x, 200));
  r = await call('GET', '/api/sentinel');
  const tracked = r.json?.devices?.[0];
  check('sentinel reports tracker kind and persistence', r.json?.device_count === 1 && tracked?.kind === 'Find My' && tracked.sightings === 4 && tracked.stalking === true, tracked);
  check('sentinel persistent alert event', msgs.some(m => m.t === 'sentinel' && m.d?.type === 'stalking' && m.d?.kind === 'Find My'), msgs.filter(m => m.t === 'sentinel').slice(-2));
  await call('POST', '/api/sentinel/stop', {});
  if (caps.includes('presence.csi')) {
    console.log('CSI presence');
    r = await call('POST', '/api/presence/csi/start', { threshold: 2 });
    check('CSI presence validates threshold', r.status === 400 && r.json?.error === 'bad_request', r.json);
    r = await call('POST', '/api/presence/csi/start', { threshold: 0.35 });
    check('CSI presence starts calibration', r.status === 200, r.json);
    r = await call('GET', '/api/presence');
    check('CSI status is nested without breaking peers', r.json?.csi?.active === true && r.json.csi.calibrating === true && Array.isArray(r.json.peers), r.json);
    if (caps.includes('desktop.install')) {
      r = await call('POST', '/api/sniffer/start', { channel: 1 });
      check('CSI radio owner rejects sniffer', r.status === 503 && r.json?.error === 'busy' && /Presence/.test(r.json?.message || ''), r.json);
    }
    r = await call('POST', '/api/_dev/presence-csi', { pattern: 'invalid', count: 4 });
    check('CSI dev hook validates patterns', r.status === 400, r.json);
    r = await call('POST', '/api/_dev/presence-csi', { pattern: 'stable', count: 64 });
    check('CSI stable frames feed detector', r.status === 200, r.json);
    r = await call('GET', '/api/presence');
    check('CSI stable room finishes calibration clear', r.json?.csi?.calibrating === false && r.json.csi.present === false && r.json.csi.motion === false && r.json.csi.samples >= 64, r.json?.csi);
    check('CSI calibration event published', msgs.some(m => m.t === 'presence.csi' && m.d?.event === 'calibrated'), msgs.filter(m => m.t === 'presence.csi').slice(-3));
    r = await call('POST', '/api/_dev/presence-csi', { pattern: 'motion', count: 64 });
    check('CSI motion frames feed detector', r.status === 200, r.json);
    r = await call('GET', '/api/presence');
    check('CSI changing multipath detects motion and presence', r.json?.csi?.motion === true && r.json.csi.present === true && r.json.csi.score >= r.json.csi.threshold, r.json?.csi);
    check('CSI motion event includes score', msgs.some(m => m.t === 'presence.csi' && m.d?.event === 'motion' && m.d?.present === true && m.d?.score >= 0.35), msgs.filter(m => m.t === 'presence.csi').slice(-4));
    await call('POST', '/api/_dev/presence-csi', { pattern: 'stable', count: 64 });
    r = await call('GET', '/api/presence');
    check('CSI presence holds after motion stops', r.json?.csi?.motion === false && r.json.csi.present === true, r.json?.csi);
    await call('POST', '/api/_dev/presence-csi', { count: 0, advance_ms: 11000 });
    r = await call('GET', '/api/presence');
    check('CSI presence hold expires', r.json?.csi?.present === false && r.json.csi.state === 'clear', r.json?.csi);
    r = await call('POST', '/api/presence/csi/calibrate', {});
    check('CSI explicit recalibration resets state', r.status === 200, r.json);
    r = await call('GET', '/api/presence');
    check('CSI recalibration is visible', r.json?.csi?.calibrating === true && r.json.csi.present === false, r.json?.csi);
    r = await call('POST', '/api/presence/csi/stop', {});
    check('CSI sensing stops', r.status === 200, r.json);
  }
  console.log('nearby peers');
  await call('POST', '/api/presence/start', {});
  r = await call('POST', '/api/_dev/presence', { mac: '02:00:00:00:00:0b', name: 'Lab Watch', rssi: -51 });
  if (caps.includes('desktop.install')) check('presence dev hook is reachable', r.status === 200, r.json);
  else {
    // The mock uses the same hook; this is a normal capability check, not an alternate implementation.
    check('presence dev hook is reachable', r.status === 200, r.json);
  }
  r = await call('GET', '/api/presence');
  check('nearby peers include arrival history', r.json?.peers?.[0]?.name === 'Lab Watch' && r.json.peers[0].present === true && r.json.history?.some(x => x.event === 'appeared'), r.json);
  check('presence event identifies peer', msgs.some(m => m.t === 'presence' && m.d?.event === 'appeared' && m.d?.mac === '02:00:00:00:00:0b'), msgs.filter(m => m.t === 'presence').slice(-2));
  await call('POST', '/api/presence/stop', {});

  console.log('FIDO2');
  await call('POST', '/api/_dev/fido2', { available: false });
  r = await call('POST', '/api/fido2/register', { rp: 'example.com', user: 'alice' });
  check('FIDO2 unavailable never fakes registration', r.status === 503 && r.json?.error === 'fido_unavailable', r.json);
  await call('POST', '/api/_dev/fido2', { available: true });
  r = await call('POST', '/api/fido2/register', { rp: '', user: '' });
  check('FIDO2 validates relying party and user', r.status === 400, r.json);
  r = await call('POST', '/api/fido2/register', { rp: 'example.com', user: 'alice' });
  check('FIDO2 simulator registers when transport enabled', r.status === 200, r.json);
  r = await call('POST', '/api/fido2/authenticate', { rp: 'other.example' });
  check('FIDO2 unknown relying party 404', r.status === 404, r.json);
  r = await call('POST', '/api/fido2/authenticate', { rp: 'example.com' });
  check('FIDO2 simulator authenticates known RP', r.status === 200, r.json);
  r = await call('GET', '/api/fido2/credentials');
  check('FIDO2 credential list has stable index', r.json?.credentials?.[0]?.index === 0 && r.json.credentials[0].rp === 'example.com', r.json);
  r = await call('POST', '/api/fido2/remove', { index: 0 });
  check('FIDO2 credential removal works', r.status === 200, r.json);
  await call('POST', '/api/_dev/fido2', { available: false });

  console.log('evil portal');
  const portalDir = '/sd/data/evilportal/portals';
  const portalLab = portalDir + '/p9-api-lab.html', portalBad = portalDir + '/p9-api-bad.html', portalUnsafe = portalDir + '/p9-api-unsafe.html', portalClone = portalDir + '/cloned.html';
  await call('DELETE', `/api/files?path=${q(portalLab)}`);
  await call('DELETE', `/api/files?path=${q(portalBad)}`);
  await call('DELETE', `/api/files?path=${q(portalUnsafe)}`);
  await call('DELETE', `/api/files?path=${q(portalClone)}`);
  const customHtml = '<!doctype html><html><head><title>Lab custom</title></head><body><h1>SD-TEMPLATE-MARKER</h1><form method="post" action="/login"><input name="username"><input type="password" name="password"><button>Connect</button></form></body></html>';
  r = await call('PUT', `/api/files/content?path=${q(portalLab)}`, customHtml, { raw: true });
  check('custom portal HTML uploads to fixed SD folder', r.status === 200, r.json);
  await call('PUT', `/api/files/content?path=${q(portalBad)}`, '<html><script>alert(1)</script></html>', { raw: true });
  await call('PUT', `/api/files/content?path=${q(portalUnsafe)}`, customHtml + '<form method="post" action="//evil.example/login"><input name="username"><input name="password"></form>', { raw: true });
  r = await call('PUT', `/api/files/content?path=${q(portalDir + '/../escape.html')}`, customHtml, { raw: true });
  check('portal template path traversal fails closed', r.status === 400, r.json);
  r = await call('POST', '/api/evilportal/templates/reload', {});
  check('portal reload accepts valid template and reports malformed file', r.status === 200 && r.json?.count >= 1 && r.json?.rejected >= 2, r.json);
  r = await call('GET', '/api/evilportal');
  check('protocol-relative external form fails closed', !r.json?.templates?.some(t => t.name === 'p9-api-unsafe'), r.json?.templates);
  const customTemplate = r.json?.templates?.find(t => t.builtin === false && t.name === 'p9-api-lab');
  check('SD template catalog survives reload with stable custom ID', customTemplate?.id >= 100 && r.json?.templateError, r.json?.templates);
  r = await call('POST', '/api/evilportal/start', { ssid: 'Custom Lab', templateId: customTemplate?.id, authorized: true });
  check('custom SD template starts', r.status === 200, r.json);
  r = await call('GET', '/template-check');
  check('captive route serves selected SD HTML', r.status === 200 && /SD-TEMPLATE-MARKER/.test(r.text), r.text.slice(0, 100));
  await call('POST', '/api/evilportal/stop', {});

  r = await call('POST', '/api/evilportal/start', { ssid: 'Lab AP', templateId: 0 });
  check('portal requires explicit authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
  r = await call('POST', '/api/evilportal/start', { ssid: 'Lab AP', gateway: 'not-an-ip', authorized: true });
  check('portal rejects invalid gateway', r.status === 400 && /IPv4/i.test(r.json?.message || ''), r.json);
  const quotedSsid = 'Lab "AP" \\ test';
  r = await call('POST', '/api/evilportal/start', { ssid: quotedSsid, templateId: 0, authorized: true });
  check('portal accepts numeric template and escapes SSID JSON', r.status === 200 && r.json?.ssid === quotedSsid, r.json || r.text);
  r = await call('POST', '/api/evilportal/start', { ssid: 'second', authorized: true });
  check('portal second start conflicts', r.status === 409, r.json);
  r = await fetch(BASE + '/login', { method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: 'username=alice&password=secret' });
  const captiveText = await r.text();
  check('captive login works without operator session', r.status === 200 && /Connected/.test(captiveText), { status: r.status, body: captiveText.slice(0, 80) });
  r = await call('GET', '/api/evilportal/captures');
  check('captured password hidden by default', r.json?.captures?.some(c => c.username === 'alice' && c.password === '******'), r.json);
  r = await call('PUT', '/api/evilportal/settings', { pwdMode: 1 });
  check('portal redaction setting applies', r.status === 200, r.json);
  r = await call('GET', '/api/evilportal/captures');
  check('masked capture preserves only first and last character', r.json?.captures?.some(c => c.username === 'alice' && c.password === 's****t'), r.json);
  r = await call('PUT', '/api/evilportal/settings', { verify: true });
  check('PSK verification setting is available', r.status === 200, r.json);
  r = await call('POST', '/api/evilportal/clone', { ssid: 'Open AP' });
  check('page cloning requires explicit authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
  await call('POST', '/api/evilportal/clear', {});
  r = await call('GET', '/api/evilportal/captures');
  check('portal captures clear', r.json?.captures?.length === 0, r.json);
  await call('POST', '/api/evilportal/stop', {});

  r = await call('POST', '/api/evilportal/clone', { ssid: 'Open Lab AP', authorized: true });
  check('safe page cloning starts asynchronously', r.status === 202 && r.json?.cloning === true, r.json);
  if (DEV) {
    r = await call('POST', '/api/_dev/evilportal', { action: 'clone', ok: true });
    check('simulator completes authorized page clone', r.status === 200, r.json);
    r = await call('GET', '/api/evilportal');
    check('clone saves bounded SD template and reloads catalog', r.json?.cloning === false && r.json?.cloneName === 'cloned.html' && r.json?.cloneBytes > 0 && r.json?.templates?.some(t => t.name === 'cloned'), r.json);
  } else {
    r = await call('GET', '/api/evilportal');
    check('watch clone exposes in-progress state', r.status === 200 && r.json?.cloning === true, r.json);
    await call('POST', '/api/evilportal/stop', {});
  }
  if (DEV) {
    await call('POST', '/api/evilportal/clone', { ssid: 'Unsafe Lab AP', authorized: true });
    r = await call('POST', '/api/_dev/evilportal', { action: 'clone', ok: false });
    check('clone failure injection completes without success', r.status === 200, r.json);
    r = await call('GET', '/api/evilportal');
    check('failed clone remains explicit and cancellable', r.json?.cloning === false && /failed/i.test(r.json?.error || ''), r.json);
  }

  await call('PUT', '/api/evilportal/settings', { pwdMode: 2, verify: true });
  r = await call('POST', '/api/evilportal/start-twin', { ssid: 'WPA2 Lab', bssid: '02:00:00:00:00:60', channel: 6, coherent: true, authorized: true });
  check('coherent twin requires known lab PSK', r.status === 400 && /PSK/.test(r.json?.message || ''), r.json);
  r = await call('POST', '/api/evilportal/start-twin', { ssid: 'WPA2 Lab', bssid: '02:00:00:00:00:60', channel: 6, coherent: true, psk: 'lab-password', authorized: true });
  check('WPA2-coherent twin starts with supplied lab PSK', r.status === 200, r.json);
  r = await call('GET', '/api/evilportal');
  check('twin status reports coherent security', r.json?.active === true && r.json?.coherent === true, r.json);
  if (DEV) {
    await call('POST', '/api/_dev/evilportal', { action: 'creds', username: 'good', password: 'lab-password' });
    let verification = null;
    for (let i = 0; i < 20; i++) {
      await new Promise(x => setTimeout(x, 100));
      verification = (await call('GET', '/api/evilportal/captures')).json?.captures?.at(-1)?.verification;
      if (verification === 'checking') break;
    }
    check('captured PSK enters verification queue', verification === 'checking', verification);
    await call('POST', '/api/_dev/evilportal', { action: 'verify', valid: true });
    await call('POST', '/api/_dev/evilportal', { action: 'creds', username: 'bad', password: 'wrong-password' });
    for (let i = 0; i < 20; i++) {
      await new Promise(x => setTimeout(x, 100));
      verification = (await call('GET', '/api/evilportal/captures')).json?.captures?.at(-1)?.verification;
      if (verification === 'checking') break;
    }
    await call('POST', '/api/_dev/evilportal', { action: 'verify', valid: false });
    r = await call('GET', '/api/evilportal/captures');
    check('per-capture PSK results distinguish valid and invalid', r.json?.captures?.[0]?.verification === 'valid' && r.json?.captures?.[1]?.verification === 'invalid' && r.json.captures.every(c => c.password === '******'), r.json);
    r = await call('GET', '/api/evilportal');
    check('PSK verification counters agree with capture results', r.json?.settings?.verifiedOk === 1 && r.json?.settings?.verifiedBad === 1, r.json?.settings);
  }
  await call('POST', '/api/evilportal/clear', {});
  await call('POST', '/api/evilportal/stop', {});

  r = await call('POST', '/api/evilportal/karma-scan', { seconds: 1 });
  check('probe discovery requires explicit authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
  r = await call('POST', '/api/evilportal/karma-scan', { seconds: 0, authorized: true });
  check('probe discovery validates duration', r.status === 400, r.json);
  r = await call('POST', '/api/evilportal/karma-scan', { seconds: 10, authorized: true });
  check('passive probe discovery starts asynchronously', r.status === 202 && r.json?.scanning === true, r.json);
  if (DEV) {
    await call('POST', '/api/_dev/evilportal', { action: 'probe', ssid: 'Lab Hidden', mac: '02:00:00:00:00:21', hits: 2, rssi: -48, channel: 6 });
    await call('POST', '/api/_dev/evilportal', { action: 'probe', ssid: 'Lab Hidden', mac: '02:00:00:00:00:22', hits: 1, rssi: -51, channel: 6 });
    r = await call('GET', '/api/evilportal/karma');
    check('probe results aggregate hits and unique clients', r.json?.probes?.[0]?.ssid === 'Lab Hidden' && r.json.probes[0].hits === 3 && r.json.probes[0].clients === 2 && r.json.probes[0].channel === 6, r.json);
    r = await call('POST', '/api/evilportal/start-karma', { ssid: 'Lab Hidden', templateId: 0, authorized: true });
    check('KARMA core starts selected discovered SSID', r.status === 200 && r.json?.mode === 'karma' && r.json?.ssid === 'Lab Hidden', r.json);
    r = await call('GET', '/api/evilportal');
    check('KARMA active state exposes mode and SSID', r.json?.active === true && r.json?.mode === 2 && r.json?.ssid === 'Lab Hidden' && r.json?.karmaScanning === false, r.json);
    await call('POST', '/api/evilportal/stop', {});
    r = await call('POST', '/api/_dev/evilportal', { action: 'probe', ssid: 'Outside Scan' });
    check('probe injection refused after discovery stops', r.status === 409, r.json);
  } else {
    r = await call('GET', '/api/evilportal/karma');
    check('watch probe discovery exposes scanning state', r.status === 200 && r.json?.scanning === true, r.json);
    r = await call('POST', '/api/evilportal/stop', {});
    check('watch probe discovery stops cleanly', r.status === 200, r.json);
  }
  await call('DELETE', `/api/files?path=${q(portalLab)}`);
  await call('DELETE', `/api/files?path=${q(portalBad)}`);
  await call('DELETE', `/api/files?path=${q(portalUnsafe)}`);
  await call('DELETE', `/api/files?path=${q(portalClone)}`);
  await call('POST', '/api/evilportal/templates/reload', {});

  console.log('deauth flood');
  r = await call('POST', '/api/deauthflood/scan', {});
  check('deauth scan is asynchronous 202', r.status === 202, r.json);
  let targets = null;
  for (let i = 0; i < 30; i++) {
    await new Promise(x => setTimeout(x, 200));
    targets = (await call('GET', '/api/deauthflood/targets')).json;
    if (!targets?.scanning) break;
  }
  check('deauth target scan completes with usable APs', targets?.scanning === false && targets.targets?.length > 0 && targets.targets[0].bssid, targets);
  r = await call('POST', '/api/deauthflood/start', { bssid: targets.targets[0].bssid, channel: targets.targets[0].channel });
  check('deauth requires authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
  r = await call('POST', '/api/deauthflood/start', { bssid: targets.targets[0].bssid, channel: 14, authorized: true });
  check('deauth validates channel', r.status === 400, r.json);
  r = await call('POST', '/api/deauthflood/start', { bssid: targets.targets[0].bssid, channel: targets.targets[0].channel, rate: 10, authorized: true });
  check('deauth starts with selected target', r.status === 200, r.json);
  r = await call('GET', '/api/deauthflood');
  check('deauth status uses camelCase contract and packet rate', r.json?.active === true && r.json.targetBssid === targets.targets[0].bssid && r.json.ratePps === 10, r.json);
  await call('POST', '/api/deauthflood/stop', {});

  if (DEV) {
    const normalTarget = targets.targets.find(t => t.channel >= 1 && t.channel <= 13 && !t.auth.includes('wpa3'));
    const pmfTarget = targets.targets.find(t => t.channel >= 1 && t.channel <= 13 && t.auth.includes('wpa3'));
    const campaignTargets = [normalTarget.bssid, pmfTarget.bssid];
    r = await call('POST', '/api/deauthflood/campaign/start', { targets: campaignTargets, frames: 2, rate: 100 });
    check('deauth campaign requires authorization for every target', r.status === 400 && r.json?.error === 'authorization_required', r.json);
    r = await call('POST', '/api/deauthflood/campaign/start', { targets: [campaignTargets[0], campaignTargets[0]], frames: 2, rate: 100, authorized: true });
    check('deauth campaign rejects duplicate targets', r.status === 400, r.json);
    r = await call('POST', '/api/deauthflood/campaign/start', { targets: campaignTargets, frames: 10000, rate: 1, authorized: true });
    check('multi-target deauth campaign starts', r.status === 200 && r.json?.active === true, r.json);
    await call('POST', '/api/_dev/deauthflood', { action: 'fail-target' });
    r = await call('GET', '/api/deauthflood');
    check('failed target skips independently with health counters', r.json?.progress?.[0]?.state === 'error' && r.json.progress[0].failed === 1 && r.json.progress[1].state === 'running' && r.json.txErrors === 1, r.json);
    await call('POST', '/api/deauthflood/stop', {});
    r = await call('POST', '/api/deauthflood/campaign/start', { targets: campaignTargets, frames: 2, rate: 100, authorized: true });
    let campaign = null;
    for (let i = 0; i < 100; i++) {
      await new Promise(x => setTimeout(x, 100));
      campaign = (await call('GET', '/api/deauthflood')).json;
      if (!campaign?.active) break;
    }
    check('all deauth targets finish independently', campaign?.campaign?.completed === 2 && campaign.progress?.[0]?.state === 'done' && campaign.progress[0].accepted === 2 && campaign.progress[1].accepted === 2, campaign);
    check('PMF target reports limited outcome not guaranteed disconnect', campaign?.progress?.[1]?.state === 'pmf_limited' && campaign.progress[1].pmfLimited === true, campaign?.progress?.[1]);
    check('deauth aggregate and per-target counters agree', campaign?.sent === campaign.progress.reduce((n, t) => n + t.accepted, 0) && campaign.txErrors === campaign.progress.reduce((n, t) => n + t.failed, 0), campaign);
    check('deauth completion reports reconnect metrics', campaign?.reconnect?.reconnected === true && campaign.reconnect.attempts >= 1 && campaign.reconnect.elapsedMs >= 0, campaign?.reconnect);
  }

  console.log('beacon spam');
  r = await call('PUT', '/api/beaconspam/ssids', '["ok","bad\\q"]', { raw: true });
  check('beacon custom JSON parser rejects bad escapes', r.status === 400, r.json);
  r = await call('PUT', '/api/beaconspam/ssids', ['Lab"Net', 'Test\\u0041']);
  check('beacon custom list accepts escaped SSIDs', r.status === 200 && r.json?.count === 2, r.json);
  r = await call('POST', '/api/beaconspam/start', { mode: 'custom', channel: 1 });
  check('beacon requires authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
  r = await call('POST', '/api/beaconspam/start', { mode: 'custom', channel: 1, authorized: true });
  check('beacon custom mode starts', r.status === 200, r.json);
  r = await call('GET', '/api/beaconspam');
  check('beacon reports custom mode, SSID count and errors', r.json?.active === true && r.json.mode === 2 && r.json.ssids === 2 && r.json.customSsids?.[0] === 'Lab"Net' && typeof r.json.txErrors === 'number', r.json);
  await call('POST', '/api/beaconspam/stop', {});
  if (DEV) {
    r = await call('POST', '/api/beaconspam/campaign/start', { mode: 'custom', channel: 1, framesPerSsid: 2 });
    check('beacon campaign requires explicit authorization', r.status === 400 && r.json?.error === 'authorization_required', r.json);
    r = await call('POST', '/api/beaconspam/campaign/start', { mode: 'generated', prefix: 'Lab', count: 65, channel: 1, framesPerSsid: 2, authorized: true });
    check('beacon campaign enforces bounded 64-SSID progress', r.status === 400, r.json);
    r = await call('POST', '/api/beaconspam/campaign/start', { mode: 'custom', channel: 1, framesPerSsid: 10000, authorized: true });
    check('custom beacon campaign starts with per-SSID goals', r.status === 200 && r.json?.active === true, r.json);
    await call('POST', '/api/_dev/beaconspam', { action: 'fail-target' });
    r = await call('GET', '/api/beaconspam');
    check('failed beacon SSID skips independently', r.json?.progress?.[0]?.state === 'error' && r.json.progress[0].failed === 1 && r.json.progress[1].state === 'running' && r.json.txErrors === 1, r.json);
    await call('POST', '/api/beaconspam/stop', {});
    r = await call('POST', '/api/beaconspam/campaign/start', { mode: 'custom', channel: 1, framesPerSsid: 2, authorized: true });
    let campaign = null;
    for (let i = 0; i < 100; i++) {
      await new Promise(x => setTimeout(x, 100));
      campaign = (await call('GET', '/api/beaconspam')).json;
      if (!campaign?.active) break;
    }
    check('beacon campaign completes every custom SSID', campaign?.campaign?.completed === 2 && campaign.progress?.every(t => t.state === 'done' && t.accepted === 2), campaign);
    check('beacon aggregate and per-SSID counters agree', campaign?.beacons === campaign.progress.reduce((n, t) => n + t.accepted, 0) && campaign.txErrors === campaign.progress.reduce((n, t) => n + t.failed, 0), campaign);
    check('beacon completion reports reconnect metrics', campaign?.reconnect?.reconnected === true && campaign.reconnect.attempts >= 1 && campaign.reconnect.elapsedMs >= 0, campaign?.reconnect);
  }
  await call('POST', '/api/_dev/fido2', { available: false });
}

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

// ---- pairing window (simulator and mock: the hook stands in for leaving the watch's Web page)
if (DEV) {
  console.log('pairing window');
  await call('POST', '/api/_dev/pairing', { open: false });
  r = await call('POST', '/api/auth/pair', { pin: PIN });
  check('pairing closed: 403 pairing_closed', r.status === 403 && r.json?.error === 'pairing_closed', r.json);
  const again = (await call('GET', '/api/_dev/pin')).json?.pin;
  check('reopening pairing makes a new PIN', again && again !== PIN, { before: PIN, after: again });
}

r = await call('POST', '/api/auth/logout', {});
check('logout', r.status === 200 && r.json?.revoked === 1, r.json);
r = await call('GET', '/api/settings');
check('logged out: 401', r.status === 401, r.status);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
