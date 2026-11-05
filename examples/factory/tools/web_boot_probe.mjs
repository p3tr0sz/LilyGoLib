#!/usr/bin/env node
// Loads the web desktop from a server (the mock watch web_mock_device.mjs, or the watch) in headless Chrome
// and records every request it makes, in two phases: before pairing and after pairing.
// Used to check what the desktop asks the watch for (docs/web-api.md) and to compare the watch with the mock
// watch (web_mock_device.mjs).
//
// Usage:
//   node web_boot_probe.mjs --url http://localhost:5620 --dev-pin        (mock watch: reads /api/_dev/pin)
//   node web_boot_probe.mjs --url http://192.168.1.50 --pin 123456      (watch: PIN from its screen)
// Options: --wait <ms> per phase (default 15000), --app <id> (repeatable) also open that app,
//          --json <file> raw report, --png <file> last screenshot, --chrome <path>.
// Needs Node 22+ (global WebSocket) and Google Chrome.

import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const args = process.argv.slice(2);
const opt = (name, dflt) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : dflt; };
const flag = (name) => args.includes(name);
const BASE = (opt('--url', 'http://localhost:5620')).replace(/\/$/, '');
const WAIT = Number(opt('--wait', 15000));
const CHROME = opt('--chrome', [
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
].find((p) => existsSync(p)));
if (!CHROME) { console.error('Chrome not found; pass --chrome <path>'); process.exit(2); }

let PIN = opt('--pin', '');
if (!PIN && flag('--dev-pin')) PIN = (await (await fetch(BASE + '/api/_dev/pin')).json()).pin;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- Chrome + CDP ----
const profile = mkdtempSync(join(tmpdir(), 'web-probe-'));
const chrome = spawn(CHROME, ['--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
  '--no-first-run', '--no-default-browser-check', '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
let wsUrl = null;
for (let i = 0; i < 100 && !wsUrl; i++) {
  await sleep(100);
  try { const [port, path] = readFileSync(join(profile, 'DevToolsActivePort'), 'utf8').split('\n'); wsUrl = `ws://127.0.0.1:${port}${path}`; } catch {}
}
if (!wsUrl) { chrome.kill(); console.error('Chrome did not start'); process.exit(2); }

const ws = new WebSocket(wsUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
let nextId = 1;
const pending = new Map();
const listeners = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { const { res, rej } = pending.get(m.id); pending.delete(m.id); m.error ? rej(new Error(m.error.message)) : res(m.result); }
  else if (m.method) for (const l of listeners) l(m);
};
const send = (method, params = {}, sessionId) => new Promise((res, rej) => {
  const id = nextId++; pending.set(id, { res, rej }); ws.send(JSON.stringify({ id, method, params, sessionId }));
});

const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
const S = (method, params) => send(method, params, sessionId);
await S('Network.enable');
await S('Page.enable');
await S('Runtime.enable');
await S('Network.setCacheDisabled', { cacheDisabled: true });

// ---- recording ----
let phase = null;
const phases = {};
const reqs = new Map();   // requestId -> record
listeners.push((m) => {
  if (m.sessionId !== sessionId || !phase) return;
  const P = phases[phase];
  const p = m.params;
  switch (m.method) {
    case 'Network.requestWillBeSent': {
      const u = new URL(p.request.url);
      if (u.protocol === 'data:' || u.protocol === 'blob:') break;
      if (u.origin !== new URL(BASE).origin) { P.external.add(u.origin + u.pathname); break; }
      const rec = { method: p.request.method, path: u.pathname, query: u.search, status: null, bytes: 0, type: p.type };
      reqs.set(p.requestId, rec); P.requests.push(rec); break;
    }
    case 'Network.responseReceived': { const r = reqs.get(p.requestId); if (r) { r.status = p.response.status; r.mime = p.response.mimeType; r.sw = !!p.response.fromServiceWorker; r.gz = /gzip/i.test(p.response.headers['content-encoding'] || p.response.headers['Content-Encoding'] || ''); } break; }
    case 'Network.loadingFinished': { const r = reqs.get(p.requestId); if (r) r.bytes = p.encodedDataLength; break; }
    case 'Network.loadingFailed': { const r = reqs.get(p.requestId); if (r) r.status = r.status || `failed:${p.errorText}`; break; }
    case 'Network.webSocketCreated': P.websockets.push(new URL(p.url).pathname + new URL(p.url).search); break;
    case 'Network.webSocketFrameReceived': P.wsFrames.push(String(p.response.payloadData).slice(0, 200)); break;
    case 'Runtime.exceptionThrown': P.errors.push(p.exceptionDetails.exception?.description?.split('\n')[0] || p.exceptionDetails.text); break;
    case 'Runtime.consoleAPICalled': if (p.type === 'error') P.errors.push(p.args.map((a) => a.value ?? a.description ?? '').join(' ').slice(0, 300)); break;
  }
});

async function runPhase(name, url) {
  phases[name] = { requests: [], websockets: [], wsFrames: [], errors: [], external: new Set() };
  phase = name;
  await S('Page.navigate', { url });
  await sleep(WAIT);
  phase = null;
}

await runPhase('unpaired', BASE + '/');
let paired = false;
if (PIN) {
  const r = await S('Runtime.evaluate', {
    expression: `fetch('/api/auth/pair',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({pin:${JSON.stringify(String(PIN))}})}).then(r=>r.status)`,
    awaitPromise: true, returnByValue: true,
  });
  paired = r.result.value === 200;
  if (paired) {
    await runPhase('paired', BASE + '/');
    // --app <id> (repeatable): open each app on its own after pairing, to see which routes it needs.
    for (let i = 0; i < args.length; i++) {
      if (args[i] === '--app') await runPhase('app:' + args[i + 1], `${BASE}/apps/${args[i + 1]}/`);
    }
  } else {
    console.error(`pairing failed: HTTP ${r.result.value}`);
  }
}
const shot = await S('Page.captureScreenshot', { format: 'png' }).catch(() => null);

ws.close(); chrome.kill(); await sleep(300);
try { rmSync(profile, { recursive: true, force: true }); } catch {}

// ---- report ----
const report = { base: BASE, paired, phases: {} };
for (const [name, P] of Object.entries(phases)) {
  const api = {}; let staticCount = 0, staticBytes = 0, fromSW = 0; const missing = [];
  for (const r of P.requests) {
    const isApi = r.path.startsWith('/api/');
    if (isApi) {
      const k = `${r.method} ${r.path}`;
      (api[k] ||= { count: 0, statuses: new Set(), queries: new Set() }).count++; api[k].statuses.add(r.status);
      if (r.query) api[k].queries.add(decodeURIComponent(r.query));
    } else {
      staticCount++; staticBytes += r.bytes || 0; if (r.sw) fromSW++;
      if (r.status && r.status !== 200 && r.status !== 304 && r.status !== 206) missing.push(`${r.status} ${r.path}`);
    }
  }
  report.phases[name] = {
    api: Object.fromEntries(Object.entries(api).sort().map(([k, v]) => [k, { count: v.count, statuses: [...v.statuses], queries: [...v.queries] }])),
    static: { requests: staticCount, kbytes: Math.round(staticBytes / 1024), fromServiceWorker: fromSW },
    staticPaths: [...new Set(P.requests.filter((r) => !r.path.startsWith('/api/')).map((r) => r.path))],
    staticNot200: [...new Set(missing)],
    websockets: [...new Set(P.websockets)],
    wsFrames: P.wsFrames.slice(0, 20),
    external: [...P.external],
    errors: [...new Set(P.errors)].slice(0, 30),
  };
}
if (opt('--json')) writeFileSync(opt('--json'), JSON.stringify(report, null, 2));
if (opt('--png') && shot) writeFileSync(opt('--png'), Buffer.from(shot.data, 'base64'));

for (const [name, P] of Object.entries(report.phases)) {
  console.log(`\n== ${name} ==  static: ${P.static.requests} requests, ${P.static.kbytes} KB, ${P.static.fromServiceWorker} from service worker`);
  for (const [k, v] of Object.entries(P.api)) console.log(`  ${k.padEnd(42)} x${v.count}  ${v.statuses.join(',')}`);
  if (P.websockets.length) console.log('  ws:', P.websockets.join(' '));
  if (P.staticNot200.length) console.log('  static not 200:', P.staticNot200.join(' | '));
  if (P.external.length) console.log('  external:', P.external.map((u) => u.slice(0, 100)).join(' | '));
  if (P.errors.length) console.log('  errors:\n   - ' + P.errors.join('\n   - '));
}
