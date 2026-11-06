#!/usr/bin/env node
// Mock T-Watch Ultra: the web API v1 of docs/web-api.md (P1 routes) on the PC, so the desktop can be built and
// tested without flashing. The firmware is checked against the same behaviour with web_api_test.mjs.
//
// Usage:
//   node web_mock_device.mjs [--root <dir>] [--web <desktop dir>] [--port 5620] [--pin 123456] [--dev]
//     --root  folder holding the fake storage: <root>/sd (SD card) and <root>/flash (internal FFat);
//             default <tmp>/mock-twatch
//     --web   desktop to serve at / when the card has none (default: examples/factory/web; a missing
//             folder means the rescue page, like a watch with no desktop on its card)
//     --dev   adds test hooks the watch never has:
//               GET  /api/_dev/pin                  the PIN; opens pairing like the watch's Web page
//               POST /api/_dev/pairing {open:bool}  open or close pairing
//               POST /api/_dev/sd {present:bool}     insert or remove the SD card
//               GET  /api/_dev/input                the last /api/input body
//               POST /api/_dev/theme {preset}        switch the watch's colour preset
//               POST /api/_dev/app {name}            act as if that app were open on the watch ("" for none)
//               POST /api/_dev/lora {text|data_b64, rssi?, snr?}  a LoRa packet arrives (when listen is on)
//               POST /api/_dev/nfc {uid?, type?, text?}           a tag is held to the reader (while it scans)
//               POST /api/_dev/link {peer?|rx:{name,size}}        a watch appears in range, or sends this watch a file
//               POST /api/_dev/hid {available}                    pretend a USB host did/didn't enumerate the keyboard
//     --dl <dir>  also act as the release server for the simulator's desktop download: serves
//                 <dir>/<file> at /dl/<file> (no pairing, Range supported). With --dev:
//               POST /api/_dev/dl {cut_after?: bytes, status?: code}  the next /dl request is cut off
//                                                      after that many bytes, or answers that status
//               GET  /api/_dev/dl-log                  the /dl requests so far: {file, range, status, sent}
// Fake hardware for the P4 tools: a GNSS fix walking slowly around one spot, a slowly turning IMU, Wi-Fi and
// BLE devices, a LoRa station that answers each packet with "ack: <text>", an NFC tag a second after a scan
// starts, a recording of a tone (a real WAV, written at real-time pace) and playback that lasts as long as the file.
// Needs Node 18+.

import { createServer } from 'node:http';
import { createHash, randomBytes, randomInt } from 'node:crypto';
import { promises as fsp, existsSync, mkdirSync, statfsSync, createReadStream } from 'node:fs';
import { join, normalize, extname, dirname, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(opt('--root', join(tmpdir(), 'mock-twatch')));
const WEB = resolve(opt('--web', join(HERE, '..', 'web')));
const PORT = Number(opt('--port', 5620));
const DL = opt('--dl') ? resolve(opt('--dl')) : null;
let dlFault = null;
const dlLog = [];
const DEV = args.includes('--dev');
let PIN = opt('--pin', String(randomInt(0, 1000000)).padStart(6, '0'));
// Pairing works only while the watch shows the PIN (Settings > Web desktop). The mock starts
// with it open unless --dev, where /api/_dev/pin opens it like the tests do on the simulator.
let pairingOpen = !args.includes('--dev');
const FW = '0.1.0+web.mock';
const BOOT = Date.now();
const ROOTS = { sd: join(ROOT, 'sd'), flash: join(ROOT, 'flash') };
for (const d of Object.values(ROOTS)) mkdirSync(d, { recursive: true });

const CAPS = ['files', 'files.flash', 'sd', 'settings', 'time', 'screen', 'input', 'display',
  'monitor', 'notify', 'sensors.imu', 'gnss', 'lora', 'nfc', 'recorder', 'audio.play', 'wifi.scan', 'ble.scan', 'link.espnow', 'usb.hid', 'wifi.tools', 'ota',
  'wifi.sniffer', 'airspace', 'sentinel', 'presence', 'fido2', 'evil.portal', 'deauth.flood', 'beacon.spam'];

// ---- state ----
const sd = { present: true, mounted: true, error: null };
let settings = { brightness: 60, screen_timeout_s: 15, web_idle_off_s: 600 };
let clock = { offset_ms: 0, tz: 'UTC', offset_min: 0, synced: false };
let battery = { percent: 76, charging: false, mv: 3968 };
const PRESETS = [['#00d4aa', '#009977', '#1a1a1a', '#333333', 'Mint'], ['#0088ff', '#0066cc', '#1a1a2e', '#333344', 'Ocean'],
  ['#ff6b35', '#cc5522', '#2a1a1a', '#443333', 'Sunset'], ['#b577ff', '#8833cc', '#1e1a2a', '#3a3344', 'Violet'],
  ['#ff3366', '#cc2255', '#2a1a20', '#443338', 'Cherry'], ['#88cc00', '#669900', '#1a2a1a', '#334433', 'Lime'],
  ['#ffb800', '#cc9200', '#2a2a1a', '#444433', 'Amber'], ['#ffffff', '#cccccc', '#2a2a2a', '#555555', 'Mono']]
  .map(([accent, accent_dim, card, track, name]) => ({ accent, accent_dim, card, track, name }));
let theme = PRESETS[0];

// ---- helpers ----
const TYPES = { '.html': 'text/html; charset=utf-8', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
  '.gif': 'image/gif', '.webp': 'image/webp', '.ico': 'image/x-icon', '.wav': 'audio/wav', '.mp3': 'audio/mpeg',
  '.txt': 'text/plain; charset=utf-8', '.md': 'text/plain; charset=utf-8', '.csv': 'text/csv', '.gpx': 'application/gpx+xml',
  '.woff2': 'font/woff2', '.webmanifest': 'application/manifest+json' };
const typeOf = (p) => TYPES[extname(p).toLowerCase()] || 'application/octet-stream';
function json(res, code, obj, headers = {}) {
  res.writeHead(code, { 'content-type': 'application/json', 'cache-control': 'no-store', ...headers });
  res.end(JSON.stringify(obj));
}
const ok = (res, obj = {}) => json(res, 200, { ok: true, ...obj });
const fail = (res, code, error, message, extra = {}) => json(res, code, { error, ...(message ? { message } : {}), ...extra });
const readBody = (req) => new Promise((done, bad) => { const c = []; req.on('data', (d) => c.push(d)); req.on('end', () => done(Buffer.concat(c))); req.on('error', bad); });
const readJSON = async (req) => { try { return JSON.parse((await readBody(req)).toString('utf8') || '{}'); } catch { return null; } };
const nowEpoch = () => Math.floor((Date.now() + clock.offset_ms) / 1000);

// API path -> host path, or an error code. Roots: /sd, /flash.
function resolvePath(p) {
  if (typeof p !== 'string' || !p.startsWith('/') || p.split('/').includes('..')) return { error: 'bad_request' };
  const m = /^\/(sd|flash)(\/.*)?$/.exec(p);
  if (!m) return { error: 'bad_request' };
  if (m[1] === 'sd' && !sd.mounted) return { error: 'sd_missing' };
  const base = ROOTS[m[1]];
  const abs = normalize(join(base, m[2] || '/'));
  if (abs !== base && !abs.startsWith(base + '/')) return { error: 'bad_request' };
  return { abs, root: m[1] };
}
function pathError(res, e) {
  return e === 'sd_missing' ? fail(res, 503, 'sd_missing', 'SD card not mounted') : fail(res, 400, 'bad_request', 'bad path');
}

// ---- sessions ----
const sessions = new Set();
let misses = 0, lockedUntil = 0;
const tokenOf = (req) => { for (const kv of (req.headers.cookie || '').split(';')) { const [k, v] = kv.trim().split('='); if (k === 'session') return v; } return null; };
const isPaired = (req) => sessions.has(tokenOf(req));

// ---- events ----
let seq = 0;
let desktop = null;

// P9 NucleoOS parity tool state
let sniffer = { active: false, packets: 0, bytes: 24, channel: null, file: null };
let airspace = { active: false, deauth_total: 0, deauth_floods: 0, beacon_total: 0, beacon_floods: 0, evil_twins: 0, started: 0 };
let sentinel = { active: false, started: 0, device_count: 0, devices: [] };
let presence = { active: false, started: 0, peers_ever: 0, peers: [], history: [] };
let fido2 = { available: false, state: 'idle', credentials: 0, creds: [] };
let evilportal = { active: false, ssid: '', clients: 0, attempts: 0, captures: [] };
let deauthflood = { active: false, target_bssid: '', target_station: 'ff:ff:ff:ff:ff:ff', channel: 1, sent: 0 };
let beaconspam = { active: false, channel: 0, beacons: 0, ssids: 0, prefix: 'FreeNet' };

function airspaceState() {
  return { active: airspace.active, deauth_total: airspace.deauth_total, deauth_floods: airspace.deauth_floods, beacon_total: airspace.beacon_total, beacon_floods: airspace.beacon_floods, evil_twins: airspace.evil_twins, started: airspace.started };
}
function sentinelState() {
  return { active: sentinel.active, started: sentinel.started, device_count: sentinel.devices.length, devices: sentinel.devices };
}
function presenceState() {
  return { active: presence.active, started: presence.started, peers_ever: presence.peers_ever, peers: presence.peers, history: presence.history };
}
function fido2State() {
  return { available: true, state: fido2.state, credentials: fido2.credentials };
}
function evilportalState() {
  return { active: evilportal.active, ssid: evilportal.ssid, clients: evilportal.clients, attempts: evilportal.attempts, started: nowEpoch() };
}
function deauthfloodState() {
  return { active: deauthflood.active, target_bssid: deauthflood.target_bssid, target_station: deauthflood.target_station, channel: deauthflood.channel, sent: deauthflood.sent };
}
function beaconspamState() {
  return { active: beaconspam.active, channel: beaconspam.channel, beacons: beaconspam.beacons, ssids: beaconspam.ssids, prefix: beaconspam.prefix };
}

function wsFrame(text) {
  const d = Buffer.from(text), n = d.length;
  let h;
  if (n < 126) h = Buffer.from([0x81, n]);
  else if (n < 65536) { h = Buffer.alloc(4); h[0] = 0x81; h[1] = 126; h.writeUInt16BE(n, 2); }
  else { h = Buffer.alloc(10); h[0] = 0x81; h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2); }
  return Buffer.concat([h, d]);
}
function publish(t, d) {
  seq++;
  if (desktop) try { desktop.write(wsFrame(JSON.stringify({ t, seq, d }))); } catch {}
}

function sdStatus() {
  let total = 0, free = 0;
  if (sd.mounted) { try { const s = statfsSync(ROOTS.sd); total = s.blocks * s.bsize; free = s.bavail * s.bsize; } catch {} }
  return { present: sd.present, mounted: sd.mounted, fs: sd.mounted ? 'FAT32' : null, card: sd.present ? 'SDHC' : null, total, free, error: sd.error };
}
function flashStatus() { return { total: 8257536, free: 102400 }; }

function device(paired) {
  const full = {
    board: 'twatch-ultra', fw: FW, uptime_s: Math.floor((Date.now() - BOOT) / 1000), battery,
    sd: { present: sd.present, mounted: sd.mounted }, flash: flashStatus(),
    net: { mode: 'sta', ssid: 'mock-wifi', ip: '127.0.0.1', rssi: -55, host: 'watch.local' },
    time: { epoch: nowEpoch(), offset_min: clock.offset_min, synced: clock.synced, tz: clock.tz },
    mem: { heap_free: 182000, psram_free: 6100000 },
  };
  full.theme = theme;
  return paired ? full : { board: full.board, fw: full.fw, battery: full.battery, theme, sd: { present: sd.present } };
}

// ---- files ----
async function filesRoute(req, res, url, path) {
  const isContent = path === '/api/files/content';
  if (path === '/api/files/mkdir' || path === '/api/files/move') {
    if (req.method !== 'POST') return fail(res, 405, 'bad_request', 'POST');
    const b = await readJSON(req);
    if (!b) return fail(res, 400, 'bad_request', 'bad JSON');
    if (path === '/api/files/mkdir') {
      const r = resolvePath(b.path); if (r.error) return pathError(res, r.error);
      const existed = existsSync(r.abs);
      if (existed && !(await fsp.stat(r.abs)).isDirectory()) return fail(res, 409, 'conflict', 'a file has that name');
      await fsp.mkdir(r.abs, { recursive: true });
      if (!existed) publish('files.changed', { op: 'mkdir', path: b.path });
      return ok(res);
    }
    const f = resolvePath(b.from), t = resolvePath(b.to);
    if (f.error) return pathError(res, f.error);
    if (t.error) return pathError(res, t.error);
    if (!existsSync(f.abs)) return fail(res, 404, 'not_found');
    if (existsSync(t.abs)) {
      if (!b.overwrite) return fail(res, 409, 'conflict', 'destination exists');
      await fsp.rm(t.abs, { recursive: true, force: true });
    }
    await fsp.mkdir(dirname(t.abs), { recursive: true });
    if (f.root === t.root) await fsp.rename(f.abs, t.abs);
    else { await fsp.cp(f.abs, t.abs, { recursive: true }); await fsp.rm(f.abs, { recursive: true, force: true }); }
    publish('files.changed', { op: 'move', path: b.from, to: b.to });
    return ok(res);
  }

  const p = url.searchParams.get('path');
  const r = resolvePath(p); if (r.error) return pathError(res, r.error);

  if (!isContent && req.method === 'GET') {                       // list
    let names;
    try { if (!(await fsp.stat(r.abs)).isDirectory()) throw 0; names = await fsp.readdir(r.abs); }
    catch { return fail(res, 404, 'not_found'); }
    const entries = [];
    for (const name of names) {
      if (name.endsWith('.tmp')) continue;
      try { const s = await fsp.stat(join(r.abs, name)); entries.push({ name, dir: s.isDirectory(), size: s.isDirectory() ? 0 : s.size, mtime: Math.floor(s.mtimeMs / 1000) }); } catch {}
    }
    entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
    return json(res, 200, { path: p, entries });
  }
  if (!isContent && req.method === 'DELETE') {
    let s; try { s = await fsp.stat(r.abs); } catch { return fail(res, 404, 'not_found'); }
    if (r.abs === ROOTS[r.root]) return fail(res, 400, 'bad_request', 'cannot delete a root');
    if (s.isDirectory()) {
      if ((await fsp.readdir(r.abs)).length && url.searchParams.get('recursive') !== '1') return fail(res, 409, 'conflict', 'folder not empty');
      await fsp.rm(r.abs, { recursive: true, force: true });
    } else await fsp.unlink(r.abs);
    publish('files.changed', { op: 'delete', path: p });
    return ok(res);
  }
  if (isContent && req.method === 'GET') {
    let s; try { s = await fsp.stat(r.abs); if (!s.isFile()) throw 0; } catch { return fail(res, 404, 'not_found'); }
    const m = req.headers.range && /bytes=(\d*)-(\d*)/.exec(req.headers.range);
    if (m) {
      let a = m[1] ? +m[1] : 0, b = m[2] ? +m[2] : s.size - 1;
      if (b >= s.size) b = s.size - 1;
      if (a > b) { res.writeHead(416, { 'content-range': `bytes */${s.size}` }); return res.end(); }
      res.writeHead(206, { 'content-type': typeOf(r.abs), 'accept-ranges': 'bytes', 'content-range': `bytes ${a}-${b}/${s.size}`, 'content-length': b - a + 1 });
      return createReadStream(r.abs, { start: a, end: b }).pipe(res);
    }
    res.writeHead(200, { 'content-type': typeOf(r.abs), 'accept-ranges': 'bytes', 'content-length': s.size, 'cache-control': 'no-store' });
    return createReadStream(r.abs).pipe(res);
  }
  if (isContent && req.method === 'PUT') {
    const data = await readBody(req);
    if (existsSync(r.abs) && (await fsp.stat(r.abs)).isDirectory()) return fail(res, 409, 'conflict', 'a folder has that name');
    await fsp.mkdir(dirname(r.abs), { recursive: true });
    if (url.searchParams.has('offset')) {
      const at = Number(url.searchParams.get('offset'));
      let have = 0; try { have = (await fsp.stat(r.abs)).size; } catch {}
      if (!Number.isInteger(at) || at !== have) return fail(res, 409, 'conflict', 'offset does not match', { size: have });
      await fsp.appendFile(r.abs, data);
      publish('files.changed', { op: 'write', path: p });
      return ok(res, { size: have + data.length });
    }
    await fsp.writeFile(r.abs + '.tmp', data);                     // write then rename: never half a file
    await fsp.rename(r.abs + '.tmp', r.abs);
    publish('files.changed', { op: 'write', path: p });
    return ok(res, { size: data.length });
  }
  return fail(res, 405, 'bad_request', 'method');
}

// ---- static: /sd/web if it holds a desktop for API 1, else --web, else a placeholder ----
async function desktopDir() {
  if (sd.mounted) {
    try { const m = JSON.parse(await fsp.readFile(join(ROOTS.sd, 'web', 'manifest.json'), 'utf8')); if (m.api === 1) return join(ROOTS.sd, 'web'); } catch {}
  }
  return existsSync(WEB) ? WEB : null;
}
const RESCUE = join(HERE, '..', 'web-rescue', 'index.html');
function sendRescue(res) {
  res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-cache', 'x-frame-options': 'SAMEORIGIN' });
  createReadStream(RESCUE).pipe(res);
}
async function staticRoute(req, res, path) {
  if (path.split('/').includes('..')) return fail(res, 400, 'bad_request');
  if (path === '/rescue' || path === '/rescue/') return sendRescue(res);
  const dir = await desktopDir();
  if (!dir) {
    if (path === '/' || path === '/index.html') return sendRescue(res);
    return fail(res, 404, 'not_found');
  }
  let rel = path === '/' ? '/index.html' : path;
  if (rel.endsWith('/')) rel += 'index.html';
  const abs = join(dir, rel);
  const gz = /gzip/.test(req.headers['accept-encoding'] || '') && existsSync(abs + '.gz');
  const file = gz ? abs + '.gz' : abs;
  let s; try { s = await fsp.stat(file); if (!s.isFile()) throw 0; } catch { return fail(res, 404, 'not_found'); }
  const etag = `"${gz ? 'g' : 'r'}${s.size.toString(16)}-${Math.floor(s.mtimeMs / 1000).toString(16)}"`;
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, { etag }); return res.end(); }
  const h = { 'content-type': typeOf(abs), 'content-length': s.size, etag, 'cache-control': 'no-cache' };
  if (gz) h['content-encoding'] = 'gzip';
  if (h['content-type'].startsWith('text/html')) h['x-frame-options'] = 'SAMEORIGIN';
  res.writeHead(200, h);
  createReadStream(file).pipe(res);
}

// ---- screen and input: a fake 502x410 watch face; taps leave dots, so a test sees the screen change ----
const SW = 502, SH = 410;
let screenOn = true;
let lastInput = {};
const dots = [];
const rgb565 = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
function frame() {
  const px = new Uint16Array(SW * SH);
  if (!screenOn) return px;
  const card = rgb565(26, 26, 26), accent = rgb565(0, 212, 170), white = rgb565(255, 255, 255);
  const rect = (x0, y0, w, h, c) => { for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) if (x >= 0 && y >= 0 && x < SW && y < SH) px[y * SW + x] = c; };
  rect(30, 20, 190, 64, card);                    // battery pill
  rect(150, 140, 200, 90, white);                 // "clock"
  rect(200, 245, 100, 12, accent);                // date
  rect(30, 315, 80, 80, card); rect(392, 315, 80, 80, card);
  rect(220, 393, 62, 5, rgb565(80, 80, 80));      // swipe-up hint
  for (const [x, y] of dots) rect(x - 4, y - 4, 9, 9, accent);
  return px;
}
function screenRoute(req, res, url) {
  const fmt = url.searchParams.get('format') || 'rle565';
  if (fmt !== 'rle565' && fmt !== 'rgb565') return fail(res, 400, 'bad_request', 'format: rle565 or rgb565');
  const px = frame();
  let h = 2166136261;
  for (const v of px) { h = Math.imul(h ^ (v & 255), 16777619); h = Math.imul(h ^ (v >> 8), 16777619); }
  const etag = `"${fmt}-${(h >>> 0).toString(16).padStart(8, '0')}"`;
  const headers = { etag, 'cache-control': 'no-cache', 'x-width': SW, 'x-height': SH };
  if (req.headers['if-none-match'] === etag) { res.writeHead(304, headers); return res.end(); }
  let body;
  if (fmt === 'rgb565') body = Buffer.from(px.buffer);
  else {
    const out = [];
    for (let i = 0; i < px.length;) {
      let n = 1;
      while (i + n < px.length && px[i + n] === px[i] && n < 65535) n++;
      out.push(n, px[i]);
      i += n;
    }
    body = Buffer.from(Uint16Array.from(out).buffer);
  }
  res.writeHead(200, { ...headers, 'content-type': 'application/octet-stream', 'content-length': body.length });
  res.end(body);
}
async function inputRoute(req, res) {
  const b = await readJSON(req);
  if (!b || typeof b.type !== 'string') return fail(res, 400, 'bad_request', 'type');
  const inside = (x, y) => Number.isFinite(x) && Number.isFinite(y) && x >= 0 && y >= 0 && x < SW && y < SH;
  if (b.type === 'tap' || b.type === 'long') {
    if (!inside(b.x, b.y)) return fail(res, 400, 'bad_request', `x 0-${SW - 1}, y 0-${SH - 1}`);
    dots.push([Math.trunc(b.x), Math.trunc(b.y)]);
    if (dots.length > 20) dots.shift();
    screenOn = true;
  } else if (b.type === 'swipe') {
    if (!inside(b.x, b.y) || !inside(b.x2, b.y2)) return fail(res, 400, 'bad_request', 'x, y, x2, y2 inside the screen');
  } else if (b.type === 'button') {
    if (b.name !== 'power_menu' && b.name !== 'wake') return fail(res, 400, 'bad_request', 'name: power_menu or wake');
    screenOn = true;
  } else return fail(res, 400, 'bad_request', 'type: tap, long, swipe or button');
  lastInput = b;
  return ok(res);
}

// ---- release server for the simulator's desktop download (--dl) ----
async function dlRoute(req, res, path) {
  const name = decodeURIComponent(path.slice(4));
  const entry = { file: name, range: req.headers.range || null, status: 0, sent: 0 };
  dlLog.push(entry);
  const fault = dlFault;
  dlFault = null;
  if (fault && fault.status) { entry.status = fault.status; return fail(res, fault.status, 'fault'); }
  if (!name || name.includes('/') || name.startsWith('.')) { entry.status = 400; return fail(res, 400, 'bad_request'); }
  const file = join(DL, name);
  let st;
  try { st = await fsp.stat(file); } catch { entry.status = 404; return fail(res, 404, 'not_found'); }
  let start = 0, end = st.size - 1, status = 200;
  const m = req.headers.range && /bytes=(\d+)-(\d*)/.exec(req.headers.range);
  if (m) {
    start = +m[1];
    if (m[2]) end = Math.min(+m[2], st.size - 1);
    if (start >= st.size) { entry.status = 416; res.writeHead(416, { 'content-range': `bytes */${st.size}` }); return res.end(); }
    status = 206;
  }
  const headers = { 'content-type': 'application/zip', 'content-length': end - start + 1, 'accept-ranges': 'bytes' };
  if (status === 206) headers['content-range'] = `bytes ${start}-${end}/${st.size}`;
  entry.status = status;
  res.writeHead(status, headers);
  const data = (await fsp.readFile(file)).subarray(start, end + 1);
  if (fault && fault.cut_after) {
    // Send part of it, then drop the connection: a download cut off halfway.
    const part = data.subarray(0, Math.min(fault.cut_after, data.length));
    entry.sent = part.length;
    res.write(part, () => setTimeout(() => res.socket.destroy(), 50));
    return;
  }
  entry.sent = data.length;
  res.end(data);
}

// ---- P4 tools (docs/web-api.md, Tools) ----
const isInt = (v, lo, hi) => Number.isInteger(v) && v >= lo && v <= hi;
const isNum = (v, lo, hi) => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi;
const uptimeMs = () => Date.now() - BOOT;

// The log ring the Monitor app tails.
const logs = [];
let logN = 0;
function log(text) {
  logs.push({ n: ++logN, ms: uptimeMs(), text });
  if (logs.length > 200) logs.shift();
}

// The app open on the watch (POST /api/_dev/app): the watch UI wins, the tool answers 503 busy.
let watchApp = '';
const BUSY = { lora: ['LoRa', 'Walkie', 'Sub-G', 'LoRaWAN'], nfc: ['NFC', 'NFC Card', 'PN532'],
  audio: ['Recorder', 'Player', 'Net Radio', 'Walkie'], ble: ['BLE Scan', 'BLE HID'],
  hid: ['BadUSB'], net: ['Network Tools', 'Wi-Fi Analyzer'] };
function busy(res, tool) {
  if (!BUSY[tool].includes(watchApp)) return false;
  fail(res, 503, 'busy', `${watchApp} is open on the watch`, { app: watchApp });
  return true;
}

function monitor() {
  const t = uptimeMs() / 1000;
  return {
    uptime_s: Math.floor(t), cpu_mhz: 240, app: watchApp,
    battery: { ...battery, ma: battery.charging ? 320 : Math.round(-85 - 20 * Math.sin(t / 7)), temp_c: 27.5, vbus_mv: battery.charging ? 5020 : 0 },
    mem: { heap_free: 182000 + Math.round(6000 * Math.sin(t / 5)), heap_min: 150000, heap_total: 320000,
      psram_free: 6100000 - Math.round(40000 * (1 + Math.sin(t / 11))), psram_total: 8388608 },
  };
}

// Notifications shown on the watch.
let notes = [];
let noteSeq = 0;
function notifyRoute(b) {
  if (!b || typeof b !== 'object') return 'bad JSON';
  if (typeof b.title !== 'string' || !b.title || Buffer.byteLength(b.title) > 64) return 'title: 1-64 bytes';
  if (b.body !== undefined && (typeof b.body !== 'string' || Buffer.byteLength(b.body) > 512)) return 'body: up to 512 bytes';
  if (b.level !== undefined && !['info', 'warn', 'alert'].includes(b.level)) return 'level: info, warn or alert';
  if (b.vibrate !== undefined && typeof b.vibrate !== 'boolean') return 'vibrate: true or false';
  if (b.id !== undefined && (typeof b.id !== 'string' || !b.id || Buffer.byteLength(b.id) > 32)) return 'id: 1-32 bytes';
  const n = { id: b.id || `n${++noteSeq}`, title: b.title, body: b.body || '', level: b.level || 'info', ts: nowEpoch() };
  notes = [n, ...notes.filter((x) => x.id !== n.id)].slice(0, 20);
  publish('notify', n);
  log(`[notify] ${n.level}: ${n.title}${b.vibrate === false ? '' : ' (vibrate)'}`);
  return n;
}

// IMU: turning slowly, tilting a little; steps go up.
function sensors() {
  const t = uptimeMs() / 1000;
  const roll = 10 * Math.sin(t / 3), pitch = 5 * Math.cos(t / 4), heading = (t * 6) % 360;
  const r = (v, k = 10) => Math.round(v * k) / k, g = 9.81, rad = Math.PI / 180;
  return {
    imu: { roll: r(roll), pitch: r(pitch), heading: r(heading),
      accel: { x: r(-g * Math.sin(pitch * rad), 100), y: r(g * Math.sin(roll * rad), 100), z: r(g * Math.cos(roll * rad) * Math.cos(pitch * rad), 100) } },
    steps: 1234 + Math.floor(t / 2), seq: Math.floor(t * 10),
  };
}

// GNSS: a fix walking slowly around one spot. gnss.fix once a second while the desktop polls.
let gnssPolled = 0;
const SATS = [['GPS', 3], ['GPS', 8], ['GPS', 12], ['GPS', 17], ['GPS', 22], ['Galileo', 5], ['Galileo', 11], ['BeiDou', 24],
  ['GLONASS', 7], ['GPS', 30], ['SBAS', 123]];
function gnss() {
  const t = uptimeMs() / 1000;
  const satellites = SATS.map(([sys, prn], i) => ({
    sys, prn, el: 15 + ((i * 37) % 70), az: Math.round((i * 33 + t / 20) % 360), cn0: 22 + ((i * 7) % 24), used: i < 9 }));
  return {
    model: 'MIA-M10Q', fix: true, lat: +(52.2297 + 0.0004 * Math.sin(t / 60)).toFixed(6), lon: +(21.0122 + 0.0006 * Math.cos(t / 60)).toFixed(6),
    alt: 112.4, speed_kmh: 1.8, sats: satellites.filter((s) => s.used).length, time: nowEpoch(), ttff_ms: 31000, antenna: 'ok', satellites,
  };
}
setInterval(() => {
  if (Date.now() - gnssPolled > 10000) return;
  const g = gnss();
  publish('gnss.fix', { fix: g.fix, lat: g.lat, lon: g.lon, alt: g.alt, speed_kmh: g.speed_kmh, sats: g.sats, time: g.time });
}, 1000).unref();

// LoRa: a station on the air answers each packet with "ack: <text>" while listen is on.
let lora = { freq: 868.0, bw: 125, sf: 12, cr: 5, power: 13, sync_word: 205, listen: false };
const BWS = [7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500];
let loraLog = [];
let loraN = 0;
function loraPacket(dir, data, rssi = null, snr = null) {
  const p = { n: ++loraN, dir, ts: nowEpoch(), rssi, snr, len: data.length, data_b64: data.toString('base64') };
  loraLog.push(p);
  if (loraLog.length > 50) loraLog.shift();
  if (dir === 'rx') publish('lora.rx', p);
  return p;
}
function loraPatch(b) {
  if (!b || typeof b !== 'object' || Array.isArray(b)) return 'bad JSON';
  const next = { ...lora };
  for (const k of Object.keys(lora)) if (k in b) next[k] = b[k];
  if (!isNum(next.freq, 150, 960)) return 'freq: 150-960 MHz';
  if (!BWS.some((w) => Math.abs(w - next.bw) < 0.01)) return `bw: one of ${BWS.join(', ')}`;
  if (!isInt(next.sf, 5, 12)) return 'sf: 5-12';
  if (!isInt(next.cr, 5, 8)) return 'cr: 5-8';
  if (!isInt(next.power, -9, 22)) return 'power: -9 to 22 dBm';
  if (!isInt(next.sync_word, 0, 255)) return 'sync_word: 0-255';
  if (typeof next.listen !== 'boolean') return 'listen: true or false';
  next.bw = BWS.find((w) => Math.abs(w - next.bw) < 0.01);
  lora = next;
  log(`[lora] ${lora.freq} MHz BW ${lora.bw} SF${lora.sf} CR4/${lora.cr} ${lora.power} dBm sync 0x${lora.sync_word.toString(16)}${lora.listen ? ', listening' : ''}`);
  return null;
}
function packetData(b) {
  if (!b || typeof b !== 'object') return null;
  if (typeof b.text === 'string') return Buffer.from(b.text, 'utf8');
  if (typeof b.data_b64 === 'string' && /^[A-Za-z0-9+/]*={0,2}$/.test(b.data_b64)) return Buffer.from(b.data_b64, 'base64');
  return null;
}

// NFC: a scan finds a tag after a second.
const nfc = { scanning: false, until: 0, last: null, timers: [] };
function nfcTag(uid = '04:A2:3B:1C:5D:80', type = 'NTAG215', text) {
  const tag = { uid, type, ts: nowEpoch(), ndef: text !== undefined ? [{ kind: 'text', value: text }]
    : [{ kind: 'uri', value: 'https://www.lilygo.cc' }, { kind: 'text', value: 'Hello from the mock watch' }] };
  nfc.last = tag;
  publish('nfc.tag', tag);
  log(`[nfc] tag ${uid} ${type}`);
}
function nfcStop() {
  nfc.timers.forEach(clearTimeout);
  nfc.timers = [];
  if (!nfc.scanning) return;
  nfc.scanning = false;
  publish('nfc.state', { scanning: false });
  log('[nfc] reader off');
}

// Recorder: a 440 Hz tone with a changing level, written to the card at real-time pace.
const RATE = 16000;
const rec = { state: 'idle', path: null, abs: null, fd: null, start: 0, elapsed_ms: 0, max_s: 60, level: 0, bytes: 0, timer: null, phase: 0 };
function wavHeader(dataBytes, rate = RATE) {
  const b = Buffer.alloc(44);
  b.write('RIFF', 0); b.writeUInt32LE(36 + dataBytes, 4); b.write('WAVE', 8); b.write('fmt ', 12);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(rate, 24);
  b.writeUInt32LE(rate * 2, 28); b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(dataBytes, 40);
  return b;
}
const recState = () => ({ state: rec.state, path: rec.path, elapsed_ms: rec.elapsed_ms, max_s: rec.max_s, level: rec.state === 'recording' ? rec.level : 0 });
async function recChunk() {
  if (rec.state !== 'recording') return;
  const due = Math.min(Date.now() - rec.start, rec.max_s * 1000);
  const want = Math.floor(due * RATE / 1000) * 2;
  if (want > rec.bytes) {
    const n = (want - rec.bytes) / 2, pcm = Buffer.alloc(n * 2);
    const amp = 0.25 + 0.2 * Math.sin(due / 700);
    for (let i = 0; i < n; i++) { pcm.writeInt16LE(Math.round(32767 * amp * Math.sin(rec.phase)), i * 2); rec.phase += 2 * Math.PI * 440 / RATE; }
    rec.level = Math.round(amp * 200);
    await rec.fd.write(pcm, 0, pcm.length, 44 + rec.bytes);
    rec.bytes += pcm.length;
  }
  rec.elapsed_ms = due;
  if (due >= rec.max_s * 1000) return recStop();
  publish('recorder', recState());
}
async function recStop() {
  if (rec.state !== 'recording') return null;
  clearInterval(rec.timer);
  rec.state = 'stopping';
  await rec.fd.write(wavHeader(rec.bytes), 0, 44, 0);
  await rec.fd.close();
  rec.fd = null;
  rec.state = 'idle';
  rec.elapsed_ms = Math.round(rec.bytes / 2 / RATE * 1000);
  publish('recorder', recState());
  publish('files.changed', { op: 'write', path: rec.path });
  log(`[recorder] saved ${rec.path} (${rec.elapsed_ms} ms)`);
  return { path: rec.path, duration_ms: rec.elapsed_ms, size: rec.bytes + 44 };
}
function stamp() {
  const d = new Date((nowEpoch() + clock.offset_min * 60) * 1000).toISOString();
  return `${d.slice(0, 10).replace(/-/g, '')}-${d.slice(11, 19).replace(/:/g, '')}`;
}

// Player: "plays" for as long as the file lasts.
const audio = { playing: false, path: null, volume: 70, timer: null };
const audioState = () => ({ playing: audio.playing, path: audio.path, volume: audio.volume });
function audioStop() {
  clearTimeout(audio.timer);
  if (!audio.playing) return;
  audio.playing = false;
  publish('audio', audioState());
}

// What a watch app opening takes from the desktop.
function watchAppOpened() {
  if (BUSY.nfc.includes(watchApp)) nfcStop();
  if (BUSY.audio.includes(watchApp)) { recStop(); audioStop(); }
  if (BUSY.hid.includes(watchApp)) hidStop();
}

const WIFI = [['Home Network', 'a4:2b:b0:11:22:33', -48, 6, 'wpa2'], ['Home Network 5G', 'a4:2b:b0:11:22:34', -57, 36, 'wpa2/wpa3'],
  ['Cafe Guest', '00:1d:7e:40:aa:01', -71, 11, 'open'], ['Office', 'f0:9f:c2:10:20:30', -76, 1, 'wpa2-ent'],
  ['', 'f0:9f:c2:10:20:31', -80, 1, 'wpa2'], ["Neighbour's very long network name", '3c:84:6a:99:88:77', -86, 13, 'wpa3']];
const BLE = [['Mi Smart Band 8', 'c8:47:8c:01:02:03', -55, true, '5701a1b2', ['fee0']], ['', '6e:12:aa:bb:cc:dd', -78, false, '4c001005', []],
  ['T-Watch S3', 'dc:54:75:aa:bb:01', -62, true, '', ['1812', '180f']], ['JBL Flip 6', '30:c0:1b:22:33:44', -69, true, '', ['fe2c']],
  ['Thermo', 'a4:c1:38:55:66:77', -88, false, '', ['181a']]];

const wifiScan = { state: { scanning: false, ts: null, networks: [] } };
const bleScan = { state: { scanning: false, ts: null, devices: [] } };


// ================= P5 tools =================
const MAC = '24:6f:28:b2:c3:d4';
const MAC4 = MAC.replace(/:/g, '').slice(-4).toUpperCase();

// ---- Link: ESP-NOW file transfer ----
let link = { enabled: false, name: `T-Watch-${MAC4}`, mac: MAC, peers: [], transfer: null, timer: null };
const linkState = () => ({ enabled: link.enabled, mac: link.mac, name: link.name, peers: link.peers, transfer: link.transfer });
const linkPublish = () => publish('link', linkState());
function linkClearTransfer() { if (link.timer) { clearInterval(link.timer); link.timer = null; } }
async function uniqueLinkPath(name) {
  await fsp.mkdir(join(ROOTS.sd, 'data', 'link'), { recursive: true });
  let base = String(name).replace(/[/\\]/g, '_') || 'file';
  const dot = base.lastIndexOf('.'), stem = dot > 0 ? base.slice(0, dot) : base, ext = dot > 0 ? base.slice(dot) : '';
  for (let i = 2; existsSync(join(ROOTS.sd, 'data', 'link', base)); i++) base = `${stem} (${i})${ext}`;
  return `/sd/data/link/${base}`;
}
function linkRun(dir, peer, name, size) {
  linkClearTransfer();
  link.transfer = { dir, peer, name, size, done: 0, state: dir === 'tx' ? 'sending' : 'receiving', error: null };
  linkPublish();
  const step = Math.max(1, Math.round(size / 6));
  link.timer = setInterval(() => {
    if (!link.transfer) return clearInterval(link.timer);
    link.transfer.done = Math.min(size, link.transfer.done + step);
    if (link.transfer.done >= size) { link.transfer.state = 'done'; linkClearTransfer(); }
    linkPublish();
  }, 250);
}

// ---- USB HID: keyboard scripts (BadUSB) ----
let hid = { available: false, state: 'idle', script: '', line: 0, lines: 0, timer: null };
const hidState = () => ({ available: hid.available, state: hid.state, script: hid.script, line: hid.line, lines: hid.lines });
const hidPublish = () => publish('hid', hidState());
function hidStop() {
  if (hid.timer) { clearTimeout(hid.timer); hid.timer = null; }
  if (hid.state !== 'idle') { hid.state = 'idle'; hid.line = 0; hidPublish(); }
}
function hidRun(text, scriptName) {
  hidStop();
  const lines = text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('REM'));
  hid.script = scriptName || ''; hid.lines = lines.length; hid.line = 0; hid.state = 'armed';
  hidPublish();
  hid.timer = setTimeout(function tick() {              // arm ~0.2s, then one line every ~120ms
    hid.state = 'running';
    if (hid.line >= hid.lines) { hid.state = 'idle'; hid.line = 0; hid.timer = null; hidPublish(); return; }
    hid.line += 1; hidPublish();
    hid.timer = setTimeout(tick, 120);
  }, 200);
}

// ---- Wi-Fi tools: diagnostics + analyzer ----
let net = { connected: true, ssid: 'Home Network', ip: '192.168.1.50', gateway: '192.168.1.1',
  netmask: '255.255.255.0', dns: '192.168.1.1', rssi: -48, channel: 6, mac: MAC };
function fakeIp(host) { let h = 0; for (const c of String(host)) h = (h * 31 + c.charCodeAt(0)) & 0xffff; return `93.184.${(h >> 8) & 255}.${(h & 255) || 1}`; }
function netChannels() {
  const aps = WIFI.filter((w) => w[3] <= 14).map(([ssid, bssid, rssi, channel]) => ({ ssid, bssid, channel, rssi }));
  const channels = [];
  for (let ch = 1; ch <= 13; ch++) {
    let count = 0, score = 0;
    for (const a of aps) { const d = Math.abs(a.channel - ch); if (d <= 4) { if (d === 0) count += 1; score += Math.max(0, 5 - d); } }
    channels.push({ channel: ch, count, score });
  }
  const best = channels.reduce((b, c) => (c.score < b.score ? c : b), channels[0]).channel;
  return { best, channels, aps };
}

// ---- OTA ----
const OTA_MAX = 4 * 1024 * 1024;
let ota = { slot: 'app0', other_slot: 'app1', state: 'idle', progress: { done: 0, total: 0 }, error: null, can_rollback: true };
const otaState = () => ({ slot: ota.slot, other_slot: ota.other_slot, state: ota.state, progress: ota.progress, error: ota.error, can_rollback: ota.can_rollback });

async function toolsRoute(req, res, url, path) {
  const m = req.method;
  if (path === '/api/monitor' && m === 'GET') return json(res, 200, monitor());
  if (path === '/api/logs' && m === 'GET') {
    const after = Number(url.searchParams.get('after') || 0);
    return json(res, 200, { lines: logs.filter((l) => l.n > after), next: logN });
  }
  if (path === '/api/notify' && m === 'GET') return json(res, 200, { items: notes });
  if (path === '/api/notify' && m === 'POST') {
    const n = notifyRoute(await readJSON(req));
    return typeof n === 'string' ? fail(res, 400, 'bad_request', n) : ok(res, { id: n.id });
  }
  if (path === '/api/sensors' && m === 'GET') return json(res, 200, sensors());
  if (path === '/api/gnss' && m === 'GET') { gnssPolled = Date.now(); return json(res, 200, gnss()); }

  if (path.startsWith('/api/lora/')) {
    if (busy(res, 'lora')) return;
    if (path === '/api/lora/config' && m === 'GET') return json(res, 200, lora);
    if (path === '/api/lora/config' && m === 'PATCH') {
      const e = loraPatch(await readJSON(req));
      return e ? fail(res, 400, 'bad_request', e) : json(res, 200, lora);
    }
    if (path === '/api/lora/send' && m === 'POST') {
      const data = packetData(await readJSON(req));
      if (!data || data.length < 1 || data.length > 255) return fail(res, 400, 'bad_request', 'text or data_b64, 1-255 bytes');
      loraPacket('tx', data);
      if (lora.listen) {
        setTimeout(() => { if (lora.listen && !BUSY.lora.includes(watchApp)) loraPacket('rx', Buffer.concat([Buffer.from('ack: '), data]).subarray(0, 255), -61, 9.5); }, 1000);
      }
      return ok(res, { len: data.length });
    }
    if (path === '/api/lora/log' && m === 'GET') return json(res, 200, { packets: loraLog });
    return fail(res, 404, 'not_found');
  }

  if (path === '/api/nfc' && m === 'GET') return json(res, 200, { scanning: nfc.scanning, last: nfc.last });
  if (path === '/api/nfc/scan' && m === 'POST') {
    if (busy(res, 'nfc')) return;
    const b = await readJSON(req);
    if (!b || typeof b !== 'object') return fail(res, 400, 'bad_request', 'bad JSON');
    const secs = b.seconds === undefined ? 20 : b.seconds;
    if (!isInt(secs, 1, 60)) return fail(res, 400, 'bad_request', 'seconds: 1-60');
    nfcStop();
    nfc.scanning = true;
    nfc.until = Date.now() + secs * 1000;
    publish('nfc.state', { scanning: true });
    log(`[nfc] reader on for ${secs} s`);
    nfc.timers.push(setTimeout(() => { if (nfc.scanning) nfcTag(); }, 1000), setTimeout(nfcStop, secs * 1000));
    return ok(res);
  }
  if (path === '/api/nfc/stop' && m === 'POST') { if (busy(res, 'nfc')) return; nfcStop(); return ok(res); }

  if (path === '/api/recorder' && m === 'GET') return json(res, 200, recState());
  if (path === '/api/recorder/start' && m === 'POST') {
    if (busy(res, 'audio')) return;
    const b = await readJSON(req);
    if (!b || typeof b !== 'object') return fail(res, 400, 'bad_request', 'bad JSON');
    if (rec.state !== 'idle') return fail(res, 409, 'conflict', 'already recording');
    const max = b.max_s === undefined ? 60 : b.max_s;
    if (!isInt(max, 1, 600)) return fail(res, 400, 'bad_request', 'max_s: 1-600');
    const p = b.path === undefined ? `/sd/data/recordings/rec-${stamp()}.wav` : b.path;
    if (typeof p !== 'string' || !/\.wav$/i.test(p)) return fail(res, 400, 'bad_request', 'path must end in .wav');
    const r = resolvePath(p); if (r.error) return pathError(res, r.error);
    await fsp.mkdir(dirname(r.abs), { recursive: true });
    Object.assign(rec, { state: 'recording', path: p, abs: r.abs, start: Date.now(), elapsed_ms: 0, max_s: max, level: 0, bytes: 0, phase: 0 });
    rec.fd = await fsp.open(r.abs, 'w');
    await rec.fd.write(wavHeader(0), 0, 44, 0);
    rec.timer = setInterval(() => recChunk().catch(() => {}), 250);
    publish('recorder', recState());
    log(`[recorder] recording ${p}`);
    return ok(res, { path: p });
  }
  if (path === '/api/recorder/stop' && m === 'POST') {
    if (rec.state !== 'recording') return fail(res, 409, 'conflict', 'not recording');
    await recChunk();
    const done = await recStop();
    return done ? ok(res, done) : fail(res, 409, 'conflict', 'not recording');
  }

  if (path === '/api/audio' && m === 'GET') return json(res, 200, audioState());
  if (path === '/api/audio' && m === 'PATCH') {
    const b = await readJSON(req);
    if (!b || !isNum(b.volume, 0, 100)) return fail(res, 400, 'bad_request', 'volume: 0-100');
    audio.volume = Math.round(b.volume);
    publish('audio', audioState());
    return json(res, 200, audioState());
  }
  if (path === '/api/audio/play' && m === 'POST') {
    if (busy(res, 'audio')) return;
    const b = await readJSON(req);
    const r = resolvePath(b && b.path); if (r.error) return pathError(res, r.error);
    let st; try { st = await fsp.stat(r.abs); if (!st.isFile()) throw 0; } catch { return fail(res, 404, 'not_found'); }
    const ext = extname(r.abs).toLowerCase();
    if (ext !== '.wav' && ext !== '.mp3') return fail(res, 400, 'bad_request', 'WAV or MP3 only');
    let ms = 3000;
    if (ext === '.wav') {
      try {
        const fh = await fsp.open(r.abs); const hd = Buffer.alloc(44); await fh.read(hd, 0, 44, 0); await fh.close();
        const rate = hd.readUInt32LE(28);
        if (rate > 0) ms = Math.max(200, Math.round((st.size - 44) / rate * 1000));
      } catch {}
    }
    audioStop();
    Object.assign(audio, { playing: true, path: b.path });
    audio.timer = setTimeout(audioStop, ms);
    publish('audio', audioState());
    log(`[audio] playing ${b.path}`);
    return ok(res);
  }
  if (path === '/api/audio/stop' && m === 'POST') { audioStop(); return ok(res); }

  if (path === '/api/wifi/scan' && m === 'GET') return json(res, 200, wifiScan.state);
  if (path === '/api/wifi/scan' && m === 'POST') {
    if (!wifiScan.state.scanning) {
      wifiScan.state.scanning = true;
      log('[wifi] scan');
      setTimeout(() => {
        const networks = WIFI.map(([ssid, bssid, rssi, channel, auth]) => ({ ssid, bssid, rssi: rssi + Math.round(Math.random() * 4 - 2), channel, auth }));
        wifiScan.state = { scanning: false, ts: nowEpoch(), networks: networks.sort((x, y) => y.rssi - x.rssi) };
        publish('wifi.scan', wifiScan.state);
      }, 1000);
    }
    return json(res, 202, { ok: true });
  }
  if (path === '/api/ble/scan' && m === 'GET') return json(res, 200, bleScan.state);
  if (path === '/api/ble/scan' && m === 'POST') {
    if (busy(res, 'ble')) return;
    const b = await readJSON(req);
    if (!b || typeof b !== 'object') return fail(res, 400, 'bad_request', 'bad JSON');
    const secs = b.seconds === undefined ? 5 : b.seconds;
    if (!isInt(secs, 1, 15)) return fail(res, 400, 'bad_request', 'seconds: 1-15');
    if (!bleScan.state.scanning) {
      bleScan.state.scanning = true;
      log(`[ble] scan ${secs} s`);
      setTimeout(() => {
        const devices = BLE.map(([name, addr, rssi, connectable, mfr, services]) => ({ addr, name, rssi: rssi + Math.round(Math.random() * 4 - 2), connectable, mfr, services }));
        bleScan.state = { scanning: false, ts: nowEpoch(), devices: devices.sort((x, y) => y.rssi - x.rssi) };
        publish('ble.scan', bleScan.state);
      }, Math.min(secs * 1000, 1000));
    }
    return json(res, 202, { ok: true });
  }

  // ---- Link ----
  if (path === '/api/link' && m === 'GET') return json(res, 200, linkState());
  if (path === '/api/link/enable' && m === 'POST') {
    const b = (await readJSON(req)) || {};
    if (typeof b.on !== 'boolean') return fail(res, 400, 'bad_request', 'on: true or false');
    link.enabled = b.on;
    if (!link.enabled) { link.peers = []; linkClearTransfer(); link.transfer = null; }
    log(`[link] ${link.enabled ? 'on' : 'off'}`);
    linkPublish();
    return ok(res, { enabled: link.enabled });
  }
  if (path === '/api/link/send' && m === 'POST') {
    const b = (await readJSON(req)) || {};
    if (!link.enabled) return fail(res, 409, 'conflict', 'link is off');
    if (link.transfer && link.transfer.state !== 'done' && link.transfer.state !== 'error') return fail(res, 409, 'conflict', 'a transfer is in flight');
    const to = String(b.to_mac || '');
    const broadcast = to.toLowerCase() === 'ff:ff:ff:ff:ff:ff';
    if (!broadcast && !link.peers.some((pr) => pr.mac === to)) return fail(res, 404, 'not_found', 'no such peer');
    const r = resolvePath(b && b.path); if (r.error) return pathError(res, r.error);
    let st; try { st = await fsp.stat(r.abs); if (!st.isFile()) throw 0; } catch { return fail(res, 404, 'not_found'); }
    const name = b.path.split('/').pop();
    log(`[link] sending ${name} (${st.size} B) to ${broadcast ? 'everyone' : to}`);
    linkRun('tx', to, name, st.size);
    return json(res, 202, { ok: true });
  }
  if (path === '/api/link/cancel' && m === 'POST') {
    linkClearTransfer();
    if (link.transfer && link.transfer.state !== 'done') { link.transfer.state = 'error'; link.transfer.error = 'cancelled'; linkPublish(); }
    return ok(res);
  }

  // ---- USB HID (BadUSB) ----
  if (path === '/api/hid' && m === 'GET') return json(res, 200, hidState());
  if (path === '/api/hid/type' && m === 'POST') {
    if (busy(res, 'hid')) return;
    if (!hid.available) return fail(res, 503, 'hid_unavailable', 'no USB host');
    const b = (await readJSON(req)) || {};
    if (typeof b.text !== 'string' || b.text.length < 1 || b.text.length > 1024) return fail(res, 400, 'bad_request', 'text: 1-1024 chars');
    log(`[hid] type ${b.text.length} chars`);
    return ok(res);
  }
  if (path === '/api/hid/run' && m === 'POST') {
    if (busy(res, 'hid')) return;
    if (!hid.available) return fail(res, 503, 'hid_unavailable', 'no USB host');
    if (hid.state !== 'idle') return fail(res, 409, 'conflict', 'already running');
    const b = (await readJSON(req)) || {};
    let text, name = '';
    if (typeof b.path === 'string') {
      if (!/\.(txt|duck|ducky)$/i.test(b.path)) return fail(res, 400, 'bad_request', 'script must be .txt/.duck/.ducky');
      const r = resolvePath(b.path); if (r.error) return pathError(res, r.error);
      try { text = await fsp.readFile(r.abs, 'utf8'); } catch { return fail(res, 404, 'not_found'); }
      name = b.path.split('/').pop();
    } else if (typeof b.text === 'string') {
      if (b.text.length > 16384) return fail(res, 400, 'bad_request', 'script up to 16 KB');
      text = b.text;
    } else return fail(res, 400, 'bad_request', 'path or text');
    log(`[hid] run ${name || 'inline script'}`);
    hidRun(text, name);
    return json(res, 202, { ok: true });
  }
  if (path === '/api/hid/stop' && m === 'POST') { hidStop(); return ok(res); }

  // ---- Wi-Fi tools ----
  if (path === '/api/net' && m === 'GET') { if (busy(res, 'net')) return; return json(res, 200, net); }
  if (path === '/api/net/resolve' && m === 'POST') {
    if (busy(res, 'net')) return;
    if (!net.connected) return fail(res, 409, 'offline', 'WiFi is disconnected');
    const b = (await readJSON(req)) || {};
    if (typeof b.host !== 'string' || !b.host) return fail(res, 400, 'bad_request', 'host');
    const found = b.host.includes('.');
    return json(res, 200, { ok: found, ip: found ? fakeIp(b.host) : null, ms: 12 + Math.floor(Math.random() * 30) });
  }
  if (path === '/api/net/tcp' && m === 'POST') {
    if (busy(res, 'net')) return;
    const b = (await readJSON(req)) || {};
    if (!isInt(b.port, 1, 65535)) return fail(res, 400, 'bad_request', 'port: 1-65535');
    if (typeof b.host !== 'string' || !b.host) return fail(res, 400, 'bad_request', 'host');
    if (!net.connected) return fail(res, 409, 'offline', 'WiFi is disconnected');
    const open = [22, 80, 443].includes(b.port);
    return json(res, 200, { ok: open, ms: 18 + Math.floor(Math.random() * 40) });
  }
  if (path === '/api/net/channels' && m === 'GET') { if (busy(res, 'net')) return; return json(res, 200, netChannels()); }

  // ---- OTA ----
  if (path === '/api/ota' && m === 'GET') return json(res, 200, otaState());
  if (path === '/api/ota/upload' && m === 'POST') {
    const data = await readBody(req);
    if (data.length < 8) { ota.state = 'error'; ota.error = 'bad_image'; publish('ota', otaState()); return fail(res, 400, 'bad_image', 'not a firmware image'); }
    if (data.length > OTA_MAX) { ota.state = 'error'; ota.error = 'too large'; publish('ota', otaState()); return fail(res, 413, 'too_large', 'image is larger than the slot'); }
    ota.error = null; ota.state = 'writing'; ota.progress = { done: 0, total: data.length }; publish('ota', otaState());
    ota.progress = { done: data.length, total: data.length }; ota.state = 'ready'; publish('ota', otaState());
    log(`[ota] wrote ${data.length} B to ${ota.other_slot}`);
    return ok(res, { size: data.length });
  }
  if (path === '/api/ota/commit' && m === 'POST') {
    if (ota.state !== 'ready') return fail(res, 409, 'conflict', 'nothing uploaded');
    log('[ota] commit: the watch would reboot into the new slot now');
    ota.state = 'idle'; ota.progress = { done: 0, total: 0 }; publish('ota', otaState());
    return ok(res);
  }
  if (path === '/api/ota/rollback' && m === 'POST') {
    if (!ota.can_rollback) return fail(res, 409, 'conflict', 'nothing to roll back to');
    log('[ota] rollback: the watch would reboot into the other slot now');
    return ok(res);
  }

  // P9 dev hooks
  if (DEV && path === '/api/_dev/evilportal' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.action === 'creds') {
      evilportal.captures.push({ mac: j.mac || '00:00:00:00:00:01', hostname: '', url: j.url || '/login', username: j.username || 'user', password: j.password || 'pass', ts: nowEpoch() });
      evilportal.attempts = evilportal.captures.length;
    }
    return ok(res);
  }
  if (DEV && path === '/api/_dev/sentinel' && m === 'POST') {
    const j = await readJSON(req);
    sentinel.devices.push({ addr: j?.addr || '00:00:00:00:00:01', name: j?.name || 'Phone', rssi: j?.rssi || -60, first_seen: nowEpoch(), last_seen: nowEpoch(), sightings: 3, stalking: false });
    sentinel.device_count = sentinel.devices.length;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/sniffer' && m === 'POST') {
    sniffer.packets += 50; sniffer.bytes += 10000;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/deauthflood' && m === 'POST') {
    deauthflood.active = true; deauthflood.sent = 100;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/beaconspam' && m === 'POST') {
    beaconspam.active = true; beaconspam.beacons = 200;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/airspace' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.type === 'deauth_flood') { airspace.deauth_floods++; airspace.deauth_total += 100; }
    if (j?.type === 'evil_twin') { airspace.evil_twins++; }
    return ok(res);
  }

  // === P9 NucleoOS parity tools ===
  // Wi-Fi Sniffer
  if (path === '/api/sniffer' && m === 'GET') return json(res, 200, { active: sniffer.active === true, channel: sniffer.channel || null, packets: sniffer.packets || 0, bytes: sniffer.bytes || 24, file: sniffer.file || null });
  if (path === '/api/sniffer/start' && m === 'POST') {
    if (sniffer.active) return fail(res, 409, 'conflict', 'already capturing');
    sniffer = { active: true, packets: 0, bytes: 24, channel: null, file: '/sd/data/sniffer/capture.pcap' };
    setTimeout(() => { sniffer.packets++; sniffer.bytes += 200; }, 500);
    return ok(res);
  }
  if (path === '/api/sniffer/stop' && m === 'POST') { sniffer.active = false; return ok(res); }

  // Airspace
  if (path === '/api/airspace' && m === 'GET') return json(res, 200, airspaceState());
  if (path === '/api/airspace/start' && m === 'POST') { airspace.active = true; airspace.started = nowEpoch(); return ok(res); }
  if (path === '/api/airspace/stop' && m === 'POST') { airspace.active = false; return ok(res); }

  // Sentinel
  if (path === '/api/sentinel' && m === 'GET') return json(res, 200, sentinelState());
  if (path === '/api/sentinel/start' && m === 'POST') { sentinel.active = true; sentinel.started = nowEpoch(); sentinel.devices = []; return ok(res); }
  if (path === '/api/sentinel/stop' && m === 'POST') { sentinel.active = false; return ok(res); }

  // Presence
  if (path === '/api/presence' && m === 'GET') return json(res, 200, presenceState());
  if (path === '/api/presence/start' && m === 'POST') { presence.active = true; presence.started = nowEpoch(); presence.peers = []; presence.history = []; return ok(res); }
  if (path === '/api/presence/stop' && m === 'POST') { presence.active = false; return ok(res); }

  // FIDO2
  if (path === '/api/fido2' && m === 'GET') return json(res, 200, fido2State());
  if (path === '/api/fido2/register' && m === 'POST') { const j = await readJSON(req); fido2.creds.push({ rp: j?.rp || '?', user: j?.user || '?' }); fido2.credentials = fido2.creds.length; return ok(res); }
  if (path === '/api/fido2/authenticate' && m === 'POST') { return ok(res); }
  if (path === '/api/fido2/credentials' && m === 'GET') return json(res, 200, { credentials: fido2.creds.map((c,i) => ({ index: i, rp: c.rp, user: c.user })) });
  if (path === '/api/fido2/remove' && m === 'POST') { const i = (await readJSON(req))?.index; if (i >= 0 && i < fido2.creds.length) { fido2.creds.splice(i, 1); fido2.credentials = fido2.creds.length; } return ok(res); }

  // Evil Portal
  if (path === '/api/evilportal' && m === 'GET') return json(res, 200, evilportalState());
  if (path === '/api/evilportal/start' && m === 'POST') { const j = await readJSON(req); evilportal.active = true; evilportal.ssid = j?.ssid || 'Free Wi-Fi'; return ok(res); }
  if (path === '/api/evilportal/stop' && m === 'POST') { evilportal.active = false; return ok(res); }
  if (path === '/api/evilportal/captures' && m === 'GET') return json(res, 200, { captures: evilportal.captures });
  if (path === '/api/evilportal/clear' && m === 'POST') { evilportal.captures = []; return ok(res); }

  // Deauth flood
  if (path === '/api/deauthflood' && m === 'GET') return json(res, 200, deauthfloodState());
  if (path === '/api/deauthflood/start' && m === 'POST') {
    const b = await readJSON(req);
    deauthflood.active = true;
    deauthflood.target_bssid = b?.bssid || '';
    deauthflood.target_station = b?.station || 'ff:ff:ff:ff:ff:ff';
    deauthflood.channel = b?.channel || 1;
    deauthflood.sent = 0;
    return ok(res);
  }
  if (path === '/api/deauthflood/stop' && m === 'POST') { deauthflood.active = false; return ok(res); }

  // Beacon spam
  if (path === '/api/beaconspam' && m === 'GET') return json(res, 200, beaconspamState());
  if (path === '/api/beaconspam/start' && m === 'POST') {
    const b = await readJSON(req);
    beaconspam.active = true;
    beaconspam.prefix = b?.prefix || 'FreeNet';
    beaconspam.ssids = b?.count || 20;
    beaconspam.channel = b?.channel || 0;
    beaconspam.beacons = 0;
    return ok(res);
  }
  if (path === '/api/beaconspam/stop' && m === 'POST') { beaconspam.active = false; return ok(res); }

  return null;
}

// ---- router ----
const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://watch.local');
  const path = url.pathname;
  try {
    if (DL && path.startsWith('/dl/')) return dlRoute(req, res, path);
    if (DEV && path === '/api/_dev/dl' && req.method === 'POST') { dlFault = (await readJSON(req)) || null; return ok(res); }
    if (DEV && path === '/api/_dev/dl-log') return json(res, 200, dlLog);
    if (!path.startsWith('/api/')) {
      if (req.method !== 'GET' && req.method !== 'HEAD') return fail(res, 405, 'bad_request', 'GET');
      return staticRoute(req, res, path);
    }
    // public
    if (path === '/api/auth' && req.method === 'GET') return json(res, 200, { paired: isPaired(req), method: 'pin' });
    if (path === '/api/auth/pair' && req.method === 'POST') {
      const now = Date.now();
      if (now < lockedUntil) return fail(res, 429, 'locked', 'too many tries', { retry_ms: lockedUntil - now });
      if (!pairingOpen) return fail(res, 403, 'pairing_closed', 'Open Settings > Web desktop on the watch to see the PIN');
      const b = await readJSON(req);
      if (b && String(b.pin || '') === PIN) {
        misses = 0;
        const t = randomBytes(24).toString('hex');
        sessions.add(t);
        log('[web] a browser paired');
        return json(res, 200, { ok: true },
          { 'set-cookie': `session=${t}; Path=/; Max-Age=7776000; HttpOnly; SameSite=Strict` });
      }
      if (++misses >= 5) { misses = 0; lockedUntil = now + 30000; }
      return fail(res, 401, 'bad_pin', 'wrong PIN');
    }
    if (path === '/api/caps' && req.method === 'GET') return json(res, 200, { api: 1, board: 'twatch-ultra', fw: FW, caps: CAPS });
    if (path === '/api/device' && req.method === 'GET') return json(res, 200, device(isPaired(req)));
    if (DEV && path === '/api/_dev/pin') {
      if (!pairingOpen) { PIN = String(randomInt(0, 1000000)).padStart(6, '0'); pairingOpen = true; misses = 0; lockedUntil = 0; }
      return json(res, 200, { pin: PIN });
    }
    if (DEV && path === '/api/_dev/pairing' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      const open = b.open !== false;
      if (open && !pairingOpen) PIN = String(randomInt(0, 1000000)).padStart(6, '0');
      pairingOpen = open;
      return ok(res);
    }
    if (DEV && path === '/api/_dev/input') return json(res, 200, lastInput);
    if (DEV && path === '/api/_dev/theme' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      if (!PRESETS[b.preset]) return fail(res, 400, 'bad_request', 'preset');
      theme = PRESETS[b.preset];
      publish('theme', theme);
      return ok(res);
    }
    if (DEV && path === '/api/_dev/app' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      if (typeof b.name !== 'string') return fail(res, 400, 'bad_request', 'name');
      watchApp = b.name;
      watchAppOpened();
      return ok(res);
    }
    if (DEV && path === '/api/_dev/lora' && req.method === 'POST') {
      const b = await readJSON(req);
      const data = packetData(b);
      if (!data || data.length < 1 || data.length > 255) return fail(res, 400, 'bad_request', 'text or data_b64, 1-255 bytes');
      const heard = lora.listen && !BUSY.lora.includes(watchApp);
      if (heard) loraPacket('rx', data, Number.isFinite(b.rssi) ? b.rssi : -70, Number.isFinite(b.snr) ? b.snr : 7.25);
      return ok(res, { received: heard });
    }
    if (DEV && path === '/api/_dev/nfc' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      if (nfc.scanning) nfcTag(b.uid || undefined, b.type || undefined, b.text);
      return ok(res, { read: nfc.scanning });
    }
    if (DEV && path === '/api/_dev/link' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      if (b.rx && typeof b.rx === 'object') {                 // a watch sends this one a file
        if (!sd.mounted) return fail(res, 503, 'sd_missing', 'no card');
        const size = isInt(b.rx.size, 0, 1 << 30) ? b.rx.size : 1024;
        const apiPath = await uniqueLinkPath(b.rx.name || 'file.bin');
        await fsp.writeFile(resolvePath(apiPath).abs, Buffer.alloc(size));
        publish('files.changed', { op: 'write', path: apiPath });
        linkRun('rx', (b.peer && b.peer.mac) || 'ff:ff:ff:ff:ff:ff', apiPath.split('/').pop(), size);
        return ok(res, { path: apiPath });
      }
      const given = typeof b.peer === 'string' ? { name: b.peer } : (b.peer || {});
      const randMac = () => Array.from({ length: 6 }, () => Math.floor(Math.random() * 256).toString(16).padStart(2, '0')).join(':');
      const peer = Object.assign({ mac: randMac(), name: 'T-Watch-7E4F', rssi: -57, last_seen: nowEpoch() }, given);
      if (!link.peers.some((pr) => pr.mac === peer.mac)) link.peers.push(peer);
      linkPublish();
      return ok(res, { peers: link.peers.length, mac: peer.mac });
    }
    if (DEV && path === '/api/_dev/hid' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      hid.available = b.available !== false;
      if (!hid.available) hidStop();
      hidPublish();
      return ok(res, { available: hid.available });
    }
    if (DEV && path === '/api/_dev/sd' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      sd.present = !!b.present; sd.mounted = sd.present; sd.error = null;
      publish('sd.changed', sdStatus());
      return ok(res, sdStatus());
    }

    // paired
    if (!isPaired(req)) return fail(res, 401, 'unpaired', 'pair with the PIN on the watch');
    if (path === '/api/auth/logout' && req.method === 'POST') {
      const b = (await readJSON(req)) || {};
      const mine = tokenOf(req); let revoked = 0;
      for (const t of [...sessions]) if (b.all || t === mine) { sessions.delete(t); revoked++; }
      return ok(res, { revoked });
    }
    if (path === '/api/time' && req.method === 'POST') {
      const b = await readJSON(req);
      if (!b || !Number.isFinite(b.epoch) || b.epoch < 0) return fail(res, 400, 'bad_request', 'epoch');
      if ('offset_min' in b && !(Number.isFinite(b.offset_min) && Math.abs(b.offset_min) <= 840)) return fail(res, 400, 'bad_request', 'offset_min');
      clock = { offset_ms: b.epoch * 1000 - Date.now(), tz: typeof b.tz === 'string' && b.tz ? b.tz : clock.tz,
        offset_min: 'offset_min' in b ? Math.trunc(b.offset_min) : clock.offset_min, synced: true };
      return ok(res);
    }
    if (path === '/api/settings' && req.method === 'GET') return json(res, 200, settings);
    if (path === '/api/settings' && req.method === 'PATCH') {
      const b = await readJSON(req);
      if (!b || typeof b !== 'object') return fail(res, 400, 'bad_request', 'bad JSON');
      const next = { ...settings };
      for (const k of Object.keys(settings)) if (k in b) {
        if (typeof b[k] !== typeof settings[k]) return fail(res, 400, 'bad_request', `bad ${k}`);
        next[k] = b[k];
      }
      if (next.brightness < 0 || next.brightness > 100) return fail(res, 400, 'bad_request', 'brightness 0-100');
      if (next.screen_timeout_s < 0 || next.screen_timeout_s > 255) return fail(res, 400, 'bad_request', 'screen_timeout_s 0-255');
      if (next.web_idle_off_s < 0 || next.web_idle_off_s > 86400) return fail(res, 400, 'bad_request', 'web_idle_off_s 0-86400');
      settings = next;
      publish('settings.changed', settings);
      log(`[settings] ${JSON.stringify(settings)}`);
      return json(res, 200, settings);
    }
    if (path === '/api/system/reboot' && req.method === 'POST') return ok(res);
    if (path === '/api/sd' && req.method === 'GET') return json(res, 200, sdStatus());
    if (path === '/api/sd/unmount' && req.method === 'POST') { sd.mounted = false; publish('sd.changed', sdStatus()); return ok(res); }
    if (path === '/api/sd/mount' && req.method === 'POST') {
      if (!sd.present) return fail(res, 503, 'sd_missing', 'no card');
      sd.mounted = true; publish('sd.changed', sdStatus()); return ok(res);
    }
    if (path === '/api/files' || path.startsWith('/api/files/')) return filesRoute(req, res, url, path);
    if (path === '/api/screen' && req.method === 'GET') return screenRoute(req, res, url);
    if (path === '/api/input' && req.method === 'POST') return inputRoute(req, res);
    if (await toolsRoute(req, res, url, path) !== null || res.headersSent || res.writableEnded) return;
    if (path === '/api/display' && req.method === 'POST') {
      const b = await readJSON(req);
      if (!b || typeof b !== 'object') return fail(res, 400, 'bad_request', 'bad JSON');
      if ('brightness' in b && !(Number.isFinite(b.brightness) && b.brightness >= 0 && b.brightness <= 100)) return fail(res, 400, 'bad_request', 'brightness 0-100');
      if ('on' in b) screenOn = !!b.on;
      return ok(res);
    }
    return fail(res, 404, 'not_found');
  } catch (e) {
    if (!res.headersSent) fail(res, 500, 'internal', String(e && e.message || e));
  }
});

server.on('upgrade', (req, sock) => {
  const url = new URL(req.url, 'http://watch.local');
  if (url.pathname !== '/api/events' || !isPaired(req)) { sock.end('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n'); return; }
  const accept = createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  sock.on('error', () => {});
  if (desktop) {                                                   // one desktop at a time: the newest wins
    const old = desktop;
    try { old.write(wsFrame(JSON.stringify({ t: 'seat.lost', seq: ++seq, d: {} }))); old.end(); } catch {}
  }
  desktop = sock;
  sock.write(wsFrame(JSON.stringify({ t: 'hello', seq: 0, d: { api: 1, caps: CAPS } })));
  sock.on('data', (c) => { if (c.length && (c[0] & 0x0f) === 0x8) sock.destroy(); });
  sock.on('close', () => { if (desktop === sock) desktop = null; });
});

// slow battery drain, so the desktop sees battery events
setInterval(() => {
  if (battery.percent > 5) { battery = { ...battery, percent: battery.percent - 1, mv: battery.mv - 6 }; publish('battery', battery); }
}, 60000).unref();

server.listen(PORT, () => {
  log(`[web] server on port ${PORT}`);
  console.log(`mock T-Watch Ultra (web API v1): http://localhost:${PORT}`);
  console.log(`storage: ${ROOT}   desktop: ${existsSync(WEB) ? WEB : '(rescue page)'}`);
  console.log(`pairing PIN: ${PIN}${DEV ? '  (also at /api/_dev/pin)' : ''}`);
});
