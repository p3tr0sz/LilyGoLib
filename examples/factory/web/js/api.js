// Client for the watch's web API v1 (docs/web-api.md).

export class ApiError extends Error {
  constructor(status, code, message, data) {
    super(message || code || `HTTP ${status}`);
    this.status = status;
    this.code = code;
    this.data = data;
  }
}

const listeners401 = new Set();
export function onUnpaired(fn) { listeners401.add(fn); return () => listeners401.delete(fn); }

async function request(method, path, { json, body, headers = {} } = {}) {
  const opts = { method, headers: { ...headers }, credentials: 'same-origin', cache: 'no-store' };
  if (json !== undefined) { opts.headers['content-type'] = 'application/json'; opts.body = JSON.stringify(json); }
  else if (body !== undefined) opts.body = body;
  let r;
  try { r = await fetch(path, opts); }
  catch { throw new ApiError(0, 'offline', 'The watch is not answering'); }
  const text = await r.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = text; }
  if (!r.ok) {
    const code = (data && data.error) || `http_${r.status}`;
    if (r.status === 401 && code === 'unpaired') listeners401.forEach((fn) => fn());
    throw new ApiError(r.status, code, data && data.message, data);
  }
  return data;
}

const q = (p) => encodeURIComponent(p);

export const api = {
  caps: () => request('GET', '/api/caps'),
  device: () => request('GET', '/api/device'),
  auth: () => request('GET', '/api/auth'),
  pair: (pin) => request('POST', '/api/auth/pair', { json: { pin } }),
  logout: (all = false) => request('POST', '/api/auth/logout', { json: { all } }),
  setTime: (epoch, offset_min, tz) => request('POST', '/api/time', { json: { epoch, offset_min, tz } }),
  settings: () => request('GET', '/api/settings'),
  patchSettings: (patch) => request('PATCH', '/api/settings', { json: patch }),
  reboot: () => request('POST', '/api/system/reboot'),
  input: (body) => request('POST', '/api/input', { json: body }),
  display: (body) => request('POST', '/api/display', { json: body }),
  sd: () => request('GET', '/api/sd'),

  // Tools (P4)
  monitor: () => request('GET', '/api/monitor'),
  logs: (after = 0) => request('GET', `/api/logs?after=${after}`),
  notify: (body) => request('POST', '/api/notify', { json: body }),
  notifications: () => request('GET', '/api/notify'),
  sensors: () => request('GET', '/api/sensors'),
  gnss: () => request('GET', '/api/gnss'),
  loraConfig: () => request('GET', '/api/lora/config'),
  loraPatch: (patch) => request('PATCH', '/api/lora/config', { json: patch }),
  loraSend: (body) => request('POST', '/api/lora/send', { json: body }),
  loraLog: () => request('GET', '/api/lora/log'),
  nfc: () => request('GET', '/api/nfc'),
  nfcScan: (seconds) => request('POST', '/api/nfc/scan', { json: { seconds } }),
  nfcStop: () => request('POST', '/api/nfc/stop'),
  recorder: () => request('GET', '/api/recorder'),
  recStart: (body = {}) => request('POST', '/api/recorder/start', { json: body }),
  recStop: () => request('POST', '/api/recorder/stop'),
  audio: () => request('GET', '/api/audio'),
  audioPlay: (path) => request('POST', '/api/audio/play', { json: { path } }),
  audioStop: () => request('POST', '/api/audio/stop'),
  audioPatch: (patch) => request('PATCH', '/api/audio', { json: patch }),
  wifiScan: () => request('POST', '/api/wifi/scan'),
  wifiResult: () => request('GET', '/api/wifi/scan'),
  bleScan: (seconds) => request('POST', '/api/ble/scan', { json: { seconds } }),
  bleResult: () => request('GET', '/api/ble/scan'),
  // P5
  link: () => request('GET', '/api/link'),
  linkEnable: (on) => request('POST', '/api/link/enable', { json: { on } }),
  linkSend: (path, to_mac) => request('POST', '/api/link/send', { json: { path, to_mac } }),
  linkCancel: () => request('POST', '/api/link/cancel'),
  hid: () => request('GET', '/api/hid'),
  hidType: (text) => request('POST', '/api/hid/type', { json: { text } }),
  hidRun: (body) => request('POST', '/api/hid/run', { json: body }),
  hidStop: () => request('POST', '/api/hid/stop'),
  net: () => request('GET', '/api/net'),
  netResolve: (host) => request('POST', '/api/net/resolve', { json: { host } }),
  netTcp: (host, port) => request('POST', '/api/net/tcp', { json: { host, port } }),
  netChannels: () => request('GET', '/api/net/channels'),
  ota: () => request('GET', '/api/ota'),
  otaCommit: () => request('POST', '/api/ota/commit'),
  otaRollback: () => request('POST', '/api/ota/rollback'),
  // OTA upload streams raw bytes with progress (like file upload).
  otaUpload(blob, onProgress) {
    return new Promise((resolve, reject) => {
      const x = new XMLHttpRequest();
      x.open('POST', '/api/ota/upload');
      x.upload.onprogress = (e) => { if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total); };
      x.onload = () => {
        let data = null; try { data = JSON.parse(x.responseText); } catch {}
        if (x.status >= 200 && x.status < 300) resolve(data);
        else { if (x.status === 401) listeners401.forEach((fn) => fn()); reject(new ApiError(x.status, data && data.error, data && data.message, data)); }
      };
      x.onerror = () => reject(new ApiError(0, 'offline', 'Upload interrupted'));
      x.send(blob);
    });
  },
  writeText: (path, text) => request('PUT', `/api/files/content?path=${q(path)}`, { body: text, headers: { 'content-type': 'application/octet-stream' } }),
  sdMount: () => request('POST', '/api/sd/mount'),
  sdUnmount: () => request('POST', '/api/sd/unmount'),

  list: (path) => request('GET', `/api/files?path=${q(path)}`),
  mkdir: (path) => request('POST', '/api/files/mkdir', { json: { path } }),
  move: (from, to, overwrite = false) => request('POST', '/api/files/move', { json: { from, to, overwrite } }),
  remove: (path, recursive = false) => request('DELETE', `/api/files?path=${q(path)}${recursive ? '&recursive=1' : ''}`),
  contentUrl: (path) => `/api/files/content?path=${q(path)}`,
  readText: async (path, max = 1 << 20) => {
    const r = await fetch(`/api/files/content?path=${q(path)}`, { headers: { range: `bytes=0-${max - 1}` }, credentials: 'same-origin' });
    if (r.status === 416) return '';            // empty file
    if (!r.ok) throw new ApiError(r.status, 'read_failed');
    return r.text();
  },

  // Upload with progress (fetch has no upload progress). Resolves with {ok, size}.
  write(path, blob, onProgress) {
    return new Promise((resolve, reject) => {
      const x = new XMLHttpRequest();
      x.open('PUT', `/api/files/content?path=${q(path)}`);
      x.upload.onprogress = (e) => { if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total); };
      x.onload = () => {
        let data = null; try { data = JSON.parse(x.responseText); } catch {}
        if (x.status >= 200 && x.status < 300) resolve(data);
        else {
          if (x.status === 401) listeners401.forEach((fn) => fn());
          reject(new ApiError(x.status, data && data.error, data && data.message, data));
        }
      };
      x.onerror = () => reject(new ApiError(0, 'offline', 'Upload interrupted'));
      x.send(blob);
    });
  },
};

// ---------------------------------------------------------------- events

// One event socket for the whole desktop. Reconnects with backoff, except after seat.lost
// (another desktop took over) until reconnect() is called.
export const events = (() => {
  const topics = new Map();
  const state = new Set();
  let ws = null, retry = 0, timer = null, seatLost = false, wanted = false, status = 'closed';

  const setStatus = (s) => { status = s; state.forEach((fn) => fn(s)); };
  const emit = (t, d) => {
    (topics.get(t) || []).forEach((fn) => { try { fn(d); } catch (e) { console.error(e); } });
    (topics.get('*') || []).forEach((fn) => { try { fn(d, t); } catch (e) { console.error(e); } });
  };

  function open() {
    clearTimeout(timer);
    if (!wanted || seatLost) return;
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/events`;
    try { ws = new WebSocket(url); } catch { schedule(); return; }
    setStatus('connecting');
    ws.onopen = () => { retry = 0; };
    ws.onmessage = (e) => {
      let m; try { m = JSON.parse(e.data); } catch { return; }
      if (m.t === 'hello') setStatus('open');
      if (m.t === 'seat.lost') { seatLost = true; setStatus('seat-lost'); }
      emit(m.t, m.d || {});
    };
    ws.onclose = () => { ws = null; if (!seatLost) { setStatus('closed'); schedule(); } };
    ws.onerror = () => {};
  }
  function schedule() {
    if (!wanted || seatLost) return;
    retry = Math.min(retry + 1, 6);
    timer = setTimeout(open, 500 * 2 ** retry);
  }
  return {
    start() { wanted = true; if (!ws) open(); },
    stop() { wanted = false; clearTimeout(timer); if (ws) ws.close(); ws = null; setStatus('closed'); },
    reconnect() { seatLost = false; retry = 0; if (ws) ws.close(); ws = null; open(); },
    on(topic, fn) {
      if (!topics.has(topic)) topics.set(topic, new Set());
      topics.get(topic).add(fn);
      return () => topics.get(topic).delete(fn);
    },
    onStatus(fn) { state.add(fn); fn(status); return () => state.delete(fn); },
    get status() { return status; },
  };
})();
