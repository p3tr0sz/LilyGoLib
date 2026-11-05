#!/usr/bin/env node
// Mock T-Watch Ultra: the web API v1 of docs/web-api.md (P1 routes) on the PC, so the desktop can be built and
// tested without flashing. The firmware is checked against the same behaviour with web_api_test.mjs.
//
// Usage:
//   node web_mock_device.mjs [--root <dir>] [--web <desktop dir>] [--port 5620] [--pin 123456] [--dev]
//     --root  folder holding the fake storage: <root>/sd (SD card) and <root>/flash (internal FFat);
//             default <tmp>/mock-twatch
//     --web   desktop to serve at / (default: examples/factory/web if present, else a placeholder page)
//     --dev   adds test hooks the watch never has:
//               GET  /api/_dev/pin               the pairing PIN
//               POST /api/_dev/sd {present:bool}  insert or remove the SD card
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
const DEV = args.includes('--dev');
const PIN = opt('--pin', String(randomInt(0, 1000000)).padStart(6, '0'));
const FW = '0.1.0+web.mock';
const BOOT = Date.now();
const ROOTS = { sd: join(ROOT, 'sd'), flash: join(ROOT, 'flash') };
for (const d of Object.values(ROOTS)) mkdirSync(d, { recursive: true });

const CAPS = ['files', 'files.flash', 'sd', 'settings', 'time'];

// ---- state ----
const sd = { present: true, mounted: true, error: null };
let settings = { brightness: 60, screen_timeout_s: 15, lang: 'en', tz: 'UTC', web_idle_off_s: 600 };
let clock = { offset_ms: 0, tz: 'UTC', synced: false };
let battery = { percent: 76, charging: false, mv: 3968 };

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
    time: { epoch: nowEpoch(), tz: clock.tz, synced: clock.synced },
    mem: { heap_free: 182000, psram_free: 6100000 },
  };
  return paired ? full : { board: full.board, fw: full.fw, battery: full.battery, sd: { present: sd.present } };
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
const PLACEHOLDER = `<!doctype html><meta charset="utf-8"><title>T-Watch Ultra</title>
<body style="font:16px system-ui;margin:2rem"><h1>T-Watch Ultra</h1><p>No desktop installed. API v1 is up: <a href="/api/caps">/api/caps</a></p>`;
async function staticRoute(req, res, path) {
  if (path.split('/').includes('..')) return fail(res, 400, 'bad_request');
  const dir = await desktopDir();
  if (!dir) {
    if (path === '/' || path === '/index.html') { res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }); return res.end(PLACEHOLDER); }
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

// ---- router ----
const server = createServer(async (req, res) => {
  const url = new URL(req.url, 'http://watch.local');
  const path = url.pathname;
  try {
    if (!path.startsWith('/api/')) {
      if (req.method !== 'GET' && req.method !== 'HEAD') return fail(res, 405, 'bad_request', 'GET');
      return staticRoute(req, res, path);
    }
    // public
    if (path === '/api/auth' && req.method === 'GET') return json(res, 200, { paired: isPaired(req), method: 'pin' });
    if (path === '/api/auth/pair' && req.method === 'POST') {
      const now = Date.now();
      if (now < lockedUntil) return fail(res, 429, 'locked', 'too many tries', { retry_ms: lockedUntil - now });
      const b = await readJSON(req);
      if (b && String(b.pin || '') === PIN) {
        misses = 0;
        const t = randomBytes(24).toString('hex');
        sessions.add(t);
        return json(res, 200, { ok: true },
          { 'set-cookie': `session=${t}; Path=/; Max-Age=7776000; HttpOnly; SameSite=Strict` });
      }
      if (++misses >= 5) { misses = 0; lockedUntil = now + 30000; }
      return fail(res, 401, 'bad_pin', 'wrong PIN');
    }
    if (path === '/api/caps' && req.method === 'GET') return json(res, 200, { api: 1, board: 'twatch-ultra', fw: FW, caps: CAPS });
    if (path === '/api/device' && req.method === 'GET') return json(res, 200, device(isPaired(req)));
    if (DEV && path === '/api/_dev/pin') return json(res, 200, { pin: PIN });
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
      if (!b || !Number.isFinite(b.epoch)) return fail(res, 400, 'bad_request', 'epoch');
      clock = { offset_ms: b.epoch * 1000 - Date.now(), tz: typeof b.tz === 'string' && b.tz ? b.tz : clock.tz, synced: true };
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
      settings = next;
      publish('settings.changed', settings);
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
  console.log(`mock T-Watch Ultra (web API v1): http://localhost:${PORT}`);
  console.log(`storage: ${ROOT}   desktop: ${existsSync(WEB) ? WEB : '(placeholder)'}`);
  console.log(`pairing PIN: ${PIN}${DEV ? '  (also at /api/_dev/pin)' : ''}`);
});
