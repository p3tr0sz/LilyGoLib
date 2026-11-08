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

const CAPS = ['files', 'files.flash', 'sd', 'settings', 'time', 'screen', 'input', 'display', 'term', 'script',
  'monitor', 'notify', 'sensors.imu', 'gnss', 'lora', 'nfc', 'recorder', 'audio.play', 'wifi.scan', 'ble.scan', 'link.espnow', 'usb.hid', 'wifi.tools', 'ota',
  'wifi.sniffer', 'airspace', 'sentinel', 'presence.peers', 'presence.csi', 'presence.faro', 'presence.alarm', 'presence.history', 'fido2',
  'evil.portal', 'evil.portal.templates', 'evil.portal.customTemplates', 'evil.portal.twin', 'evil.portal.karma', 'evil.portal.settings',
  'evil.portal.clone', 'evil.portal.verify', 'evil.portal.coherentTwin', 'deauth.flood', 'deauth.campaign', 'deauth.health',
  'beacon.spam', 'beacon.campaign', 'beacon.health'];

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

// ---- terminal (mirrors web_tool_term.cpp term_exec so mock and firmware agree) ----
const TERM_HELP = [
  'help            this list', 'echo TEXT       print TEXT', 'clear           clear the screen',
  'pwd             working directory', 'cd [PATH]       change directory (/sd, /flash)',
  'ls [PATH]       list a directory', 'cat FILE        print a file (first 16 KB)',
  'status          uptime, cpu, app, battery, wifi, heap', 'free            heap and psram',
  'uptime          seconds since boot', 'date            device clock (UTC epoch)',
  'ver             firmware version and board', 'logs [N]        last N console lines (default 20)',
  'reboot          restart the watch',
];
function termNorm(cwd, arg) {
  const p = !arg ? cwd : (arg.startsWith('/') ? arg : cwd + '/' + arg);
  const stack = [];
  for (const seg of p.split('/')) { if (seg === '' || seg === '.') continue; else if (seg === '..') stack.pop(); else stack.push(seg); }
  return '/' + stack.join('/');
}
async function termExec(cwd, line) {
  if (!cwd) cwd = '/sd';
  const out = [];
  const add = (s) => out.push(s);
  let status = 0;                                       // 0 success, 1 a failed command, 127 not found
  const t = (line || '').trim();
  const sp = t.indexOf(' ');
  const cmd = sp < 0 ? t : t.slice(0, sp);
  const rest = sp < 0 ? '' : t.slice(sp + 1).trim();
  if (cmd === '') { /* nothing */ }
  else if (cmd === 'help' || cmd === '?') TERM_HELP.forEach(add);
  else if (cmd === 'echo') add(rest);
  else if (cmd === 'pwd') add(cwd);
  else if (cmd === 'cd') {
    const api = termNorm(cwd, rest);
    if (api === '/') cwd = '/';
    else { const r = resolvePath(api); let ok = false; if (!r.error) { try { ok = (await fsp.stat(r.abs)).isDirectory(); } catch {} } if (ok) cwd = api; else { add(`cd: ${api}: no such directory`); status = 1; } }
  } else if (cmd === 'ls' || cmd === 'dir') {
    const api = termNorm(cwd, rest);
    if (api === '/') { add('flash/'); add('sd/'); }
    else {
      const r = resolvePath(api);
      if (r.error) { add(`ls: ${api}: ${r.error === 'sd_missing' ? 'not mounted' : 'no such path'}`); status = 1; }
      else try {
        const st = await fsp.stat(r.abs);
        if (!st.isDirectory()) add(api);
        else { const names = (await fsp.readdir(r.abs)).sort(); if (!names.length) add('(empty)'); for (const n of names) { const s = await fsp.stat(join(r.abs, n)); add(s.isDirectory() ? n + '/' : `${n}\t${s.size} B`); } }
      } catch { add(`ls: ${api}: no such path`); status = 1; }
    }
  } else if (cmd === 'cat' || cmd === 'type') {
    if (!rest) { add('cat: missing file'); status = 1; }
    else { const api = termNorm(cwd, rest); const r = resolvePath(api);
      if (r.error) { add(`cat: ${api}: ${r.error === 'sd_missing' ? 'not mounted' : 'no such file'}`); status = 1; }
      else try { const st = await fsp.stat(r.abs); if (st.isDirectory()) { add(`cat: ${api}: not a file`); status = 1; } else { let buf = await fsp.readFile(r.abs); const total = buf.length; if (buf.length > 16384) buf = buf.subarray(0, 16384); out.push(buf.toString('utf8').replace(/\n$/, '')); if (total > 16384) add(`… (truncated at 16384 B of ${total})`); } } catch { add(`cat: ${api}: not a file`); status = 1; }
    }
  } else if (cmd === 'status') { add(`uptime   ${Math.floor(process.uptime())} s`); add('cpu      240 MHz'); add('app      (home)'); add('battery  99%  charging  4178 mV'); add('wifi     sta  mock  -50 dBm'); add('heap     180000 free / 320000 B'); }
  else if (cmd === 'free') { add('heap   180000 free  120000 min  320000 total'); add('psram  2000000 free  8000000 total'); }
  else if (cmd === 'uptime') add(`${Math.floor(process.uptime())} s`);
  else if (cmd === 'date') add(`${Math.floor(Date.now() / 1000)} (UTC epoch)`);
  else if (cmd === 'ver' || cmd === 'version') add(`${FW}  twatch-ultra`);
  else if (cmd === 'logs') add('(no log lines yet)');
  else if (cmd === 'reboot') add('Rebooting…');
  else if (cmd === 'clear') { /* front-end handles it */ }
  else { add(`${cmd}: command not found (try help)`); status = 127; }
  return { cwd, output: out.length ? out.join('\n') + '\n' : '', status };
}

// ---- scripting engine (mirrors web_tool_script.cpp script_run so mock and firmware agree) ----
const SCRIPT_SRC_MAX = 16 * 1024, SCRIPT_OUT_MAX = 32 * 1024, SCRIPT_STEP_MAX = 20000;
const SCRIPT_MS_MAX = 8000, SCRIPT_SLEEP_MAX = 5000, SCRIPT_DEPTH_MAX = 24, SCRIPT_DIR = '/sd/scripts';
const isNameStart = (c) => /[A-Za-z_]/.test(c);
const isNameChar = (c) => /[A-Za-z0-9_]/.test(c);
const scriptNameOk = (n) => typeof n === 'string' && /^[A-Za-z0-9._-]{1,48}$/.test(n) && !n.includes('..') && n.endsWith('.scr');

// Seg: {type:0 lit|1 var(text=name)|2 arith}. Word = Seg[]. Statement = Word[].
function scriptLex(src) {
  const stmts = [];
  let stmt = [], word = [], lit = '', wordActive = false, quote = 0;
  const flushLit = () => { if (lit) { word.push({ type: 0, text: lit }); lit = ''; } };
  const flushWord = () => { flushLit(); if (wordActive) { stmt.push(word); word = []; wordActive = false; } };
  const endStmt = () => { flushWord(); if (stmt.length) { stmts.push(stmt); stmt = []; } };
  let i = 0; const n = src.length;
  while (i < n) {
    const c = src[i];
    if (quote === 0) {
      if (c === '#' && !wordActive) { while (i < n && src[i] !== '\n') i++; continue; }
      if (c === '\n' || c === ';') { endStmt(); i++; continue; }
      if (c === ' ' || c === '\t' || c === '\r') { flushWord(); i++; continue; }
      if (c === "'") { wordActive = true; quote = 1; i++; continue; }
      if (c === '"') { wordActive = true; quote = 2; i++; continue; }
      if (c !== '$') { lit += c; wordActive = true; i++; continue; }
      wordActive = true;
    } else if (quote === 1) {
      if (c === "'") { quote = 0; i++; continue; }
      lit += c; i++; continue;
    } else {
      if (c === '"') { quote = 0; i++; continue; }
      if (c !== '$') { lit += c; i++; continue; }
    }
    // c === '$'
    if (i + 1 < n && src[i + 1] === '{') {
      const j = src.indexOf('}', i + 2);
      if (j < 0) return { err: 'bad substitution' };
      flushLit(); word.push({ type: 1, text: src.slice(i + 2, j) }); i = j + 1;
    } else if (i + 2 < n && src[i + 1] === '(' && src[i + 2] === '(') {
      const j = src.indexOf('))', i + 3);
      if (j < 0) return { err: 'bad arithmetic' };
      flushLit(); word.push({ type: 2, text: src.slice(i + 3, j) }); i = j + 2;
    } else if (i + 1 < n && src[i + 1] === '?') {
      flushLit(); word.push({ type: 1, text: '?' }); i += 2;
    } else if (i + 1 < n && isNameStart(src[i + 1])) {
      let j = i + 1; while (j < n && isNameChar(src[j])) j++;
      flushLit(); word.push({ type: 1, text: src.slice(i + 1, j) }); i = j;
    } else { lit += '$'; i++; }
  }
  if (quote !== 0) return { err: 'unterminated quote' };
  endStmt();
  return { stmts };
}

const wordIs = (w, kw) => w.length === 1 && w[0].type === 0 && w[0].text === kw;
const KEYWORDS = new Set(['if', 'then', 'elif', 'else', 'fi', 'for', 'in', 'do', 'done', 'while']);
function stmtKw(s) {
  if (!s.length || s[0].length !== 1 || s[0][0].type !== 0) return '';
  const t = s[0][0].text;
  return KEYWORDS.has(t) ? t : '';
}
function splitTrailingKeywords(stmts) {
  for (let i = 0; i < stmts.length; i++) {
    const k = stmtKw(stmts[i]);
    if ((k === 'then' || k === 'do' || k === 'else') && stmts[i].length > 1) {
      const rest = stmts[i].slice(1);
      stmts[i] = [stmts[i][0]];
      stmts.splice(i + 1, 0, rest);
    }
  }
}

// Parser: statements -> node list. Nodes: {t:'cmd',assigns,argv} {t:'andor',op,left,right}
//   {t:'if',cond,body,elifs:[{cond,body}],elseBody} {t:'for',var,items,body} {t:'while',cond,body}
function scriptParse(stmts) {
  let pos = 0; let err = '';
  const fail = (m) => { if (!err) err = m; return null; };
  const kw = () => (pos < stmts.length ? stmtKw(stmts[pos]) : '');
  const tail = () => stmts[pos].slice(1);

  function simpleFromWords(words) {
    const chunks = [[]]; const ops = [];
    for (const w of words) {
      if (wordIs(w, '&&') || wordIs(w, '||')) {
        if (!chunks[chunks.length - 1].length) return fail(`syntax error near '${w[0].text}'`);
        ops.push(wordIs(w, '&&') ? 0 : 1); chunks.push([]);
      } else chunks[chunks.length - 1].push(w);
    }
    if (!chunks[chunks.length - 1].length) return fail('syntax error: empty command');
    const makeCmd = (ws) => {
      const cmd = { t: 'cmd', assigns: [], argv: [] };
      let k = 0;
      for (; k < ws.length; k++) {
        const w = ws[k];
        if (!w.length || w[0].type !== 0) break;
        const first = w[0].text; const eq = first.indexOf('=');
        if (eq <= 0 || !isNameStart(first[0])) break;
        let ok = true;
        for (let c = 1; c < eq; c++) if (!isNameChar(first[c])) { ok = false; break; }
        if (!ok) break;
        const value = [{ type: 0, text: first.slice(eq + 1) }, ...w.slice(1)];
        cmd.assigns.push({ name: first.slice(0, eq), value });
      }
      for (; k < ws.length; k++) cmd.argv.push(ws[k]);
      return cmd;
    };
    let node = makeCmd(chunks[0]);
    for (let c = 0; c < ops.length; c++) node = { t: 'andor', op: ops[c], left: node, right: makeCmd(chunks[c + 1]) };
    return node;
  }

  function parseBlock(stop, depth) {
    if (depth > SCRIPT_DEPTH_MAX) return fail('too deeply nested');
    const out = [];
    while (pos < stmts.length) {
      const k = kw();
      if (stop.includes(k)) return out;
      if (['then', 'elif', 'else', 'fi', 'done', 'in'].includes(k)) return fail(`unexpected '${k}'`);
      let n;
      if (k === 'if') n = parseIf(depth + 1);
      else if (k === 'for') n = parseFor(depth + 1);
      else if (k === 'while') n = parseWhile(depth + 1);
      else { n = simpleFromWords(stmts[pos]); pos++; }
      if (err) return null;
      out.push(n);
    }
    return out;
  }

  function parseIf(depth) {
    const node = { t: 'if', elifs: [], elseBody: null };
    const c = tail(); if (!c.length) return fail('if: missing condition');
    pos++;
    node.cond = simpleFromWords(c); if (err) return null;
    if (kw() !== 'then') return fail("if: missing 'then'");
    pos++;
    node.body = parseBlock(['elif', 'else', 'fi'], depth); if (err) return null;
    while (true) {
      const k = kw();
      if (k === 'elif') {
        const ec = tail(); if (!ec.length) return fail('elif: missing condition');
        pos++;
        const econd = simpleFromWords(ec); if (err) return null;
        if (kw() !== 'then') return fail("elif: missing 'then'");
        pos++;
        const ebody = parseBlock(['elif', 'else', 'fi'], depth); if (err) return null;
        node.elifs.push({ cond: econd, body: ebody });
      } else if (k === 'else') {
        pos++;
        node.elseBody = parseBlock(['fi'], depth); if (err) return null;
      } else if (k === 'fi') { pos++; return node; }
      else return fail("if: missing 'fi'");
    }
  }

  function parseFor(depth) {
    const node = { t: 'for', items: [] };
    const w = stmts[pos];
    if (w.length < 2 || w[1].length !== 1 || w[1][0].type !== 0 || !isNameStart(w[1][0].text[0])) return fail('for: missing variable');
    node.var = w[1][0].text;
    for (const ch of node.var) if (!isNameChar(ch)) return fail('for: bad variable');
    if (w.length < 3 || !wordIs(w[2], 'in')) return fail("for: missing 'in'");
    for (let k = 3; k < w.length; k++) node.items.push(w[k]);
    pos++;
    if (kw() !== 'do') return fail("for: missing 'do'");
    pos++;
    node.body = parseBlock(['done'], depth); if (err) return null;
    if (kw() !== 'done') return fail("for: missing 'done'");
    pos++;
    return node;
  }

  function parseWhile(depth) {
    const node = { t: 'while' };
    const c = tail(); if (!c.length) return fail('while: missing condition');
    pos++;
    node.cond = simpleFromWords(c); if (err) return null;
    if (kw() !== 'do') return fail("while: missing 'do'");
    pos++;
    node.body = parseBlock(['done'], depth); if (err) return null;
    if (kw() !== 'done') return fail("while: missing 'done'");
    pos++;
    return node;
  }

  const prog = parseBlock([], 0);
  return { prog, err };
}

function arithEval(expr, vars) {
  let i = 0; const s = expr;
  const ws = () => { while (i < s.length && (s[i] === ' ' || s[i] === '\t')) i++; };
  const varVal = (name) => { const v = vars.get(name); const n = parseInt(v, 10); return Number.isNaN(n) ? 0 : n; };
  function factor() {
    ws();
    if (s[i] === '(') { i++; const v = expr2(); ws(); if (s[i] === ')') i++; return v; }
    if (s[i] === '-' || s[i] === '+') { const op = s[i++]; const v = factor(); return op === '-' ? -v : v; }
    if (/[0-9]/.test(s[i] || '')) { let v = 0; while (/[0-9]/.test(s[i] || '')) v = v * 10 + (s.charCodeAt(i++) - 48); return v; }
    if (isNameStart(s[i] || '')) { let a = i; while (isNameChar(s[i] || '')) i++; return varVal(s.slice(a, i)); }
    return 0;
  }
  function term() {
    let v = factor();
    for (;;) { ws(); const c = s[i]; if (c === '*' || c === '/' || c === '%') { i++; const r = factor(); if (c === '*') v *= r; else if (r === 0) v = 0; else if (c === '/') v = Math.trunc(v / r); else v = v % r; } else break; }
    return v;
  }
  function expr2() {
    let v = term();
    for (;;) { ws(); const c = s[i]; if (c === '+' || c === '-') { i++; const r = term(); v = c === '+' ? v + r : v - r; } else break; }
    return v;
  }
  return Math.trunc(expr2());
}

async function scriptRun(cwd, src) {
  if (!cwd) cwd = '/sd';
  const lexed = scriptLex(src);
  if (lexed.err) return { cwd, output: `script: ${lexed.err}\n` };
  splitTrailingKeywords(lexed.stmts);
  const parsed = scriptParse(lexed.stmts);
  if (parsed.err) return { cwd, output: `script: ${parsed.err}\n` };

  const C = { cwd, out: '', vars: new Map(), status: 0, steps: 0, start: Date.now(), exited: false, aborted: false };
  const outAdd = (s) => { if (C.out.length < SCRIPT_OUT_MAX) C.out += s; };
  const outLine = (s) => outAdd(s + '\n');
  const expandWord = (w) => {
    let r = '';
    for (const seg of w) {
      if (seg.type === 0) r += seg.text;
      else if (seg.type === 1) { if (seg.text === '?') r += String(C.status); else if (C.vars.has(seg.text)) r += C.vars.get(seg.text); }
      else r += String(arithEval(seg.text, C.vars));
    }
    return r;
  };
  const checkCaps = () => {
    if (C.aborted) return true;
    let why = null;
    if (++C.steps > SCRIPT_STEP_MAX) why = 'step limit';
    else if (Date.now() - C.start > SCRIPT_MS_MAX) why = 'time limit';
    else if (C.out.length >= SCRIPT_OUT_MAX) why = 'output limit';
    if (why) { C.aborted = true; C.out += `\nscript: aborted (${why})\n`; return true; }
    return false;
  };
  const pathStat = async (arg) => {
    const api = !arg ? C.cwd : (arg.startsWith('/') ? arg : C.cwd + '/' + arg);
    const r = resolvePath(api);
    if (r.error) return null;
    try { return await fsp.stat(r.abs); } catch { return null; }
  };
  const evalTest = async (a) => {
    const n = a.length;
    if (n === 0) return 1;
    if (n === 1) return a[0] ? 0 : 1;
    if (n === 2) {
      const op = a[0], x = a[1];
      if (op === '-z') return x ? 1 : 0;
      if (op === '-n') return x ? 0 : 1;
      if (op === '-e' || op === '-f' || op === '-d') {
        const st = await pathStat(x);
        if (!st) return 1;
        if (op === '-d') return st.isDirectory() ? 0 : 1;
        if (op === '-f') return st.isFile() ? 0 : 1;
        return 0;
      }
      return 2;
    }
    if (n === 3) {
      const x = a[0], op = a[1], y = a[2];
      if (op === '=' || op === '==') return x === y ? 0 : 1;
      if (op === '!=') return x !== y ? 0 : 1;
      const ix = parseInt(x, 10) || 0, iy = parseInt(y, 10) || 0;
      if (op === '-eq') return ix === iy ? 0 : 1;
      if (op === '-ne') return ix !== iy ? 0 : 1;
      if (op === '-lt') return ix < iy ? 0 : 1;
      if (op === '-le') return ix <= iy ? 0 : 1;
      if (op === '-gt') return ix > iy ? 0 : 1;
      if (op === '-ge') return ix >= iy ? 0 : 1;
      return 2;
    }
    return 2;
  };

  const runArgv = async (argv) => {
    if (!argv.length) { C.status = 0; return; }
    const cmd = argv[0];
    if (cmd === 'help' || cmd === '?') {
      [
        'scripting engine — a small sh-like language.',
        'variables   NAME=value ; expand $NAME ${NAME} $? ; $(( expr )) integer maths',
        'quoting     \'literal\'   "expands $vars"   # comment to end of line',
        'echo TEXT   print (expanded)        set    list variables',
        'test / [ … ]  -z -n -e -f -d, = != -eq -ne -lt -le -gt -ge',
        'sleep N     wait N seconds          true / false / :   exit status 0 / 1 / 0',
        'exit [N]    stop the script',
        'if LIST; then … elif … else … fi',
        'for NAME in WORDS; do … done        while LIST; do … done',
        'LIST && LIST   LIST || LIST',
        'any other command (ls, cat, status, cd, logs, …) runs in the Terminal shell.',
      ].forEach(outLine);
      C.status = 0; return;
    }
    if (cmd === ':' || cmd === 'true') { C.status = 0; return; }
    if (cmd === 'false') { C.status = 1; return; }
    if (cmd === 'exit') { C.exited = true; if (argv.length > 1) C.status = parseInt(argv[1], 10) || 0; return; }
    if (cmd === 'sleep') {
      if (argv.length < 2) { outLine('sleep: missing seconds'); C.status = 1; return; }
      let sec = parseFloat(argv[1]); if (!(sec >= 0)) sec = 0;
      let ms = Math.floor(sec * 1000); if (ms > SCRIPT_SLEEP_MAX) ms = SCRIPT_SLEEP_MAX;
      await new Promise((r) => setTimeout(r, ms));
      C.status = 0; return;
    }
    if (cmd === 'set' && argv.length === 1) {
      const lines = [...C.vars.entries()].map(([k, v]) => `${k}=${v}`).sort();
      for (const l of lines) outLine(l);
      C.status = 0; return;
    }
    if (cmd === 'test' || cmd === '[') {
      const a = argv.slice(1);
      if (cmd === '[') { if (!a.length || a[a.length - 1] !== ']') { outLine("[: missing ']'"); C.status = 2; return; } a.pop(); }
      C.status = await evalTest(a);
      return;
    }
    // external: reconstruct the line and hand to the Terminal shell (keeps the sandbox). Its real
    // exit status becomes $?, so `if cmd`, `while cmd`, `&&` and `||` branch on actual success.
    const r = await termExec(C.cwd, argv.join(' '));
    C.cwd = r.cwd;
    outAdd(r.output);
    C.status = r.status;
  };

  const runSimple = async (n) => {
    if (C.exited || C.aborted) return;
    if (n.t === 'andor') {
      await runSimple(n.left);
      if (C.exited || C.aborted) return;
      if ((n.op === 0 && C.status === 0) || (n.op === 1 && C.status !== 0)) await runSimple(n.right);
      return;
    }
    for (const a of n.assigns) C.vars.set(a.name, expandWord(a.value));
    if (!n.argv.length) { C.status = 0; return; }
    if (checkCaps()) return;
    await runArgv(n.argv.map(expandWord));
  };

  const runNodes = async (nodes) => {
    for (const n of nodes) {
      if (C.exited || C.aborted) return;
      if (n.t === 'cmd' || n.t === 'andor') { await runSimple(n); continue; }
      if (n.t === 'if') {
        await runSimple(n.cond);
        if (C.exited || C.aborted) return;
        if (C.status === 0) { await runNodes(n.body); continue; }
        let done = false;
        for (const e of n.elifs) {
          await runSimple(e.cond);
          if (C.exited || C.aborted) return;
          if (C.status === 0) { await runNodes(e.body); done = true; break; }
        }
        if (!done && n.elseBody) await runNodes(n.elseBody);
        continue;
      }
      if (n.t === 'for') {
        for (const item of n.items) {
          if (checkCaps()) return;
          C.vars.set(n.var, expandWord(item));
          await runNodes(n.body);
          if (C.exited || C.aborted) return;
        }
        continue;
      }
      if (n.t === 'while') {
        for (;;) {
          if (checkCaps()) return;
          await runSimple(n.cond);
          if (C.exited || C.aborted) return;
          if (C.status !== 0) break;
          await runNodes(n.body);
          if (C.exited || C.aborted) return;
        }
      }
    }
  };

  await runNodes(parsed.prog);
  return { cwd: C.cwd, output: C.out };
}

async function scriptListNames() {
  const r = resolvePath(SCRIPT_DIR);
  if (r.error) return [];
  let names = [];
  try { names = await fsp.readdir(r.abs); } catch { return []; }
  const out = [];
  for (const nm of names.sort()) {
    if (!scriptNameOk(nm)) continue;
    try { const st = await fsp.stat(join(r.abs, nm)); if (st.isFile()) out.push({ name: nm, size: st.size }); } catch { /* skip */ }
  }
  return out;
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
let sniffer = { active: false, packets: 0, drops: 0, bytes: 24, channel: null, filter: 'all', file: null,
  seen: 0, management: 0, control: 0, data: 0, beacons: 0, probes: 0, deauth: 0, eapol: 0, handshakes: 0, aps: [], hs: [] };
let airspace = { active: false, deauth_total: 0, deauth_floods: 0, beacon_total: 0, beacon_floods: 0, evil_twins: 0, started: 0 };
let sentinel = { active: false, started: 0, device_count: 0, devices: [] };
let presence = { active: false, started: 0, peers_ever: 0, peers: [], history: [] };
let presenceCsi = {}, presenceFaroZone = {}, presenceCsiHistory = [], presenceHistoryBucket = 0;
let presenceFaro = { active: false, channel: null, mac: null, started: 0, realStarted: 0, tx_errors: 0, error: null };
let presenceAlarm = { state: 'off', stateMs: 0, exit_s: 10, duration_s: 30, triggers: 0, zone: null, error: null };
function newPresenceZone(active, threshold = 0.35) {
  return { linked: active, discovered: active, calibrating: active, present: false, motion: false,
    score: 0, level: 0, noise: 0.005, threshold, samples: 0, rate: 0, drops: 0, rssi: -90,
    last_motion_ms: 0, last_seen_ms: 0, mac: active ? '02:00:00:00:00:f1' : null,
    calibrationFrames: 0, over: 0 };
}
function resetPresenceCsi(active = false, threshold = 0.35) {
  const clock = Date.now();
  presenceCsi = { active, state: active ? 'calibrating' : 'stopped', calibrating: active, present: false, motion: false,
    score: 0, level: 0, noise: 0.005, threshold, samples: 0, rate: 0, drops: 0, rssi: -90,
    channel: active ? 1 : null, ssid: active ? 'mock-wifi' : null, started: active ? nowEpoch() : 0,
    last_motion_ms: 0, error: null, calibrationFrames: 0, over: 0, clock };
  presenceFaroZone = newPresenceZone(false, threshold);
  presenceCsiHistory = [];
  presenceHistoryBucket = clock;
  presenceAlarm.state = 'off'; presenceAlarm.zone = null; presenceAlarm.error = null;
}
resetPresenceCsi();
let fido2 = { available: false, state: 'idle', credentials: 0, creds: [] };
let evilportal = { active: false, ssid: '', clients: 0, attempts: 0, captures: [], customTemplates: [], templateError: null,
  cloning: false, cloneName: '', cloneBytes: 0, cloneSsid: '', coherent: false,
  karmaScanning: false, karmaTimer: null, karmaProbes: [], html: '', settings: { gateway: '192.168.4.1', pwdMode: 2, verify: false, verifiedOk: 0, verifiedBad: 0, autoStopBase: 0, autoStopExtended: 0, debug: false, logPath: '' } };
const BUILTIN_PORTAL_TEMPLATES = ['Default (Wi-Fi Login)', 'Router Update', 'Google Sign-In', 'OAuth / Social Login'];
let deauthflood = { active: false, targetBssid: '', targetStation: 'ff:ff:ff:ff:ff:ff', targetSsid: '', channel: 1, sent: 0, txErrors: 0, ratePps: 10, started: 0, error: null,
  campaign: { active: false, current: 0, completed: 0, count: 0, framesPerTarget: 100 }, progress: [], campaignTimer: null,
  reconnect: { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 } };
let deauthScan = { scanning: false, targets: [], error: null, timer: null };
let beaconspam = { active: false, channel: 0, beacons: 0, ssids: 0, prefix: 'FreeNet', mode: 0, txErrors: 0, customSsids: [], error: null,
  campaign: { active: false, current: 0, completed: 0, count: 0, framesPerSsid: 10 }, progress: [], campaignTimer: null,
  reconnect: { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 } };

function pcapGlobalHeader() {
  const b = Buffer.alloc(24);
  b.writeUInt32LE(0xa1b2c3d4, 0); b.writeUInt16LE(2, 4); b.writeUInt16LE(4, 6);
  b.writeUInt32LE(65535, 16); b.writeUInt32LE(127, 20);
  return b;
}
const SNIFFER_FILTERS = ['all', 'management', 'data', 'beacons', 'probes', 'eapol'];

function snifferFrame(kind = 'beacon', msg = 0) {
  if (kind === 'probe' || kind === 'management') {
    const f = Buffer.alloc(24); f[0] = 0x40; f.fill(0xff, 4, 10); f[10] = 2; f[16] = 2; f[21] = 1; return f;
  }
  if (kind === 'deauth') {
    const f = Buffer.alloc(26); f[0] = 0xc0; f.fill(0xff, 4, 10); f[10] = 2; f[16] = 2; f[21] = 1; return f;
  }
  if (kind === 'data') {
    const f = Buffer.alloc(24); f[0] = 0x08; f[1] = 0x01; f[4] = 2; f[10] = 2; f[16] = 2; f[21] = 1; return f;
  }
  if (kind === 'eapol') {
    const f = Buffer.alloc(80); f[0] = 0x08; f[1] = 0x01;
    f[4] = 2; f[10] = 2; f[16] = 2; f[21] = 1;
    Buffer.from([0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e]).copy(f, 24);
    f[32] = 2; f[33] = 3; f[34] = 0; f[35] = 44; f[36] = 2;
    f[37] = msg === 1 ? 0x80 : msg === 2 ? 0x01 : msg === 3 ? 0xc1 : msg === 4 ? 0x03 : 0;
    return f;
  }
  const f = Buffer.alloc(45); f[0] = 0x80; f.fill(0xff, 4, 10);
  f[10] = 2; f[16] = 2; f[21] = 1;
  f[36] = 0; f[37] = 7; Buffer.from('TestNet').copy(f, 38);
  return f;
}

function pcapRecord(frame) {
  const b = Buffer.alloc(16 + 13 + frame.length);
  b.writeUInt32LE(Math.floor(Date.now() / 1000), 0);
  b.writeUInt32LE(49, 8); b.writeUInt32LE(frame.length + 13, 12);
  b[18] = 13; b[20] = 0x28; b.writeUInt16LE(2412, 24); b[28] = (256 - 50) & 255;
  frame.copy(b, 29);
  return b;
}

function snifferHeaderLen(f) {
  if (f.length < 24) return 0;
  const fc = f[0] | (f[1] << 8), type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
  let n = 24;
  const toDs = !!(fc & 0x0100), fromDs = !!(fc & 0x0200);
  if (type === 2 && toDs && fromDs) n += 6;
  if (type === 2 && (subtype & 8)) n += 2;
  if (fc & 0x8000) n += 4;
  return n <= f.length ? n : 0;
}
function snifferEapol(f, hdr) {
  if (!hdr || f.length < hdr + 12) return null;
  const l = f.subarray(hdr);
  if (l[0] !== 0xaa || l[1] !== 0xaa || l[2] !== 0x03 || l[6] !== 0x88 || l[7] !== 0x8e) return null;
  if (l[9] !== 3 || f.length < hdr + 15) return null;
  return { keyInfo: l[13] };
}
function snifferMac(f, off) {
  return [...f.subarray(off, off + 6)].map(x => x.toString(16).padStart(2, '0')).join(':');
}
function snifferTrackAp(f, channel, rssi, beacon) {
  if (f.length < 36) return;
  const bssid = snifferMac(f, 16);
  let ssid = '', ds = 0, off = 36;
  while (off + 2 <= f.length) {
    const tag = f[off], len = f[off + 1];
    if (off + 2 + len > f.length) break;
    if (tag === 0) ssid = f.subarray(off + 2, off + 2 + len).toString('utf8');
    else if (tag === 3 && len === 1) ds = f[off + 2];
    off += 2 + len;
  }
  let ap = sniffer.aps.find(a => a.bssid === bssid);
  if (!ap) { ap = { bssid, ssid: '', channel: 0, rssi: 0, beacons: 0 }; sniffer.aps.push(ap); }
  if (ssid && !ap.ssid) ap.ssid = ssid;
  if (ds) ap.channel = ds; else if (channel > 0) ap.channel = channel;
  ap.rssi = rssi;
  if (beacon) ap.beacons++;
}
function snifferAnalyse(f) {
  if (f.length < 2) return;
  sniffer.seen++;
  const fc = f[0] | (f[1] << 8), type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
  if (type === 0) {
    sniffer.management++;
    if (subtype === 8) { sniffer.beacons++; snifferTrackAp(f, sniffer.channel || 1, -50, true); }
    else if (subtype === 4) sniffer.probes++;
    else if (subtype === 5) snifferTrackAp(f, sniffer.channel || 1, -50, false);
    if (subtype === 10 || subtype === 12) sniffer.deauth++;
  } else if (type === 1) sniffer.control++;
  else if (type === 2) {
    sniffer.data++;
    const e = snifferEapol(f, snifferHeaderLen(f));
    if (e) {
      sniffer.eapol++;
      const hi = e.keyInfo, ack = !!(hi & 0x80), mic = !!(hi & 0x01), secure = !!(hi & 0x02), install = !!(hi & 0x40);
      const msg = ack && mic && install ? 3 : ack && mic ? 3 : ack ? 1 : mic && secure ? 4 : mic ? 2 : 0;
      if (msg) {
        let a = snifferMac(f, 10), b = snifferMac(f, 4);
        if (a > b) [a, b] = [b, a];
        let hs = sniffer.hs.find(x => x.a === a && x.b === b);
        if (!hs) { hs = { a, b, mask: 0 }; sniffer.hs.push(hs); }
        hs.mask |= 1 << msg;
        if ((hs.mask & 0x1e) === 0x1e) { sniffer.handshakes++; hs.mask = 0; }
      }
    }
  }
}
function snifferFilterMatch(f) {
  if (sniffer.filter === 'all') return true;
  if (f.length < 2) return false;
  const fc = f[0] | (f[1] << 8), type = (fc >> 2) & 3, subtype = (fc >> 4) & 15;
  if (sniffer.filter === 'management') return type === 0;
  if (sniffer.filter === 'data') return type === 2;
  if (sniffer.filter === 'beacons') return type === 0 && subtype === 8;
  if (sniffer.filter === 'probes') return type === 0 && subtype === 4;
  if (sniffer.filter === 'eapol') return type === 2 && !!snifferEapol(f, snifferHeaderLen(f));
  return true;
}
function snifferState() {
  return { active: sniffer.active === true, channel: sniffer.channel || null, filter: sniffer.filter || 'all',
    packets: sniffer.packets || 0, drops: sniffer.drops || 0, bytes: sniffer.bytes || 24,
    seen: sniffer.seen || 0, management: sniffer.management || 0, control: sniffer.control || 0, data: sniffer.data || 0,
    beacons: sniffer.beacons || 0, probes: sniffer.probes || 0, deauth: sniffer.deauth || 0,
    eapol: sniffer.eapol || 0, handshakes: sniffer.handshakes || 0,
    aps: sniffer.aps || [], file: sniffer.file || null, error: sniffer.error || null };
}

function airspaceState() {
  return { active: airspace.active, deauth_total: airspace.deauth_total, deauth_floods: airspace.deauth_floods,
    beacon_total: airspace.beacon_total, beacon_floods: airspace.beacon_floods, evil_twins: airspace.evil_twins,
    broadcast_deauth: airspace.broadcast_deauth || 0, drops: 0, started: airspace.started, error: null };
}
function sentinelState() {
  return { active: sentinel.active, scanning: false, started: sentinel.started, device_count: sentinel.devices.length, devices: sentinel.devices, error: null };
}
function presenceZoneFresh() {
  return presenceCsi.active && presenceFaroZone.discovered && presenceCsi.clock - presenceFaroZone.last_seen_ms <= 3000;
}
function presenceZoneName(z, linked, stopped = false) {
  if (stopped) return 'stopped';
  if (!linked) return z.discovered ? 'absent' : 'waiting';
  return z.calibrating ? 'calibrating' : z.motion ? 'motion' : z.present ? 'present' : 'clear';
}
function presenceZoneJson(z, id) {
  const linked = id === 'router' ? presenceCsi.active : presenceZoneFresh();
  const out = { id, linked, calibrating: linked && z.calibrating, present: linked && z.present,
    motion: linked && z.motion, score: linked ? z.score : 0, level: z.level, noise: z.noise,
    threshold: z.threshold, samples: z.samples, rate: z.rate, drops: z.drops, rssi: z.rssi,
    channel: presenceCsi.channel, last_motion_ms: z.last_motion_ms,
    state: id === 'router' ? presenceCsi.state : presenceZoneName(z, linked, !presenceCsi.active) };
  if (id === 'faro') Object.assign(out, { discovered: z.discovered, last_seen_ms: z.last_seen_ms, mac: z.mac });
  return out;
}
function presenceAlarmRemaining() {
  const total = presenceAlarm.state === 'arming' ? presenceAlarm.exit_s : presenceAlarm.state === 'triggered' ? presenceAlarm.duration_s : 0;
  return total ? Math.max(0, Math.ceil((total * 1000 - (presenceCsi.clock - presenceAlarm.stateMs)) / 1000)) : 0;
}
function presenceAlarmEvent(event) {
  publish('presence.alarm', { event, state: presenceAlarm.state, remaining_s: presenceAlarmRemaining(), triggers: presenceAlarm.triggers, zone: presenceAlarm.zone });
}
function presenceStep() {
  for (const [id, z] of [['router', presenceCsi], ['faro', presenceFaroZone]]) {
    const linked = id === 'router' ? presenceCsi.active : presenceZoneFresh();
    if (linked && z.present && !z.motion && presenceCsi.clock - z.last_motion_ms > 10000) {
      z.present = false;
      publish('presence.csi', { event: 'clear', zone: id, state: presenceZoneName(z, linked), present: false, motion: z.motion, score: z.score });
    }
  }
  presenceCsi.state = !presenceCsi.active ? 'stopped' : presenceCsi.calibrating ? 'calibrating' : presenceCsi.motion ? 'motion' : presenceCsi.present ? 'present' : 'clear';
  if (presenceAlarm.state === 'arming' && presenceCsi.clock - presenceAlarm.stateMs >= presenceAlarm.exit_s * 1000) {
    presenceAlarm.state = 'armed'; presenceAlarm.stateMs = presenceCsi.clock; presenceAlarmEvent('armed');
  }
  if (presenceAlarm.state === 'armed' && presenceCsi.clock - presenceAlarm.stateMs >= 1500) {
    const routerMotion = presenceCsi.active && !presenceCsi.calibrating && presenceCsi.motion;
    const faroMotion = presenceZoneFresh() && !presenceFaroZone.calibrating && presenceFaroZone.motion;
    if (routerMotion || faroMotion) {
      presenceAlarm.state = 'triggered'; presenceAlarm.stateMs = presenceCsi.clock;
      presenceAlarm.zone = faroMotion && !routerMotion ? 'faro' : 'router'; presenceAlarm.triggers++;
      presenceAlarmEvent('triggered');
    }
  }
  if (presenceAlarm.state === 'triggered' && presenceCsi.clock - presenceAlarm.stateMs >= presenceAlarm.duration_s * 1000) {
    presenceAlarm.state = 'off'; presenceAlarm.zone = null; presenceAlarmEvent('timeout');
  }
  if (presenceCsi.active) {
    while (presenceCsi.clock - presenceHistoryBucket >= 500) {
      presenceHistoryBucket += 500;
      presenceCsiHistory.push({ t: presenceHistoryBucket, router: Math.round(presenceCsi.score * 100),
        faro: presenceZoneFresh() ? Math.round(presenceFaroZone.score * 100) : 0 });
      if (presenceCsiHistory.length > 120) presenceCsiHistory.splice(0, presenceCsiHistory.length - 120);
    }
  }
}
function presenceState() {
  const { calibrationFrames, over, clock, ...base } = presenceCsi;
  const csi = { ...base, zones: [presenceZoneJson(presenceCsi, 'router'), presenceZoneJson(presenceFaroZone, 'faro')], history: presenceCsiHistory };
  const { stateMs, ...alarm } = presenceAlarm;
  const sent = presenceFaro.active ? Math.floor((Date.now() - presenceFaro.realStarted) / 10) : presenceFaro.sent || 0;
  const faro = { active: presenceFaro.active, channel: presenceFaro.channel, mac: presenceFaro.mac,
    sent, rate: presenceFaro.active ? 100 : 0, tx_errors: presenceFaro.tx_errors, started: presenceFaro.started, error: presenceFaro.error };
  return { active: presence.active, started: presence.started, peers_ever: presence.peers_ever, peers: presence.peers,
    history: presence.history, error: null, csi, faro, alarm: { ...alarm, remaining_s: presenceAlarmRemaining() } };
}
function fido2State() {
  return { available: fido2.available, state: fido2.state, credentials: fido2.credentials, error: fido2.available ? null : 'Simulated FIDO2 transport is disabled' };
}
function evilportalState() {
  return { active: evilportal.active, mode: evilportal.mode || 0, ssid: evilportal.ssid, twinBssid: evilportal.twinBssid,
    channel: evilportal.channel || 1, coherent: evilportal.coherent, deauthFrames: evilportal.deauthFrames || 0,
    cloning: evilportal.cloning, cloneName: evilportal.cloneName, cloneBytes: evilportal.cloneBytes, cloneSsid: evilportal.cloneSsid,
    clients: evilportal.clients, attempts: evilportal.attempts, confirmed: evilportal.confirmed || 0,
    started: evilportal.started || 0, dhcpsOk: true, probesTotal: evilportal.probesTotal || 0, error: evilportal.error || null,
    templates: [...BUILTIN_PORTAL_TEMPLATES.map((name, id) => ({ id, name, builtin: true })),
      ...evilportal.customTemplates.map(({ path, html, ...publicFields }) => publicFields)],
    templateError: evilportal.templateError, karmaScanning: evilportal.karmaScanning,
    ssidPresets: ['Free Wi-Fi', 'WiFi Guest', 'Hotel Wi-Fi'], settings: evilportal.settings };
}

function validPortalHtml(html) {
  const compact = html.toLowerCase().replace(/[\s]/g, '');
  return compact.includes('<html') && compact.includes('</html>') && compact.includes('<form') &&
    ["method='post'", 'method="post"', 'method=post'].some(x => compact.includes(x)) &&
    ["action='/login'", 'action="/login"', 'action=/login'].some(x => compact.includes(x)) &&
    ["name='username'", 'name="username"', 'name=username'].some(x => compact.includes(x)) &&
    ["name='password'", 'name="password"', 'name=password'].some(x => compact.includes(x)) &&
    !['<script', '<iframe', '<object', '<embed', '<svg', '<math', '<meta', '<base', '<link',
      'src=', 'srcset=', 'href=', 'http:', 'https:', 'javascript:', 'vbscript:', 'data:', '//', '\\', '&'].some(x => compact.includes(x));
}
async function reloadPortalTemplates() {
  const dir = join(ROOTS.sd, 'data/evilportal/portals');
  const found = [];
  let rejected = 0;
  let names = [];
  try { names = await fsp.readdir(dir); } catch {}
  names.sort();
  for (const name of names) {
    if (!/^[A-Za-z0-9_.-]{6,64}$/.test(name) || name.startsWith('.') || name.includes('..') || !name.endsWith('.html') || found.length >= 16) { rejected++; continue; }
    try {
      const path = join(dir, name), st = await fsp.stat(path);
      if (!st.isFile() || st.size < 1 || st.size > 65536) throw new Error('invalid');
      const html = await fsp.readFile(path, 'utf8');
      if (!validPortalHtml(html)) throw new Error('invalid');
      found.push({ id: 100 + found.length, name: name.slice(0, -5), builtin: false, source: 'sd', path, html });
    } catch { rejected++; }
  }
  evilportal.customTemplates = found;
  evilportal.templateError = rejected ? `${rejected} invalid or unsupported SD template(s) omitted` : null;
  return { count: found.length, rejected };
}
function selectPortalTemplate(id) {
  if (Number.isInteger(id) && id >= 0 && id < BUILTIN_PORTAL_TEMPLATES.length) return { id, html: '' };
  return evilportal.customTemplates.find(t => t.id === id) || null;
}
function finishKarmaScan() {
  clearTimeout(evilportal.karmaTimer);
  evilportal.karmaTimer = null;
  evilportal.karmaScanning = false;
}
function addPortalCapture(capture) {
  const cap = { url: '/login', ts: nowEpoch(), ...capture,
    verification: evilportal.settings.verify && evilportal.mode === 1 ? 'queued' : 'off' };
  evilportal.captures.push(cap);
  evilportal.attempts++;
  if (cap.verification === 'queued') setTimeout(() => { if (cap.verification === 'queued') cap.verification = 'checking'; }, 0);
  return cap;
}
function stopPortalClone() {
  evilportal.cloning = false;
  evilportal.cloneSsid = '';
}
function deauthfloodState() { const { campaignTimer, ...state } = deauthflood; return { ...state }; }
function beaconspamState() { const { campaignTimer, ...state } = beaconspam; return { ...state }; }
function mockReconnect(tool) { tool.reconnect = { pending: false, reconnected: true, attempts: 1, elapsedMs: 25 }; }
function finishDeauthCampaign() {
  clearInterval(deauthflood.campaignTimer); deauthflood.campaignTimer = null;
  deauthflood.active = false; deauthflood.campaign.active = false; mockReconnect(deauthflood);
}
function advanceDeauthCampaign() {
  deauthflood.campaign.completed = deauthflood.progress.filter(t => ['done', 'error', 'pmf_limited'].includes(t.state)).length;
  let next = deauthflood.progress.findIndex(t => t.state === 'pending');
  deauthflood.campaign.current = next < 0 ? deauthflood.progress.length : next;
  if (next < 0) return finishDeauthCampaign();
  const t = deauthflood.progress[next]; t.state = 'running'; t.started = Date.now();
  deauthflood.targetBssid = t.bssid; deauthflood.targetSsid = t.ssid; deauthflood.targetStation = t.station; deauthflood.channel = t.channel;
}
function startMockDeauthCampaign(targets, frames, rate, station) {
  clearInterval(deauthflood.campaignTimer);
  deauthflood.progress = targets.map(ap => ({ ssid: ap.ssid, bssid: ap.bssid, station, channel: ap.channel, auth: ap.auth,
    state: ap.channel < 1 || ap.channel > 13 ? 'error' : 'pending', accepted: 0, failed: 0, goal: frames, elapsedMs: 0,
    pmfLimited: ap.auth.includes('wpa3'), error: ap.channel < 1 || ap.channel > 13 ? 'Only 2.4 GHz channels 1-13 are supported' : null }));
  deauthflood.campaign = { active: true, current: 0, completed: 0, count: targets.length, framesPerTarget: frames };
  Object.assign(deauthflood, { active: true, sent: 0, txErrors: 0, ratePps: rate, started: nowEpoch(), error: null,
    reconnect: { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 } });
  advanceDeauthCampaign();
  deauthflood.campaignTimer = setInterval(() => {
    const t = deauthflood.progress[deauthflood.campaign.current];
    if (!deauthflood.active || !t) return;
    const add = Math.min(t.goal - t.accepted, Math.max(1, Math.floor(deauthflood.ratePps / 20)));
    t.accepted += add; deauthflood.sent += add; t.elapsedMs = Date.now() - t.started;
    if (t.accepted >= t.goal) { t.state = t.pmfLimited ? 'pmf_limited' : 'done'; advanceDeauthCampaign(); }
  }, 25);
}
function finishBeaconCampaign() {
  clearInterval(beaconspam.campaignTimer); beaconspam.campaignTimer = null;
  beaconspam.active = false; beaconspam.campaign.active = false; mockReconnect(beaconspam);
}
function advanceBeaconCampaign() {
  beaconspam.campaign.completed = beaconspam.progress.filter(t => ['done', 'error'].includes(t.state)).length;
  const next = beaconspam.progress.findIndex(t => t.state === 'pending');
  beaconspam.campaign.current = next < 0 ? beaconspam.progress.length : next;
  if (next < 0) return finishBeaconCampaign();
  const t = beaconspam.progress[next]; t.state = 'running'; t.started = Date.now();
}
function startMockBeaconCampaign(ssids, frames, channel) {
  clearInterval(beaconspam.campaignTimer);
  beaconspam.progress = ssids.map(ssid => ({ ssid, state: 'pending', accepted: 0, failed: 0, goal: frames, elapsedMs: 0, error: null }));
  beaconspam.campaign = { active: true, current: 0, completed: 0, count: ssids.length, framesPerSsid: frames };
  beaconspam.active = true; beaconspam.beacons = 0; beaconspam.txErrors = 0; beaconspam.channel = channel;
  beaconspam.reconnect = { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 };
  advanceBeaconCampaign();
  beaconspam.campaignTimer = setInterval(() => {
    const t = beaconspam.progress[beaconspam.campaign.current];
    if (!beaconspam.active || !t) return;
    t.accepted++; beaconspam.beacons++; t.elapsedMs = Date.now() - t.started;
    if (t.accepted >= t.goal) { t.state = 'done'; advanceBeaconCampaign(); }
  }, 25);
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
  if (path === '/api/term' && m === 'POST') { const b = await readJSON(req); return json(res, 200, await termExec(typeof b?.cwd === 'string' ? b.cwd : '/sd', typeof b?.line === 'string' ? b.line : '')); }
  if (path === '/api/script/run' && m === 'POST') {
    const b = await readJSON(req);
    const source = typeof b?.source === 'string' ? b.source : '';
    if (source.length > SCRIPT_SRC_MAX) return fail(res, 400, 'too_large', 'script too large');
    return json(res, 200, await scriptRun(typeof b?.cwd === 'string' ? b.cwd : '/sd', source));
  }
  if (path === '/api/script/list' && m === 'GET') return json(res, 200, { dir: SCRIPT_DIR, scripts: await scriptListNames() });
  if (path === '/api/script/read' && m === 'GET') {
    const name = url.searchParams.get('name') || '';
    if (!scriptNameOk(name)) return fail(res, 400, 'bad_name', 'bad script name');
    const r = resolvePath(`${SCRIPT_DIR}/${name}`);
    if (r.error) return fail(res, 503, 'sd_missing', 'SD card not mounted');
    try { const src = (await fsp.readFile(r.abs)).toString('utf8').slice(0, SCRIPT_SRC_MAX); return json(res, 200, { name, source: src }); }
    catch { return fail(res, 404, 'not_found', 'no such script'); }
  }
  if (path === '/api/script/save' && m === 'POST') {
    const b = await readJSON(req);
    const name = typeof b?.name === 'string' ? b.name : '';
    const source = typeof b?.source === 'string' ? b.source : '';
    if (!scriptNameOk(name)) return fail(res, 400, 'bad_name', 'bad script name');
    if (source.length > SCRIPT_SRC_MAX) return fail(res, 400, 'too_large', 'script too large');
    const dir = resolvePath(SCRIPT_DIR);
    if (dir.error) return fail(res, 503, 'sd_missing', 'SD card not mounted');
    try { await fsp.mkdir(dir.abs, { recursive: true }); await fsp.writeFile(join(dir.abs, name), source); }
    catch { return fail(res, 500, 'write_failed', 'could not write script'); }
    return json(res, 200, { ok: true, name, size: Buffer.byteLength(source) });
  }
  if (path === '/api/script/delete' && m === 'POST') {
    const b = await readJSON(req);
    const name = typeof b?.name === 'string' ? b.name : '';
    if (!scriptNameOk(name)) return fail(res, 400, 'bad_name', 'bad script name');
    const r = resolvePath(`${SCRIPT_DIR}/${name}`);
    if (r.error) return fail(res, 503, 'sd_missing', 'SD card not mounted');
    try { await fsp.unlink(r.abs); } catch { return fail(res, 404, 'not_found', 'no such script'); }
    return ok(res);
  }
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
  if (DEV && path === '/api/_dev/fido2' && m === 'POST') {
    const j = await readJSON(req);
    fido2.available = j?.available !== false;
    fido2.state = fido2.available ? 'idle' : 'unavailable';
    if (j?.reset) { fido2.creds = []; fido2.credentials = 0; }
    return ok(res);
  }
  if (DEV && path === '/api/_dev/presence-csi' && m === 'POST') {
    const j = await readJSON(req);
    if (!presenceCsi.active) return fail(res, 409, 'conflict', 'start CSI sensing first');
    const pattern = j?.pattern || 'stable', count = j?.count ?? 1, advance = j?.advance_ms || 0, zone = j?.zone || 'router';
    if (!['stable', 'motion'].includes(pattern)) return fail(res, 400, 'bad_request', 'pattern must be stable or motion');
    if (!['router', 'faro'].includes(zone)) return fail(res, 400, 'bad_request', 'zone must be router or faro');
    if (!Number.isInteger(count) || count < 0 || count > 500 || !Number.isFinite(advance) || advance < 0 || advance > 600000)
      return fail(res, 400, 'bad_request', 'count: 0-500; advance_ms: 0-600000');
    let z = zone === 'router' ? presenceCsi : presenceFaroZone;
    if (zone === 'faro') {
      const mac = j?.mac || '02:00:00:00:00:f1';
      if (!z.discovered || z.mac !== mac) {
        z = presenceFaroZone = newPresenceZone(true, presenceCsi.threshold);
        z.mac = mac;
      }
    }
    const oldPresent = z.present;
    for (let frame = 0; frame < count; frame++) {
      const wasCalibrating = z.calibrating, wasMotion = z.motion;
      z.samples++;
      presenceCsi.clock += 20;
      z.rssi = Number(j?.rssi ?? -50);
      if (zone === 'faro') z.last_seen_ms = presenceCsi.clock;
      if (z.calibrating) {
        z.calibrationFrames++;
        z.score = 0;
        if (z.calibrationFrames >= 32) z.calibrating = false;
      } else {
        z.motion = pattern === 'motion' && ++z.over >= 3;
        if (pattern !== 'motion') z.over = 0;
        z.score = pattern === 'motion' ? Math.min(1, 0.4 + frame / 20) : 0.01;
        z.level = pattern === 'motion' ? z.score * 0.08 : 0.004;
        if (z.motion) { z.present = true; z.last_motion_ms = presenceCsi.clock; }
      }
      const linked = zone === 'router' || presenceZoneFresh();
      const state = zone === 'router' ? (z.calibrating ? 'calibrating' : z.motion ? 'motion' : z.present ? 'present' : 'clear') : presenceZoneName(z, linked);
      if (wasCalibrating !== z.calibrating) publish('presence.csi', { event: 'calibrated', zone, state, present: z.present, motion: z.motion, score: z.score });
      if (wasMotion !== z.motion) publish('presence.csi', { event: z.motion ? 'motion' : 'still', zone, state, present: z.present, motion: z.motion, score: z.score });
      presenceStep();
    }
    presenceCsi.clock += advance;
    z.rate = count;
    presenceStep();
    if (oldPresent !== z.present) {
      const linked = zone === 'router' || presenceZoneFresh();
      publish('presence.csi', { event: z.present ? 'present' : 'clear', zone,
        state: zone === 'router' ? presenceCsi.state : presenceZoneName(z, linked), present: z.present, motion: z.motion, score: z.score });
    }
    return ok(res);
  }
  if (DEV && path === '/api/_dev/presence' && m === 'POST') {
    const j = await readJSON(req);
    if (!presence.active) return fail(res, 409, 'conflict', 'start discovery first');
    const mac = j?.mac || '02:00:00:00:00:01';
    let peer = presence.peers.find(p => p.mac === mac);
    if (!peer) {
      peer = { mac, name: j?.name || 'Test Watch', rssi: j?.rssi || -50, first_seen: nowEpoch(), last_seen: nowEpoch(), present: true };
      presence.peers.push(peer); presence.peers_ever++;
    } else { peer.present = true; peer.last_seen = nowEpoch(); }
    presence.history.push({ ts: nowEpoch(), mac, name: peer.name, event: 'appeared' });
    publish('presence', { event: 'appeared', mac, name: peer.name, rssi: peer.rssi });
    return ok(res);
  }
  if (DEV && path === '/api/_dev/evilportal' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.action === 'creds') {
      if (!evilportal.active) return fail(res, 409, 'conflict', 'start the portal first');
      addPortalCapture({ mac: j.mac || '00:00:00:00:00:01', username: j.username || 'user', password: j.password || 'pass' });
    } else if (j?.action === 'clone') {
      if (!evilportal.cloning) return fail(res, 409, 'conflict', 'start page cloning first');
      if (j?.ok === false) { evilportal.error = 'Simulator clone request failed'; stopPortalClone(); return ok(res); }
      const dir = join(ROOTS.sd, 'data/evilportal/portals');
      await fsp.mkdir(dir, { recursive: true });
      const html = `<!doctype html><html><head><title>Cloned lab portal</title></head><body><h1>Cloned: ${evilportal.cloneSsid}</h1><form method="post" action="/login"><input name="username"><input type="password" name="password"></form></body></html>`;
      await fsp.writeFile(join(dir, 'cloned.html.tmp'), html);
      await fsp.rename(join(dir, 'cloned.html.tmp'), join(dir, 'cloned.html'));
      evilportal.cloneBytes = Buffer.byteLength(html); evilportal.cloneName = 'cloned.html'; evilportal.error = null;
      stopPortalClone(); await reloadPortalTemplates();
    } else if (j?.action === 'verify') {
      const cap = evilportal.captures.find(c => c.verification === 'checking');
      if (!cap) return fail(res, 409, 'conflict', 'no PSK verification is pending');
      cap.verification = j?.error ? 'error' : j?.valid ? 'valid' : 'invalid';
      if (!j?.error) evilportal.settings[cap.verification === 'valid' ? 'verifiedOk' : 'verifiedBad']++;
    } else if (j?.action === 'probe') {
      if (!evilportal.karmaScanning) return fail(res, 409, 'conflict', 'start probe discovery first');
      const ssid = j?.ssid;
      if (typeof ssid !== 'string' || !ssid || Buffer.byteLength(ssid) > 32) return fail(res, 400, 'bad_request', 'ssid: 1-32 bytes');
      const hits = Math.max(1, Math.min(100, j?.hits || 1));
      let probe = evilportal.karmaProbes.find(p => p.ssid === ssid);
      if (!probe) {
        if (evilportal.karmaProbes.length >= 32) return fail(res, 409, 'conflict', 'probe list full');
        probe = { ssid, hits: 0, clients: [], rssi: j?.rssi || -60, channel: j?.channel || 1 };
        evilportal.karmaProbes.push(probe);
      }
      const mac = j?.mac || '02:00:00:00:00:01';
      probe.hits += hits;
      if (!probe.clients.includes(mac) && probe.clients.length < 8) probe.clients.push(mac);
      probe.lastClient = mac; probe.last_seen = nowEpoch();
    }
    return ok(res);
  }
  if (DEV && path === '/api/_dev/sentinel' && m === 'POST') {
    const j = await readJSON(req);
    if (!sentinel.active) return fail(res, 409, 'conflict', 'start monitoring first');
    const addr = j?.addr || '00:00:00:00:00:01';
    let device = sentinel.devices.find(d => d.addr === addr);
    if (!device) {
      device = { addr, name: j?.name || '', kind: j?.kind || 'Find My', rssi: j?.rssi || -60,
        first_seen: nowEpoch() - (j?.age_s || 0), last_seen: nowEpoch(), sightings: Math.max(0, (j?.sightings || 1) - 1), stalking: false, separated: !!j?.separated };
      sentinel.devices.push(device);
    }
    device.sightings++;
    device.last_seen = nowEpoch();
    if (device.sightings >= 4 && nowEpoch() - device.first_seen >= (device.separated ? 150 : 300)) device.stalking = true;
    sentinel.device_count = sentinel.devices.length;
    if (device.stalking) publish('sentinel', { type: 'stalking', addr, kind: device.kind, detail: `Persistent ${device.kind} tracker nearby`, sightings: device.sightings });
    return ok(res);
  }
  if (DEV && path === '/api/_dev/sniffer' && m === 'POST') {
    if (!sniffer.active) return fail(res, 409, 'conflict', 'start a capture first');
    const j = await readJSON(req);
    const count = Math.min(100, Math.max(1, j?.count || 50));
    const kind = j?.kind || 'beacon';
    const msg = j?.msg || 0;
    for (let i = 0; i < count && sniffer.active; i++) {
      const frame = snifferFrame(kind, msg);
      snifferAnalyse(frame);
      if (!snifferFilterMatch(frame)) continue;
      const record = pcapRecord(frame);
      if (sniffer.maxBytes && sniffer.bytes + record.length > sniffer.maxBytes) {
        sniffer.active = false; sniffer.error = 'Capture size limit reached';
        publish('sniffer', { active: false, reason: 'max_bytes' });
        break;
      }
      await fsp.appendFile(join(ROOTS.sd, 'data', 'sniffer', 'capture.pcap'), record);
      sniffer.packets++; sniffer.bytes += record.length;
    }
    return ok(res);
  }
  if (DEV && path === '/api/_dev/deauthflood' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.action === 'fail-target') {
      const t = deauthflood.progress[deauthflood.campaign.current];
      if (!deauthflood.active || !t) return fail(res, 409, 'conflict', 'no campaign target is running');
      t.failed++; deauthflood.txErrors++; t.state = 'error'; t.error = 'Simulator injection failure'; t.elapsedMs = Date.now() - t.started;
      advanceDeauthCampaign(); return ok(res);
    }
    deauthflood.active = true; deauthflood.sent = 100;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/beaconspam' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.action === 'fail-target') {
      const t = beaconspam.progress[beaconspam.campaign.current];
      if (!beaconspam.active || !t) return fail(res, 409, 'conflict', 'no campaign SSID is running');
      t.failed++; beaconspam.txErrors++; t.state = 'error'; t.error = 'Simulator injection failure'; t.elapsedMs = Date.now() - t.started;
      advanceBeaconCampaign(); return ok(res);
    }
    beaconspam.active = true; beaconspam.beacons = 200;
    return ok(res);
  }
  if (DEV && path === '/api/_dev/airspace' && m === 'POST') {
    const j = await readJSON(req);
    if (!airspace.active) return fail(res, 409, 'conflict', 'start monitoring first');
    if (j?.type === 'deauth_flood') { airspace.deauth_floods++; airspace.deauth_total += 100; }
    else if (j?.type === 'beacon_flood') { airspace.beacon_floods++; airspace.beacon_total += 100; }
    else if (j?.type === 'evil_twin') { airspace.evil_twins++; }
    else if (j?.type === 'broadcast_deauth') { airspace.broadcast_deauth = (airspace.broadcast_deauth || 0) + 1; airspace.deauth_total++; }
    else return fail(res, 400, 'bad_request', 'unknown alert type');
    publish('airspace', { type: j.type, detail: j?.detail || 'Injected simulator alert', bssid: j?.bssid || '', rssi: j?.rssi || 0 });
    return ok(res);
  }

  // === P9 NucleoOS parity tools ===
  // Wi-Fi Sniffer
  if (path === '/api/sniffer' && m === 'GET') return json(res, 200, snifferState());
  if (path === '/api/sniffer/start' && m === 'POST') {
    const j = await readJSON(req);
    if (sniffer.active) return fail(res, 409, 'conflict', 'already capturing');
    if (j?.channel === 0 || j?.channel > 13 || j?.channel < -1) return fail(res, 400, 'bad_request', 'channel -1 or 1-13');
    const filter = j?.filter || 'all';
    if (!SNIFFER_FILTERS.includes(filter)) return fail(res, 400, 'bad_request', 'filter must be all, management, data, beacons, probes or eapol');
    sniffer = { active: true, packets: 0, drops: 0, bytes: 24, channel: j?.channel > 0 ? j.channel : null,
      filter, maxBytes: j?.max_bytes || 0, file: '/sd/data/sniffer/capture.pcap', error: null,
      seen: 0, management: 0, control: 0, data: 0, beacons: 0, probes: 0, deauth: 0, eapol: 0, handshakes: 0, aps: [], hs: [] };
    await fsp.mkdir(join(ROOTS.sd, 'data', 'sniffer'), { recursive: true });
    await fsp.writeFile(join(ROOTS.sd, 'data', 'sniffer', 'capture.pcap'), pcapGlobalHeader());
    publish('sniffer', { active: true, filter });
    return ok(res);
  }
  if (path === '/api/sniffer/stop' && m === 'POST') {
    if (!sniffer.active) return fail(res, 409, 'conflict', 'not capturing');
    sniffer.active = false; publish('sniffer', { active: false, filter: sniffer.filter }); return ok(res);
  }

  // Airspace
  if (path === '/api/airspace' && m === 'GET') return json(res, 200, airspaceState());
  if (path === '/api/airspace/start' && m === 'POST') {
    airspace = { active: true, deauth_total: 0, deauth_floods: 0, beacon_total: 0, beacon_floods: 0, evil_twins: 0, broadcast_deauth: 0, started: nowEpoch() };
    return ok(res);
  }
  if (path === '/api/airspace/stop' && m === 'POST') { airspace.active = false; return ok(res); }

  // Sentinel
  if (path === '/api/sentinel' && m === 'GET') return json(res, 200, sentinelState());
  if (path === '/api/sentinel/start' && m === 'POST') { sentinel.active = true; sentinel.started = nowEpoch(); sentinel.devices = []; return ok(res); }
  if (path === '/api/sentinel/stop' && m === 'POST') { sentinel.active = false; return ok(res); }

  // Presence: dual-link Wi-Fi CSI, Faro transmitter, alarm and nearby ESP-NOW peers
  if (path === '/api/presence' && m === 'GET') return json(res, 200, presenceState());
  if (path === '/api/presence/start' && m === 'POST') { presence.active = true; presence.started = nowEpoch(); presence.peers = []; presence.history = []; presence.peers_ever = 0; return ok(res); }
  if (path === '/api/presence/stop' && m === 'POST') { presence.active = false; return ok(res); }
  if (path === '/api/presence/csi/start' && m === 'POST') {
    const j = await readJSON(req), threshold = j?.threshold ?? 0.35;
    if (typeof threshold !== 'number' || threshold < 0.05 || threshold > 0.95) return fail(res, 400, 'bad_request', 'threshold: 0.05-0.95');
    if (presenceFaro.active) return fail(res, 409, 'conflict', 'stop the Faro transmitter first');
    resetPresenceCsi(true, threshold);
    return ok(res);
  }
  if (path === '/api/presence/csi/stop' && m === 'POST') {
    presenceCsi.active = false; presenceCsi.state = 'stopped'; presenceCsi.calibrating = presenceCsi.motion = presenceCsi.present = false;
    presenceCsi.score = presenceCsi.rate = 0; presenceFaroZone.motion = presenceFaroZone.present = presenceFaroZone.calibrating = false;
    if (presenceAlarm.state !== 'off') { presenceAlarm.state = 'off'; presenceAlarm.zone = null; presenceAlarmEvent('sensing_stopped'); }
    return ok(res);
  }
  if (path === '/api/presence/csi/calibrate' && m === 'POST') {
    if (!presenceCsi.active) return fail(res, 409, 'conflict', 'start CSI sensing first');
    const j = await readJSON(req), zone = j?.zone || 'all';
    if (!['router', 'faro', 'all'].includes(zone)) return fail(res, 400, 'bad_request', 'zone must be router, faro or all');
    if (zone === 'faro' && !presenceZoneFresh()) return fail(res, 409, 'conflict', 'no live Faro link to calibrate');
    if (zone !== 'faro') {
      Object.assign(presenceCsi, { state: 'calibrating', calibrating: true, present: false, motion: false,
        score: 0, level: 0, noise: 0.005, calibrationFrames: 0, over: 0, last_motion_ms: presenceCsi.clock });
    }
    if (zone !== 'router' && presenceFaroZone.discovered) {
      Object.assign(presenceFaroZone, { calibrating: true, present: false, motion: false,
        score: 0, level: 0, noise: 0.005, calibrationFrames: 0, over: 0, last_motion_ms: presenceCsi.clock });
    }
    return ok(res);
  }
  if (path === '/api/presence/faro/start' && m === 'POST') {
    const j = await readJSON(req), channel = j?.channel ?? 1;
    if (!Number.isInteger(channel) || channel < 1 || channel > 13) return fail(res, 400, 'bad_request', 'channel: 1-13');
    if (presenceCsi.active) return fail(res, 409, 'conflict', 'stop CSI sensing before enabling the Faro transmitter');
    if (!presenceFaro.active) presenceFaro = { active: true, channel, mac: '02:00:00:00:00:f0', started: nowEpoch(), realStarted: Date.now(), sent: 0, tx_errors: 0, error: null };
    return ok(res);
  }
  if (path === '/api/presence/faro/stop' && m === 'POST') {
    presenceFaro.sent = presenceFaro.active ? Math.floor((Date.now() - presenceFaro.realStarted) / 10) : presenceFaro.sent || 0;
    presenceFaro.active = false;
    return ok(res);
  }
  if (path === '/api/presence/alarm/arm' && m === 'POST') {
    const j = await readJSON(req), exit_s = j?.exit_s ?? presenceAlarm.exit_s, duration_s = j?.duration_s ?? presenceAlarm.duration_s;
    if (!Number.isInteger(exit_s) || exit_s < 5 || exit_s > 120 || !Number.isInteger(duration_s) || duration_s < 10 || duration_s > 300)
      return fail(res, 400, 'bad_request', 'exit_s: 5-120; duration_s: 10-300');
    if (!presenceCsi.active || presenceCsi.calibrating || presenceFaro.active)
      return fail(res, 409, 'conflict', 'finish router calibration before arming; Faro transmitters cannot sense');
    if (presenceAlarm.state !== 'off') return fail(res, 409, 'conflict', 'disarm the alarm before changing its settings');
    presenceAlarm.exit_s = exit_s; presenceAlarm.duration_s = duration_s; presenceAlarm.state = 'arming';
    presenceAlarm.stateMs = presenceCsi.clock; presenceAlarm.zone = null; presenceAlarm.error = null;
    presenceAlarmEvent('arming');
    return ok(res);
  }
  if (path === '/api/presence/alarm/disarm' && m === 'POST') {
    if (presenceAlarm.state !== 'off') { presenceAlarm.state = 'off'; presenceAlarm.zone = null; presenceAlarmEvent('disarmed'); }
    presenceAlarm.error = null;
    return ok(res);
  }

  // FIDO2 (explicit simulator fake, unavailable until its dev hook enables it)
  if (path === '/api/fido2' && m === 'GET') return json(res, 200, fido2State());
  if (path === '/api/fido2/register' && m === 'POST') {
    const j = await readJSON(req);
    if (!fido2.available) return fail(res, 503, 'fido_unavailable', 'Enable the simulated FIDO2 transport first');
    if (!j?.rp || !j?.user) return fail(res, 400, 'bad_request', 'rp and user are required');
    fido2.creds.push({ rp: j.rp, user: j.user }); fido2.credentials = fido2.creds.length; return ok(res);
  }
  if (path === '/api/fido2/authenticate' && m === 'POST') {
    const j = await readJSON(req);
    if (!fido2.available) return fail(res, 503, 'fido_unavailable', 'Enable the simulated FIDO2 transport first');
    if (!fido2.creds.some(c => c.rp === j?.rp)) return fail(res, 404, 'not_found', 'no credential for this RP');
    return ok(res);
  }
  if (path === '/api/fido2/credentials' && m === 'GET') return json(res, 200, { credentials: fido2.creds.map((c,i) => ({ index: i, rp: c.rp, user: c.user })) });
  if (path === '/api/fido2/remove' && m === 'POST') {
    const i = (await readJSON(req))?.index;
    if (!Number.isInteger(i) || i < 0 || i >= fido2.creds.length) return fail(res, 400, 'bad_request', 'invalid credential index');
    fido2.creds.splice(i, 1); fido2.credentials = fido2.creds.length; return ok(res);
  }

  // Evil Portal
  if (path === '/api/evilportal' && m === 'GET') return json(res, 200, evilportalState());
  if ((path === '/api/evilportal/start' || path === '/api/evilportal/start-twin') && m === 'POST') {
    const j = await readJSON(req);
    if (j?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may test this network');
    if (j?.gateway && !/^(?:\d{1,3}\.){3}\d{1,3}$/.test(j.gateway)) return fail(res, 400, 'bad_request', 'Gateway must be a valid IPv4 address');
    if (evilportal.active || evilportal.karmaScanning || evilportal.cloning) return fail(res, 409, 'conflict', 'already running');
    if (j?.coherent && (!path.endsWith('twin') || typeof j?.psk !== 'string' || j.psk.length < 8 || j.psk.length > 63 || !/^[\x20-\x7e]+$/.test(j.psk))) {
      return fail(res, 400, 'bad_request', 'WPA2 twin requires the authorized lab AP PSK (8-63 ASCII bytes)');
    }
    if (path.endsWith('twin') && !/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(j?.bssid || '')) return fail(res, 400, 'bad_request', 'invalid BSSID format');
    const template = selectPortalTemplate(j?.templateId ?? 0);
    if (!template) return fail(res, 400, 'bad_request', 'Unknown, changed or invalid SD portal template');
    evilportal.active = true; evilportal.mode = path.endsWith('twin') ? 1 : 0;
    evilportal.html = template.html;
    evilportal.ssid = j?.ssid || 'Free Wi-Fi'; evilportal.twinBssid = j?.bssid || '';
    evilportal.coherent = !!j?.coherent; evilportal.settings.verifiedOk = 0; evilportal.settings.verifiedBad = 0;
    evilportal.channel = j?.channel || 1; evilportal.started = nowEpoch(); evilportal.attempts = 0;
    if (j?.gateway) evilportal.settings.gateway = j.gateway;
    return ok(res, { ssid: evilportal.ssid, mode: evilportal.mode ? 'twin' : 'standalone' });
  }
  if (path === '/api/evilportal/templates/reload' && m === 'POST') return ok(res, await reloadPortalTemplates());
  if (path === '/api/evilportal/karma-scan' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may test this network');
    const seconds = j?.seconds ?? 30;
    if (!Number.isInteger(seconds) || seconds < 1 || seconds > 120) return fail(res, 400, 'bad_request', 'seconds must be 1-120');
    if (evilportal.active || evilportal.karmaScanning || evilportal.cloning) return fail(res, 409, 'conflict', 'Stop the portal, page cloning or current probe discovery first');
    evilportal.karmaProbes = []; evilportal.karmaScanning = true;
    evilportal.karmaTimer = setTimeout(finishKarmaScan, seconds * 1000);
    return json(res, 202, { ok: true, scanning: true, seconds });
  }
  if (path === '/api/evilportal/karma' && m === 'GET') return json(res, 200, { scanning: evilportal.karmaScanning,
    probes: evilportal.karmaProbes.map(p => ({ ...p, clients: p.clients.length })) });
  if (path === '/api/evilportal/start-karma' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may test this network');
    const choices = evilportal.karmaProbes.filter(p => !j?.ssid || p.ssid === j.ssid).sort((a, b) => b.hits - a.hits);
    if (!choices.length) return fail(res, 409, 'conflict', j?.ssid ? 'Requested SSID was not discovered' : 'Run probe discovery first');
    if (evilportal.cloning) return fail(res, 409, 'conflict', 'Stop page cloning first');
    const template = selectPortalTemplate(j?.templateId ?? 0);
    if (!template) return fail(res, 400, 'bad_request', 'Unknown, changed or invalid SD portal template');
    if (j?.gateway && !/^(?:\d{1,3}\.){3}\d{1,3}$/.test(j.gateway)) return fail(res, 400, 'bad_request', 'Gateway must be a valid IPv4 address');
    if (j?.channel && (!Number.isInteger(j.channel) || j.channel < 1 || j.channel > 13)) return fail(res, 400, 'bad_request', 'channel must be 1-13');
    finishKarmaScan();
    evilportal.active = true; evilportal.mode = 2; evilportal.ssid = choices[0].ssid; evilportal.html = template.html;
    evilportal.coherent = false; evilportal.settings.verifiedOk = 0; evilportal.settings.verifiedBad = 0;
    evilportal.channel = j?.channel || 1; evilportal.started = nowEpoch(); evilportal.attempts = 0;
    if (j?.gateway) evilportal.settings.gateway = j.gateway;
    return ok(res, { ssid: evilportal.ssid, mode: 'karma' });
  }
  if (path === '/api/evilportal/clone' && m === 'POST') {
    const j = await readJSON(req);
    if (j?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may clone this open lab network');
    if (typeof j?.ssid !== 'string' || !j.ssid || Buffer.byteLength(j.ssid) > 32) return fail(res, 400, 'bad_request', 'SSID must be 1-32 UTF-8 bytes');
    if (evilportal.active || evilportal.karmaScanning || evilportal.cloning) return fail(res, 409, 'conflict', 'Stop the portal, probe discovery or current clone first');
    evilportal.cloning = true; evilportal.cloneSsid = j.ssid; evilportal.cloneName = ''; evilportal.cloneBytes = 0; evilportal.error = null;
    return json(res, 202, { ok: true, cloning: true, ssid: j.ssid });
  }
  if (path === '/api/evilportal/settings' && m === 'PUT') {
    const j = await readJSON(req);
    if ('verify' in (j || {}) && typeof j.verify !== 'boolean') return fail(res, 400, 'bad_request', 'verify must be boolean');
    evilportal.settings = { ...evilportal.settings, ...j };
    if (j?.verify === false) for (const c of evilportal.captures) if (['queued', 'checking'].includes(c.verification)) c.verification = 'off';
    return ok(res);
  }
  if (path === '/api/evilportal/stop' && m === 'POST') { finishKarmaScan(); stopPortalClone(); evilportal.active = false; evilportal.coherent = false; evilportal.clients = 0; return ok(res); }
  if (path === '/api/evilportal/captures' && m === 'GET') return json(res, 200, { captures: evilportal.captures.map(c => {
    const mode = evilportal.settings.pwdMode, p = c.password;
    return { ...c, password: mode === 2 ? '******' : mode === 3 ? `${p.length} chars` : mode === 1 && p.length > 2 ? p[0] + '*'.repeat(p.length - 2) + p.at(-1) : p };
  }) });
  if (path === '/api/evilportal/clear' && m === 'POST') { evilportal.captures = []; evilportal.attempts = 0; evilportal.settings.verifiedOk = 0; evilportal.settings.verifiedBad = 0; return ok(res); }

  // Deauth flood
  if (path === '/api/deauthflood' && m === 'GET') return json(res, 200, deauthfloodState());
  if (path === '/api/deauthflood/start' && m === 'POST') {
    const b = await readJSON(req);
    if (b?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may test this network');
    if (!/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(b?.bssid || '')) return fail(res, 400, 'bad_request', 'invalid BSSID format');
    if (!Number.isInteger(b?.channel) || b.channel < 1 || b.channel > 13) return fail(res, 400, 'bad_request', 'channel must be 1-13');
    if (!Number.isInteger(b?.rate) || b.rate < 1 || b.rate > 100) return fail(res, 400, 'bad_request', 'rate must be 1-100 packets/s');
    if (deauthflood.active) return fail(res, 409, 'conflict', 'already running');
    clearInterval(deauthflood.campaignTimer); deauthflood.campaignTimer = null;
    deauthflood.campaign = { active: false, current: 0, completed: 0, count: 0, framesPerTarget: 100 }; deauthflood.progress = [];
    deauthflood.reconnect = { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 };
    deauthflood = { ...deauthflood, active: true, targetBssid: b.bssid, targetStation: b?.station || 'ff:ff:ff:ff:ff:ff',
      targetSsid: deauthScan.targets.find(t => t.bssid === b.bssid)?.ssid || '', channel: b?.channel || 1,
      ratePps: b?.rate || 10, sent: 0, txErrors: 0, started: nowEpoch(), error: null };
    return ok(res);
  }
  if (path === '/api/deauthflood/campaign/start' && m === 'POST') {
    const b = await readJSON(req);
    if (b?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm every selected lab target is authorized');
    if (deauthflood.active) return fail(res, 409, 'conflict', 'already running');
    if (!Array.isArray(b?.targets) || !b.targets.length || b.targets.length > 8 || new Set(b.targets.map(x => x.toLowerCase())).size !== b.targets.length ||
        b.targets.some(x => !/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(x))) return fail(res, 400, 'bad_request', 'Select 1-8 unique target BSSIDs');
    if (!Number.isInteger(b?.frames) || b.frames < 1 || b.frames > 100000 || !Number.isInteger(b?.rate) || b.rate < 1 || b.rate > 100) return fail(res, 400, 'bad_request', 'frames: 1-100000; rate: 1-100');
    if (b?.station && !/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(b.station)) return fail(res, 400, 'bad_request', 'invalid station MAC');
    const targets = b.targets.map(mac => deauthScan.targets.find(t => t.bssid.toLowerCase() === mac.toLowerCase()));
    if (targets.some(t => !t)) return fail(res, 400, 'bad_request', 'Every campaign BSSID must come from the latest scan');
    if (!targets.some(t => t.channel >= 1 && t.channel <= 13)) return fail(res, 400, 'bad_request', 'No selected target is on a supported 2.4 GHz channel');
    startMockDeauthCampaign(targets, b.frames, b.rate, b.station || 'ff:ff:ff:ff:ff:ff');
    return ok(res, { active: deauthflood.campaign.active });
  }
  if ((path === '/api/deauthflood/stop' || path === '/api/deauthflood/campaign/stop') && m === 'POST') { clearInterval(deauthflood.campaignTimer); deauthflood.campaignTimer = null; deauthflood.active = false; deauthflood.campaign.active = false; if (deauthflood.progress.length) mockReconnect(deauthflood); return ok(res); }
  if (path === '/api/deauthflood/scan' && m === 'POST') {
    if (deauthflood.active) return fail(res, 409, 'conflict', 'stop before scanning');
    if (!deauthScan.scanning) {
      deauthScan.scanning = true; deauthScan.error = null;
      deauthScan.timer = setTimeout(() => {
        deauthScan.targets = [
          { ssid: 'Lab AP', bssid: '02:00:00:00:00:01', channel: 1, rssi: -42, auth: 'wpa2' },
          { ssid: 'Test Guest', bssid: '02:00:00:00:00:02', channel: 6, rssi: -66, auth: 'open' },
          { ssid: 'Lab PMF', bssid: '02:00:00:00:00:03', channel: 11, rssi: -70, auth: 'wpa3' },
        ];
        deauthScan.scanning = false;
      }, 400);
    }
    return json(res, 202, { ok: true });
  }
  if (path === '/api/deauthflood/targets' && m === 'GET') return json(res, 200, { scanning: deauthScan.scanning, error: deauthScan.error, targets: deauthScan.targets });

  // Beacon spam
  if (path === '/api/beaconspam' && m === 'GET') return json(res, 200, beaconspamState());
  if (path === '/api/beaconspam/start' && m === 'POST') {
    const b = await readJSON(req);
    if (b?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm that you own or may test this environment');
    if (beaconspam.active) return fail(res, 409, 'conflict', 'already running');
    if (b?.mode === 'custom' && !beaconspam.customSsids.length) return fail(res, 400, 'bad_request', 'add custom SSIDs first');
    beaconspam.prefix = b?.prefix || 'FreeNet';
    clearInterval(beaconspam.campaignTimer); beaconspam.campaignTimer = null;
    beaconspam.campaign = { active: false, current: 0, completed: 0, count: 0, framesPerSsid: 10 }; beaconspam.progress = [];
    beaconspam.reconnect = { pending: false, reconnected: false, attempts: 0, elapsedMs: 0 };
    beaconspam.active = true;
    beaconspam.mode = b?.mode === 'custom' ? 2 : 0;
    beaconspam.ssids = beaconspam.mode === 2 ? beaconspam.customSsids.length : (b?.count || 20);
    beaconspam.channel = b?.channel ?? 0;
    beaconspam.beacons = 0;
    return ok(res);
  }
  if (path === '/api/beaconspam/ssids' && m === 'PUT') {
    const list = await readJSON(req);
    if (!Array.isArray(list) || list.length > 64 || list.some(x => typeof x !== 'string' || !x || Buffer.byteLength(x) > 32)) return fail(res, 400, 'bad_request', 'invalid SSID list');
    beaconspam.customSsids = list; return ok(res, { count: list.length });
  }
  if (path === '/api/beaconspam/campaign/start' && m === 'POST') {
    const b = await readJSON(req);
    if (b?.authorized !== true) return fail(res, 400, 'authorization_required', 'Confirm this beacon campaign is authorized');
    if (beaconspam.active) return fail(res, 409, 'conflict', 'already running');
    if (!['generated', 'custom'].includes(b?.mode)) return fail(res, 400, 'bad_request', 'mode: generated or custom');
    if (!Number.isInteger(b?.framesPerSsid) || b.framesPerSsid < 1 || b.framesPerSsid > 10000) return fail(res, 400, 'bad_request', 'framesPerSsid: 1-10000');
    if (!Number.isInteger(b?.channel) || b.channel < 0 || b.channel > 13) return fail(res, 400, 'bad_request', 'channel: 0-13');
    if (b.mode === 'generated' && (!Number.isInteger(b?.count) || b.count < 1 || b.count > 64 || typeof b?.prefix !== 'string' || !b.prefix || Buffer.byteLength(b.prefix) > 20)) return fail(res, 400, 'bad_request', 'Campaigns support 1-64 SSIDs and a 1-20 byte prefix');
    if (b.mode === 'custom' && (!beaconspam.customSsids.length || beaconspam.customSsids.length > 64)) return fail(res, 400, 'bad_request', 'Save 1-64 custom SSIDs first');
    beaconspam.prefix = b.prefix || 'FreeNet'; beaconspam.mode = b.mode === 'custom' ? 2 : 0;
    beaconspam.ssids = b.mode === 'custom' ? beaconspam.customSsids.length : b.count;
    const ssids = b.mode === 'custom' ? [...beaconspam.customSsids] : Array.from({ length: b.count }, (_, i) => `${beaconspam.prefix}${String(i).padStart(4, '0')}`);
    startMockBeaconCampaign(ssids, b.framesPerSsid, b.channel);
    return ok(res, { active: beaconspam.campaign.active });
  }
  if ((path === '/api/beaconspam/stop' || path === '/api/beaconspam/campaign/stop') && m === 'POST') { clearInterval(beaconspam.campaignTimer); beaconspam.campaignTimer = null; beaconspam.active = false; beaconspam.campaign.active = false; if (beaconspam.progress.length) mockReconnect(beaconspam); return ok(res); }

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
      if (evilportal.active) {
        if (path === '/login' && req.method === 'POST') {
          const form = new URLSearchParams((await readBody(req)).toString('utf8'));
          const username = form.get('username') || '', password = form.get('password') || '';
          if (!username || !password) return json(res, 400, { error: 'bad_request' });
          addPortalCapture({ username, password });
          res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
          return res.end('<!doctype html><title>Connected</title><h1>Connected</h1>');
        }
        if (req.method === 'GET' || req.method === 'HEAD') {
          res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
          return res.end(evilportal.html || '<!doctype html><html><title>Network Login</title><h1>Network Login</h1><form method="post" action="/login"><label>Username<input name="username"></label><label>Password<input type="password" name="password"></label><button>Connect</button></form></html>');
        }
      }
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
