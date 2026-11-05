#!/usr/bin/env node
// Clicks through the web desktop in headless Chrome against the mock watch (web_mock_device.mjs --dev)
// or the simulator: pairing, Files (open a folder, preview, rename, new folder, upload, delete, live
// refresh), Settings (brightness), SD removal, the rescue page installing a desktop, and the phone layout.
//
// Usage: node web_ui_test.mjs [--url http://localhost:5620] [--png-dir <dir>]
// Needs Node 22+ and Google Chrome, and a server with the /api/_dev hooks (mock or simulator).
// It creates and removes /sd/ui-test on the server.

import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, existsSync, rmSync, writeFileSync, readdirSync, statSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname } from 'node:path';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const BASE = opt('--url', 'http://localhost:5620').replace(/\/$/, '');
const SHOTS = opt('--png-dir');
const CHROME = ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium']
  .find((p) => existsSync(p));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail) => {
  if (ok) { passed++; console.log(`  ok    ${name}`); }
  else { failed++; console.log(`  FAIL  ${name}${detail !== undefined ? `  -> ${JSON.stringify(detail)}` : ''}`); }
};

// ---- wait for the server (the simulator takes a few seconds to start)
for (let i = 0; ; i++) {
  try { if ((await fetch(`${BASE}/api/caps`)).ok) break; } catch {}
  if (i >= 60) { console.log(`no server at ${BASE}`); process.exit(2); }
  await sleep(500);
}

// ---- a session for direct API calls (seeding and outside changes)
const pin = (await (await fetch(`${BASE}/api/_dev/pin`)).json()).pin;
const pr = await fetch(`${BASE}/api/auth/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
const cookie = (pr.headers.get('set-cookie') || '').split(';')[0];
const call = (method, path, body, raw) => fetch(BASE + path, {
  method, headers: { cookie, ...(raw ? {} : { 'content-type': 'application/json' }) }, body: raw ? body : body && JSON.stringify(body),
});
await call('POST', '/api/_dev/sd', { present: true });
await call('DELETE', '/api/files?path=%2Fsd%2Fui-test&recursive=1');
await call('PUT', '/api/files/content?path=%2Fsd%2Fui-test%2Fnotes%2Ftodo.txt', 'Shopping: milk, bread', true);
await call('PUT', '/api/files/content?path=%2Fsd%2Fui-test%2Fsettings.json', '{"a":1,"b":[1,2]}', true);

// ---- Chrome
const profile = mkdtempSync(join(tmpdir(), 'web-ui-test-'));
const chrome = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run',
  '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
let wsUrl;
for (let i = 0; i < 300 && !wsUrl; i++) {
  await sleep(100);
  try {
    const [port, path] = readFileSync(join(profile, 'DevToolsActivePort'), 'utf8').split('\n');
    if (port && path) wsUrl = `ws://127.0.0.1:${port}${path}`;
  } catch {}
}
if (!wsUrl) { chrome.kill(); console.log('Chrome did not start'); process.exit(2); }
const ws = new WebSocket(wsUrl);
await new Promise((r) => { ws.onopen = r; });
let id = 0;
const pending = new Map();
const errors = [];
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  else if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description?.split('\n')[0]);
  else if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map((a) => a.value ?? a.description).join(' '));
};
const send = (method, params = {}, sessionId) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params, sessionId })); });
const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
const S = (m, p) => send(m, p, sessionId);
await S('Runtime.enable');
await S('Page.enable');

const js = async (expr) => {
  const r = await S('Runtime.evaluate', { expression: `(async () => { ${expr} })()`, awaitPromise: true, returnByValue: true });
  if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || 'eval failed');
  return r.result?.result?.value;
};
const waitFor = async (expr, ms = 4000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { if (await js(`return !!(${expr});`)) return true; } catch {}
    await sleep(100);
  }
  return false;
};
const shot = async (name) => {
  if (!SHOTS) return;
  const r = await S('Page.captureScreenshot', { format: 'png' });
  writeFileSync(join(SHOTS, `${name}.png`), Buffer.from(r.result.data, 'base64'));
};
// Helpers that run in the page.
const PAGE = `
  window.$t = (sel, text) => [...document.querySelectorAll(sel)].find((e) => e.textContent.trim() === text);
  window.$row = (name) => [...document.querySelectorAll('.frow:not(.head)')].find((r) => r.querySelector('.fname')?.textContent === name);
  window.$type = (el, v) => { el.value = v; el.dispatchEvent(new Event('input', { bubbles: true })); };
`;
const go = async (hash = '') => {
  await S('Page.navigate', { url: `${BASE}/${hash}` });
  await sleep(150);
  await S('Page.reload', { ignoreCache: true });
  await waitFor('document.readyState === "complete"');
  await js(PAGE);
};

const hadDesktop = (await call('GET', '/api/files/content?path=%2Fsd%2Fweb%2Fmanifest.json')).ok;
const hadData = (await call('GET', '/api/files?path=%2Fsd%2Fdata')).ok;
try {
  console.log(`web desktop UI test: ${BASE}`);

  // ---- rescue page: install a freshly built desktop through it
  console.log('rescue');
  const buildDir = mkdtempSync(join(tmpdir(), 'web-build-'));
  const tool = join(dirname(fileURLToPath(import.meta.url)), 'web_build.py');
  execFileSync('python3', [tool, '--out', join(buildDir, 'web')]);
  execFileSync('python3', [tool, '--zip', join(buildDir, 'zip')]);
  const files = [];
  const walk = (d, rel) => {
    for (const n of readdirSync(d)) {
      const p = join(d, n), r = rel ? `${rel}/${n}` : n;
      if (statSync(p).isDirectory()) walk(p, r); else files.push({ path: r, b64: readFileSync(p).toString('base64') });
    }
  };
  walk(join(buildDir, 'web'), '');
  const built = JSON.parse(readFileSync(join(buildDir, 'web', 'manifest.json'), 'utf8'));
  const zipName = readdirSync(join(buildDir, 'zip'))[0];
  const zipB64 = readFileSync(join(buildDir, 'zip', zipName)).toString('base64');
  // The same ZIP as Finder makes it: everything inside a folder, plus __MACOSX junk.
  const wrapped = join(buildDir, 'wrapped.zip');
  execFileSync('python3', ['-c', `
import zipfile, sys
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as a, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as b:
    for i in a.infolist():
        b.writestr('twatch-sd/' + i.filename, a.read(i))
    b.writestr('__MACOSX/twatch-sd/._README.txt', b'junk')
`, join(buildDir, 'zip', zipName), wrapped]);
  const wrappedB64 = readFileSync(wrapped).toString('base64');
  rmSync(buildDir, { recursive: true, force: true });
  const blob = (b64) => `new Blob([Uint8Array.from(atob(${JSON.stringify(b64)}), (c) => c.charCodeAt(0))])`;

  await go('rescue');
  check('rescue page asks for the PIN', await waitFor('!document.getElementById("pair").hidden'));
  await js(`document.getElementById('pin').value = ${JSON.stringify(pin)}; document.getElementById('pairBtn').click();`);
  check('rescue page pairs and shows the installer', await waitFor('!document.getElementById("install").hidden && !document.getElementById("pickBtn").disabled'));

  let installed = await js(`return await window.installZip(${blob(zipB64)});`);
  check(`install from the SD starter ZIP (${zipName})`, installed === built.version, installed);
  const onCard = await (await call('GET', '/api/files/content?path=%2Fsd%2Fweb%2Fmanifest.json')).json().catch(() => null);
  check('desktop is on the card with every file listed', onCard && Object.keys(onCard.files || {}).length === Object.keys(built.files).length, onCard);
  const dataDirs = (await (await call('GET', '/api/files?path=%2Fsd%2Fdata')).json().catch(() => ({}))).entries?.map((e) => e.name) || [];
  check('the ZIP made the data folders', ['gnss', 'ir', 'lora', 'nfc', 'recordings'].every((d) => dataDirs.includes(d)), dataDirs);
  installed = await js(`return await window.installZip(${blob(wrappedB64)});`);
  check('install from a ZIP wrapped in a folder (Finder)', installed === built.version, installed);
  installed = await js(`
    const files = ${JSON.stringify(files)}.map((f) => ({ path: f.path,
      file: new Blob([Uint8Array.from(atob(f.b64), (c) => c.charCodeAt(0))]) }));
    return await window.installDesktop(files);`);
  check('update over an installed desktop from a folder', installed === built.version, installed);
  const notZip = await js(`try { await window.installZip(new Blob(['hello'])); return 'no error'; } catch (e) { return e.message; }`);
  check('a file that is not a ZIP is refused clearly', /not a ZIP/.test(notZip), notZip);
  const left = await call('GET', '/api/files?path=%2Fsd%2Fweb.new');
  check('no web.new left behind', left.status === 404, left.status);
  // Log this browser out again, so the desktop's own pairing is tested next.
  await js(`await fetch('/api/auth/logout', { method: 'POST', headers: { 'content-type': 'application/json' }, body: '{}' });`);


  // ---- pairing
  console.log('pairing');
  await go();
  check('pairing screen shows', await waitFor('document.querySelectorAll(".pin input").length === 6'));
  await shot('1-pairing');
  await js(`document.querySelectorAll('.pin input').forEach((b) => $type(b, '9'));`);
  check('wrong PIN is refused', await waitFor('document.querySelector(".pair-error")?.textContent.includes("Wrong PIN")'));
  const digits = [...pin];
  await js(`const b = document.querySelectorAll('.pin input'); ${JSON.stringify(digits)}.forEach((d, i) => $type(b[i], d));`);
  check('right PIN opens the installed desktop', await waitFor('document.querySelector(".launcher .tile")'));
  check('status bar shows the battery', await waitFor('/\\d+%/.test(document.querySelector(".bar").textContent)'));
  check('event stream connected', await waitFor('document.querySelector(".link-dot.on")'));
  await shot('2-desktop');

  // ---- files
  console.log('files');
  await go('#app=files');
  check('Files opens from the URL', await waitFor('document.querySelector(".win .frow")'));
  await js(`$row('ui-test').click();`);
  check('open a folder', await waitFor('$row("notes") && $row("settings.json")'));
  await js(`$row('settings.json').click();`);
  check('preview pretty-prints JSON', await waitFor('document.querySelector(".dialog pre")?.textContent.includes(\'"b": [\')'));
  await js(`$t('.dialog .btn', 'Close').click();`);
  await js(`$row('notes').click();`);
  await waitFor('$row("todo.txt")');
  await js(`$row('todo.txt').querySelector('[title="Rename"]').click();`);
  await waitFor('document.querySelector(".dialog input")');
  await js(`$type(document.querySelector('.dialog input'), 'todo-2.txt'); $t('.dialog .btn', 'Rename').click();`);
  check('rename', await waitFor('$row("todo-2.txt") && !$row("todo.txt")'));
  await js(`document.querySelector('[aria-label="New folder"]').click();`);
  await waitFor('document.querySelector(".dialog input")');
  await js(`$type(document.querySelector('.dialog input'), 'sub'); $t('.dialog .btn', 'Create').click();`);
  check('new folder', await waitFor('$row("sub")'));
  await js(`
    const input = document.querySelector('input[type=file]');
    const dt = new DataTransfer();
    dt.items.add(new File(['uploaded from the browser'], 'hello.txt', { type: 'text/plain' }));
    input.files = dt.files;
    input.dispatchEvent(new Event('change'));`);
  check('upload', await waitFor('$row("hello.txt")'));
  const up = await (await call('GET', '/api/files/content?path=%2Fsd%2Fui-test%2Fnotes%2Fhello.txt')).text();
  check('uploaded file has the right content', up === 'uploaded from the browser', up);
  await js(`$row('sub').querySelector('[title="Delete"]').click();`);
  await waitFor('document.querySelector(".dialog")');
  await js(`$t('.dialog .btn', 'Delete').click();`);
  check('delete', await waitFor('!$row("sub")'));
  await call('PUT', '/api/files/content?path=%2Fsd%2Fui-test%2Fnotes%2Ffrom-outside.txt', 'x', true);
  check('a change made elsewhere shows up by itself', await waitFor('$row("from-outside.txt")'));
  await shot('3-files');

  // ---- SD removal
  console.log('sd');
  await call('POST', '/api/_dev/sd', { present: false });
  check('Files says the card is gone', await waitFor('document.querySelector(".win .empty")?.textContent.includes("No SD card")'));
  check('status bar says No SD', await waitFor('document.querySelector(".bar").textContent.includes("No SD")'));
  await shot('4-no-sd');
  await call('POST', '/api/_dev/sd', { present: true });
  check('card back: Files lists again', await waitFor('document.querySelector(".win .frow")'));

  // ---- settings
  console.log('settings');
  await go('#app=settings');
  check('Settings opens', await waitFor('document.querySelector("input[type=range]")'));
  await js(`$type(document.querySelector('input[type=range]'), '25');`);
  await sleep(600);
  const st = await (await call('GET', '/api/settings')).json();
  check('brightness slider changes the watch', Math.abs(st.brightness - 25) <= 1, st);
  check('storage section shows the card', await waitFor('document.querySelector(".settings").textContent.includes("Safe to remove")'));
  await shot('5-settings');

  // ---- theme: changing the watch's colours recolours the desktop live
  console.log('theme');
  const accentNow = () => js(`return getComputedStyle(document.documentElement).getPropertyValue('--accent').trim().toLowerCase();`);
  await call('POST', '/api/_dev/theme', { preset: 1 });           // Ocean
  check('desktop follows the watch theme (Ocean)', await waitFor(`getComputedStyle(document.documentElement).getPropertyValue('--accent').trim().toLowerCase() === '#0088ff'`, 5000), await accentNow());
  // Every watch preset: the desktop takes its accent, and text on the accent stays readable (4.5:1).
  const contrastOk = await js(`
    const lin = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };
    const L = (hex) => { const n = parseInt(hex.slice(1), 16); return 0.2126 * lin((n >> 16) & 255) + 0.7152 * lin((n >> 8) & 255) + 0.0722 * lin(n & 255); };
    const bad = [];
    for (let p = 0; p < 8; p++) {
      await fetch('/api/_dev/theme', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ preset: p }) });
      const want = (await (await fetch('/api/device')).json()).theme.accent;
      const end = Date.now() + 4000;
      let acc = '';
      while (Date.now() < end) {
        acc = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim().toLowerCase();
        if (acc === want) break;
        await new Promise((r) => setTimeout(r, 100));
      }
      const on = getComputedStyle(document.documentElement).getPropertyValue('--on-accent').trim();
      const [a, b] = [L(acc), L(on)].sort((x, y) => y - x);
      const ratio = (a + 0.05) / (b + 0.05);
      if (acc !== want || ratio < 4.5) bad.push({ p, want, acc, on, ratio: ratio.toFixed(1) });
    }
    return bad;`);
  check('desktop follows all 8 watch presets, text on the accent 4.5:1 or more', contrastOk.length === 0, contrastOk);
  await call('POST', '/api/_dev/theme', { preset: 7 });           // Mono, for the screenshot
  await waitFor(`getComputedStyle(document.documentElement).getPropertyValue('--accent').trim().toLowerCase() === '#ffffff'`, 4000);
  await shot('7b-theme-mono');
  await call('POST', '/api/_dev/theme', { preset: 0 });

  // ---- screen: the live picture, and taps / swipes on it reach the watch
  console.log('screen');
  await S('Emulation.clearDeviceMetricsOverride');
  await go('#app=screen');
  check('Screen shows frames', await waitFor('+document.querySelector(".screen-canvas")?.dataset.frames >= 1', 6000));
  check('the picture is not blank', await js(`
    const c = document.querySelector('.screen-canvas');
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let lit = 0; for (let i = 0; i < d.length; i += 4 * 97) if (d[i] + d[i + 1] + d[i + 2] > 60) lit++;
    return lit > 20;`));
  const fire = (type, fx, fy) => `(() => { const c = document.querySelector('.screen-canvas'); const r = c.getBoundingClientRect();
    c.dispatchEvent(new PointerEvent('${type}', { bubbles: true, pointerId: 7, clientX: r.left + r.width * ${fx}, clientY: r.top + r.height * ${fy} })); })();`;
  await js(fire('pointerdown', 0.5, 0.5) + fire('pointerup', 0.5, 0.5));
  await sleep(500);
  let last = await (await call('GET', '/api/_dev/input')).json();
  check('a click on the picture taps the watch there', last.type === 'tap' && Math.abs(last.x - 251) <= 3 && Math.abs(last.y - 205) <= 3, last);
  await js(fire('pointerdown', 0.5, 0.95) + fire('pointerup', 0.5, 0.3));
  await sleep(500);
  last = await (await call('GET', '/api/_dev/input')).json();
  check('a drag on the picture swipes', last.type === 'swipe' && last.y2 < last.y, last);
  // Swipe right to undo whatever the swipe opened, so the watch is left as found.
  await call('POST', '/api/input', { type: 'swipe', x: 40, y: 200, x2: 450, y2: 200 });
  await shot('8-screen');

  // ---- phone
  console.log('phone layout');
  await S('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await go('#app=files');
  await waitFor('document.querySelector(".win .frow")');
  check('app is full screen on a phone', await js(`const r = document.querySelector('.win').getBoundingClientRect(); return r.width >= 389 && r.left === 0;`));
  check('no sideways scroll', await js('return document.documentElement.scrollWidth <= 390;'));
  await shot('6-phone-files');
  await js(`document.querySelector('.win .back').click();`);
  check('back returns to the launcher', await waitFor('!document.querySelector(".win") && document.querySelector(".tile")'));
  await shot('7-phone-launcher');

  check('no JavaScript errors', errors.length === 0, errors);
} catch (e) {
  failed++;
  console.log(`  FAIL  ${e.message}`);
} finally {
  await call('DELETE', '/api/files?path=%2Fsd%2Fui-test&recursive=1').catch(() => {});
  // Leave the card as it was: remove only what the test put there.
  if (!hadDesktop) await call('DELETE', '/api/files?path=%2Fsd%2Fweb&recursive=1').catch(() => {});
  if (!hadData) await call('DELETE', '/api/files?path=%2Fsd%2Fdata&recursive=1').catch(() => {});
  await call('PATCH', '/api/settings', { brightness: 60 }).catch(() => {});
  await call('POST', '/api/_dev/theme', { preset: 0 }).catch(() => {});
  await call('POST', '/api/auth/logout', {}).catch(() => {});
  ws.close();
  chrome.kill();
  await sleep(300);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
}
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
