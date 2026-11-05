// Screen: the watch screen, live, and the watch driven from here: tap, hold and swipe on the picture.
import { h, icon, toast } from '../../js/ui.js';

const FAST_MS = 250;        // just after an input: follow the animation
const SLOW_MS = 900;        // otherwise
const FAST_FOR_MS = 3000;
const HOLD_MS = 600;        // longer press = long press on the watch
const MOVE_PX = 12;         // more movement (in watch pixels) = swipe

function decode(buf, w, h, img) {
  const src = new DataView(buf);
  const out = img.data;
  let p = 0;
  const raw = buf.byteLength === w * h * 2;
  const put = (c) => {
    out[p++] = ((c >> 11) & 31) * 255 / 31 | 0;
    out[p++] = ((c >> 5) & 63) * 255 / 63 | 0;
    out[p++] = (c & 31) * 255 / 31 | 0;
    out[p++] = 255;
  };
  if (raw) {
    for (let i = 0; i < w * h; i++) put(src.getUint16(i * 2, true));
    return;
  }
  for (let i = 0; i + 3 < buf.byteLength && p < out.length; i += 4) {
    const n = src.getUint16(i, true), c = src.getUint16(i + 2, true);
    for (let k = 0; k < n && p < out.length; k++) put(c);
  }
}

export function mount(el, ctx) {
  const { api, caps } = ctx;
  const canInput = caps.includes('input');
  const canDisplay = caps.includes('display');
  const canvas = h('canvas', { class: 'screen-canvas', width: 502, height: 410, 'aria-label': 'Watch screen', tabindex: '0' });
  const status = h('span', { class: 'muted screen-status' }, 'Connecting…');
  const stage = h('div', { class: 'screen-stage' }, canvas);
  const btn = (label, ic, fn, title) => h('button', { class: 'btn', title: title || label, onclick: fn }, icon(ic), h('span', { class: 'lbl' }, label));
  const input = (body) => api.input(body)
    .then(() => { fastUntil = Date.now() + FAST_FOR_MS; poll(true); })
    .catch((e) => toast(`Input failed: ${e.message}`, 'error'));

  const tools = h('div', { class: 'toolbar' }, status, h('span', { style: { flex: 1 } }),
    canInput ? btn('Wake', 'sun', () => input({ type: 'button', name: 'wake' }), 'Wake the screen') : null,
    canInput ? btn('Power menu', 'power', () => input({ type: 'button', name: 'power_menu' })) : null,
    canDisplay ? btn('Screen off', 'moon', () => api.display({ on: false }).then(() => poll(true)).catch((e) => toast(e.message, 'error'))) : null,
    btn('Save image', 'download', save));
  el.append(h('div', { class: 'app' }, tools, stage,
    canInput ? h('div', { class: 'screen-hint muted' }, 'Tap, hold or swipe on the picture to use the watch.') : null));

  const g = canvas.getContext('2d');
  let img = null, etag = '', w = 0, hgt = 0, timer = null, alive = true, busy = false;
  let fastUntil = 0, frames = 0, bytes = 0, lastStat = Date.now();

  async function poll(now = false) {
    clearTimeout(timer);
    if (!alive) return;
    if (busy) { timer = setTimeout(poll, 50); return; }
    if (document.hidden && !now) { timer = setTimeout(poll, SLOW_MS); return; }
    busy = true;
    try {
      const r = await fetch('/api/screen', { credentials: 'same-origin', cache: 'no-store', headers: etag ? { 'if-none-match': etag } : {} });
      if (r.status === 200) {
        const nw = +r.headers.get('x-width'), nh = +r.headers.get('x-height');
        const buf = await r.arrayBuffer();
        if (nw !== w || nh !== hgt) {
          w = nw; hgt = nh;
          canvas.width = w; canvas.height = hgt;
          img = g.createImageData(w, hgt);
        }
        decode(buf, w, hgt, img);
        g.putImageData(img, 0, 0);
        etag = r.headers.get('etag') || '';
        frames++; bytes += buf.byteLength;
        canvas.dataset.frames = String(frames);
      } else if (r.status !== 304) {
        status.textContent = r.status === 503 ? 'The watch is busy' : `Screen not available (HTTP ${r.status})`;
      }
      const t = Date.now();
      if (t - lastStat > 2000) {
        status.textContent = `${w}×${hgt} · ${(bytes / 1024 / ((t - lastStat) / 1000)).toFixed(0)} KB/s`;
        bytes = 0; lastStat = t;
      } else if (frames === 1 && r.status === 200) {
        status.textContent = `${w}×${hgt}`;
      }
    } catch {
      status.textContent = 'Watch not reachable';
    } finally {
      busy = false;
      if (alive) timer = setTimeout(poll, Date.now() < fastUntil ? FAST_MS : SLOW_MS);
    }
  }

  // Watch coordinates of a pointer event on the scaled canvas.
  const toWatch = (e) => {
    const r = canvas.getBoundingClientRect();
    return {
      x: Math.max(0, Math.min(w - 1, Math.round(((e.clientX - r.left) / r.width) * w))),
      y: Math.max(0, Math.min(hgt - 1, Math.round(((e.clientY - r.top) / r.height) * hgt))),
    };
  };
  if (canInput) {
    let start = null;
    canvas.addEventListener('pointerdown', (e) => {
      if (!w) return;
      try { canvas.setPointerCapture(e.pointerId); } catch {}
      start = { ...toWatch(e), t: Date.now() };
      e.preventDefault();
    });
    canvas.addEventListener('pointerup', (e) => {
      if (!start) return;
      const end = toWatch(e);
      const dist = Math.hypot(end.x - start.x, end.y - start.y);
      const held = Date.now() - start.t;
      if (dist > MOVE_PX) input({ type: 'swipe', x: start.x, y: start.y, x2: end.x, y2: end.y });
      else input({ type: held >= HOLD_MS ? 'long' : 'tap', x: start.x, y: start.y });
      start = null;
    });
    canvas.addEventListener('pointercancel', () => { start = null; });
  }

  function save() {
    if (!w) return;
    canvas.toBlob((blob) => {
      const a = h('a', { href: URL.createObjectURL(blob), download: `watch-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.png` });
      document.body.append(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }, 'image/png');
  }

  const onVis = () => { if (!document.hidden) poll(true); };
  document.addEventListener('visibilitychange', onVis);
  poll(true);
  return { unmount() { alive = false; clearTimeout(timer); document.removeEventListener('visibilitychange', onVis); } };
}
