#!/usr/bin/env node
// The watch installs its web desktop by itself (docs/web-roadmap.md, "Getting the desktop onto the card"):
// from a ZIP on the card, and by downloading the ZIP the firmware pins. Runs against the simulator, with
// the mock watch as the release server:
//
//   node web_mock_device.mjs --dev --dl /tmp/dl &                    (release server on :5620)
//   TWATCH_SIM_WEB_PORT=8090 TWATCH_SIM_DESKTOP_URL=http://localhost:5620/dl <simulator> &
//   node web_desktop_test.mjs --url http://localhost:8090 --release http://localhost:5620 --dl /tmp/dl
//
// It empties the simulated card's web/ and data/ and leaves the current desktop installed at the end.
// Needs Node 18+ and python3 (it builds the ZIPs with web_build.py).

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, readdirSync, rmSync, copyFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const BASE = opt('--url', 'http://localhost:8090').replace(/\/$/, '');
const RELEASE = opt('--release', 'http://localhost:5620').replace(/\/$/, '');
const DL = opt('--dl');
if (!DL) { console.log('--dl <dir>: the folder the mock serves at /dl'); process.exit(2); }
const TOOL = join(dirname(fileURLToPath(import.meta.url)), 'web_build.py');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail) => {
  if (ok) { passed++; console.log(`  ok    ${name}`); }
  else { failed++; console.log(`  FAIL  ${name}${detail !== undefined ? `  -> ${JSON.stringify(detail)}` : ''}`); }
};

for (let i = 0; ; i++) {
  try { if ((await fetch(`${BASE}/api/caps`)).ok && (await fetch(`${RELEASE}/api/caps`)).ok) break; } catch {}
  if (i >= 60) { console.log(`no server at ${BASE} or ${RELEASE}`); process.exit(2); }
  await sleep(500);
}
const pin = (await (await fetch(`${BASE}/api/_dev/pin`)).json()).pin;
const pr = await fetch(`${BASE}/api/auth/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
const cookie = (pr.headers.get('set-cookie') || '').split(';')[0];
const call = async (method, path, body, raw) => {
  const r = await fetch(BASE + path, { method, headers: { cookie, ...(raw ? {} : { 'content-type': 'application/json' }) }, body: raw ? body : body && JSON.stringify(body) });
  const text = await r.text();
  let json = null; try { json = JSON.parse(text); } catch {}
  return { status: r.status, json, text };
};
const rel = (path, body) => fetch(RELEASE + path, { method: body ? 'POST' : 'GET', headers: { 'content-type': 'application/json' }, body: body && JSON.stringify(body) }).then((r) => r.json());
const q = encodeURIComponent;

// ---- the ZIPs
const tmp = mkdtempSync(join(tmpdir(), 'desktop-test-'));
const zipOf = (version) => {
  const dir = join(tmp, version || 'pinned');
  execFileSync('python3', [TOOL, '--zip', dir, ...(version ? ['--version', version] : [])]);
  const name = readdirSync(dir)[0];
  return { name, path: join(dir, name), data: readFileSync(join(dir, name)) };
};
const pinned = zipOf();
const older = zipOf('0.0.1');
const newer = zipOf('9.9.9');
const damaged = zipOf('9.9.10');
{ // flip a byte inside web/js/shell.js's data: its CRC fails
  const d = damaged.data, name = Buffer.from('web/js/shell.js');
  const at = d.indexOf(Buffer.concat([Buffer.from([0x50, 0x4b, 0x03, 0x04])]), 0);
  let i = -1;
  for (let p = at; p >= 0 && p < d.length; p = d.indexOf(Buffer.from([0x50, 0x4b, 0x03, 0x04]), p + 4)) {
    if (d.subarray(p + 30, p + 30 + d.readUInt16LE(p + 26)).equals(name)) { i = p; break; }
  }
  const dataAt = i + 30 + d.readUInt16LE(i + 26) + d.readUInt16LE(i + 28);
  d[dataAt + 100] ^= 0xff;
}
// The same desktop as Finder would pack it: deflated, inside a folder, with __MACOSX junk.
const finder = (() => {
  const src = zipOf('9.9.11');
  const path = join(tmp, 'twatch-sd-9.9.12.zip');
  execFileSync('python3', ['-c', `
import zipfile, sys
with zipfile.ZipFile(sys.argv[1]) as a, zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as b:
    for i in a.infolist():
        b.writestr('twatch-sd/' + i.filename, a.read(i))
    b.writestr('__MACOSX/twatch-sd/._README.txt', b'junk')
`, src.path, path]);
  return { name: 'twatch-sd-9.9.12.zip', path, data: readFileSync(path) };
})();
mkdirSync(DL, { recursive: true });
copyFileSync(pinned.path, join(DL, pinned.name));

const desktop = async () => (await call('GET', '/api/desktop')).json;
// Wait for the state a step should end in (a job starts up to a second after its trigger).
const waitFor = async (pred, ms = 20000) => {
  const end = Date.now() + ms;
  let d;
  while (Date.now() < end) {
    d = await desktop();
    if (d && pred(d)) return d;
    await sleep(200);
  }
  return d;
};
// Each check the watch starts has a number (run): a step waits for a run after its trigger.
let since = 0;
const mark = async () => { since = (await desktop()).run; };
const finished = (d) => d.run > since && (d.state === 'idle' || d.state === 'error');
const done = (version) => (d) => finished(d) && d.state === 'idle' && d.installed === version;
const failedWith = (re) => (d) => finished(d) && d.state === 'error' && re.test(d.error || '');
const putZip = (z) => call('PUT', `/api/files/content?path=${q('/sd/' + z.name)}`, z.data, true);
const clearCard = async () => {
  await call('DELETE', `/api/files?path=${q('/sd/web')}&recursive=1`);
  await call('DELETE', `/api/files?path=${q('/sd/data')}&recursive=1`);
  const root = (await call('GET', `/api/files?path=${q('/sd')}`)).json?.entries || [];
  for (const e of root) if (/^twatch-sd-.*\.zip(\.part)?$/i.test(e.name)) await call('DELETE', `/api/files?path=${q('/sd/' + e.name)}`);
};
const reinsert = async () => {
  await call('POST', '/api/_dev/sd', { present: false });
  await sleep(300);
  await call('POST', '/api/_dev/sd', { present: true });
};
const servesDesktop = async () => (await (await fetch(BASE + '/')).text()).includes('js/shell.js');

try {
  const caps = (await (await fetch(`${BASE}/api/caps`)).json()).caps;
  check('watch can install its desktop (desktop.install)', caps.includes('desktop.install'), caps);
  const d0 = await desktop();
  check('GET /api/desktop says which version the firmware wants', typeof d0?.wanted === 'string', d0);
  const wanted = d0.wanted;
  const pinnedVersion = JSON.parse(readFileSync(join(dirname(fileURLToPath(import.meta.url)), '..', 'web', 'manifest.json'), 'utf8')).version;
  if (wanted !== pinnedVersion) console.log(`  note: the firmware wants ${wanted}, web/manifest.json says ${pinnedVersion}: run web_build.py --rescue and rebuild`);

  console.log('a ZIP on the card');
  await clearCard();
  await putZip(pinned);
  await mark();
  await reinsert();
  let d = await waitFor(done(wanted));
  check('card inserted with a ZIP: the watch installs it by itself', done(wanted)(d), d);
  check('/ serves the installed desktop', await servesDesktop());
  const data = (await call('GET', `/api/files?path=${q('/sd/data')}`)).json?.entries?.map((e) => e.name) || [];
  check('the ZIP made the data folders', ['ir', 'gnss', 'nfc', 'recordings', 'lora'].every((n) => data.includes(n)), data);
  check('the ZIP stays on the card', (await call('GET', `/api/files?path=${q('/sd')}`)).json?.entries?.some((e) => e.name === pinned.name));

  await call('DELETE', `/api/files?path=${q('/sd/' + pinned.name)}`);
  await putZip(older);
  await mark();
  await reinsert();
  d = await waitFor((x) => finished(x) && x.zip_on_card === older.name);
  check('an older ZIP is seen and left alone', d.zip_on_card === older.name && d.installed === wanted, d);
  await call('DELETE', `/api/files?path=${q('/sd/' + older.name)}`);

  await putZip(newer);
  await mark();
  await call('POST', '/api/desktop/install', { source: 'card' });
  d = await waitFor(done('9.9.9'));
  check('install from the card picks the newest ZIP (9.9.9)', d.installed === '9.9.9', d);

  await putZip(finder);
  await mark();
  await call('POST', '/api/desktop/install', { source: 'card' });
  d = await waitFor(done('9.9.11'));
  check('a deflated ZIP wrapped in a folder (Finder) unpacks on the watch', d.installed === '9.9.11', d);
  await call('DELETE', `/api/files?path=${q('/sd/' + finder.name)}`);
  await mark();
  await call('POST', '/api/desktop/install', { source: 'card' });
  d = await waitFor(done('9.9.9'));

  await putZip(damaged);
  await mark();
  await call('POST', '/api/desktop/install', { source: 'card' });
  d = await waitFor(failedWith(/CRC/));
  check('a damaged ZIP is refused (CRC)', failedWith(/CRC/)(d), d);
  check('…and the desktop before it stays', d.installed === '9.9.9' && await servesDesktop(), d);
  check('…and no web.new is left behind', (await call('GET', `/api/files?path=${q('/sd/web.new')}`)).status === 404);

  console.log('download');
  await clearCard();
  const logStart = (await rel('/api/_dev/dl-log')).length;
  await mark();
  let r = await call('POST', '/api/desktop/install', { source: 'download' });
  check('POST /api/desktop/install download: 202', r.status === 202, r);
  d = await waitFor(done(wanted));
  check('downloads the pinned ZIP and installs it', done(wanted)(d), d);
  check('the downloaded ZIP is kept on the card', (await call('GET', `/api/files?path=${q('/sd')}`)).json?.entries?.some((e) => e.name === pinned.name));
  check('/ serves the downloaded desktop', await servesDesktop());

  await clearCard();
  await rel('/api/_dev/dl', { cut_after: 40000 });
  await mark();
  await call('POST', '/api/desktop/install', { source: 'download' });
  d = await waitFor(failedWith(/resumes/));
  check('a download cut off halfway reports it', failedWith(/resumes/)(d), d);
  const part = (await call('GET', `/api/files?path=${q('/sd')}`)).json?.entries?.find((e) => e.name === pinned.name + '.part');
  check('…keeping what arrived (.part)', part && part.size === 40000, part);
  await mark();
  await call('POST', '/api/desktop/install', { source: 'download' });
  d = await waitFor(done(wanted));
  const log = (await rel('/api/_dev/dl-log')).slice(logStart);
  check('…and the next try resumes it with a Range request', d.installed === wanted && log.at(-1)?.range === 'bytes=40000-' && log.at(-1)?.status === 206, { d, last: log.at(-1) });

  await clearCard();
  copyFileSync(newer.path, join(DL, pinned.name));          // the right name, the wrong bytes
  await mark();
  await call('POST', '/api/desktop/install', { source: 'download' });
  d = await waitFor(failedWith(/SHA-256/));
  check('a ZIP that is not the pinned one is refused (SHA-256)', failedWith(/SHA-256/)(d) && !d.installed, d);
  check('…and deleted', !(await call('GET', `/api/files?path=${q('/sd')}`)).json?.entries?.some((e) => e.name.startsWith(pinned.name)));
  copyFileSync(pinned.path, join(DL, pinned.name));

  await rel('/api/_dev/dl', { status: 404 });
  await mark();
  await call('POST', '/api/desktop/install', { source: 'download' });
  d = await waitFor(failedWith(/404/));
  check('a release without the ZIP (404) is reported', failedWith(/404/)(d), d);

  console.log('automatic');
  await clearCard();
  await mark();
  await reinsert();
  d = await waitFor(done(wanted), 30000);
  check('empty card inserted, watch online: it downloads and installs by itself', done(wanted)(d), d);
  check('/ serves it', await servesDesktop());
} catch (e) {
  failed++;
  console.log(`  FAIL  ${e.stack || e.message}`);
} finally {
  rmSync(tmp, { recursive: true, force: true });
  await call('POST', '/api/auth/logout', {}).catch(() => {});
}
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
