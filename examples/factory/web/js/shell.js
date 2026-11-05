// The desktop shell: pairing, status bar, launcher and windows.
import { api, events, onUnpaired, ApiError } from './api.js';
import { h, icon, toast } from './ui.js';

// Apps the desktop knows. An app is shown only when the watch has every capability in `caps`
// and at least one in `capsAny`.
const APPS = [
  { id: 'files', title: 'Files', icon: 'folder', caps: ['files'], load: () => import('../apps/files/app.js') },
  { id: 'screen', title: 'Screen', icon: 'screen', caps: ['screen'], load: () => import('../apps/screen/app.js') },
  { id: 'notify', title: 'Notify', icon: 'bell', caps: ['notify'], load: () => import('../apps/notify/app.js') },
  { id: 'monitor', title: 'Monitor', icon: 'monitor', caps: ['monitor'], load: () => import('../apps/monitor/app.js') },
  { id: 'sensors', title: 'Sensors', icon: 'sensors', caps: ['sensors.imu'], load: () => import('../apps/sensors/app.js') },
  { id: 'gnss', title: 'GNSS', icon: 'gnss', caps: ['gnss'], load: () => import('../apps/gnss/app.js') },
  { id: 'lora', title: 'LoRa', icon: 'lora', caps: ['lora'], load: () => import('../apps/lora/app.js') },
  { id: 'nfc', title: 'NFC', icon: 'nfc', caps: ['nfc'], load: () => import('../apps/nfc/app.js') },
  { id: 'recorder', title: 'Recorder', icon: 'mic', caps: ['recorder'], load: () => import('../apps/recorder/app.js') },
  { id: 'scanners', title: 'Scanners', icon: 'radar', capsAny: ['wifi.scan', 'ble.scan'], load: () => import('../apps/scanners/app.js') },
  { id: 'link', title: 'Link', icon: 'link', caps: ['link.espnow'], load: () => import('../apps/link/app.js') },
  { id: 'hid', title: 'BadUSB', icon: 'usb', caps: ['usb.hid'], load: () => import('../apps/hid/app.js') },
  { id: 'wifitools', title: 'Net Tools', icon: 'globe', caps: ['wifi.tools'], load: () => import('../apps/wifitools/app.js') },
  { id: 'ota', title: 'Update', icon: 'cpu', caps: ['ota'], load: () => import('../apps/ota/app.js') },
  { id: 'settings', title: 'Settings', icon: 'settings', caps: ['settings'], load: () => import('../apps/settings/app.js') },
];
const allowed = (app) => (app.caps || []).every((c) => state.caps.includes(c)) &&
  (!app.capsAny || app.capsAny.some((c) => state.caps.includes(c)));

const root = document.getElementById('app');

// ---------------------------------------------------------------- theme: the desktop follows the watch's colours

const THEME_KEY = 'twatch.theme';
export function applyTheme(t) {
  if (!t || !/^#[0-9a-f]{6}$/i.test(t.accent || '')) return;
  const css = document.documentElement.style;
  css.setProperty('--accent', t.accent);
  if (/^#[0-9a-f]{6}$/i.test(t.accent_dim || '')) css.setProperty('--accent-dim', t.accent_dim);
  if (/^#[0-9a-f]{6}$/i.test(t.card || '')) css.setProperty('--card', t.card);
  if (/^#[0-9a-f]{6}$/i.test(t.track || '')) css.setProperty('--line', t.track);
  // Text on the accent: black or white, whichever contrasts more (WCAG relative luminance).
  const n = parseInt(t.accent.slice(1), 16);
  const lin = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; };
  const L = 0.2126 * lin((n >> 16) & 255) + 0.7152 * lin((n >> 8) & 255) + 0.0722 * lin(n & 255);
  css.setProperty('--on-accent', (L + 0.05) / 0.05 >= 1.05 / (L + 0.05) ? '#0b0b0b' : '#ffffff');
  document.documentElement.dataset.theme = t.name || '';
  try { localStorage.setItem(THEME_KEY, JSON.stringify(t)); } catch {}
}
try { applyTheme(JSON.parse(localStorage.getItem(THEME_KEY) || 'null')); } catch {}
const state = { caps: [], device: null, deviceAt: 0, windows: new Map(), z: 10 };

// ---------------------------------------------------------------- boot

async function boot() {
  try {
    api.device().then((d) => applyTheme(d.theme)).catch(() => {});
    const caps = await api.caps();
    state.caps = caps.caps || [];
    const auth = await api.auth();
    if (auth.paired) startDesktop();
    else showPairing();
  } catch (e) {
    root.replaceChildren(h('div', { class: 'pair' }, h('div', { class: 'pair-card' },
      h('img', { src: 'icons/watch.svg', alt: '' }),
      h('h1', {}, 'Watch not reachable'),
      h('p', {}, 'Check that the watch is on the same Wi-Fi and its web server is on.'),
      h('button', { class: 'btn primary', onclick: boot }, 'Try again'))));
  }
}

onUnpaired(() => {
  if (!document.querySelector('.shell')) return;
  events.stop();
  for (const w of state.windows.values()) w.close(true);
  showPairing('Your session ended. Pair again with the PIN on the watch.');
});

// ---------------------------------------------------------------- pairing

function showPairing(message) {
  const boxes = Array.from({ length: 6 }, (_, i) => h('input', {
    inputmode: 'numeric', maxlength: '1', autocomplete: i ? 'off' : 'one-time-code', 'aria-label': `Digit ${i + 1}`,
  }));
  const err = h('div', { class: 'pair-error' }, message || '');
  const go = h('button', { class: 'btn primary', disabled: true }, 'Pair');
  const pin = () => boxes.map((b) => b.value).join('');
  let lockTimer = null;

  const update = () => { go.disabled = pin().length !== 6; };
  boxes.forEach((b, i) => {
    b.addEventListener('input', () => {
      b.value = b.value.replace(/\D/g, '').slice(-1);
      if (b.value && i < 5) boxes[i + 1].focus();
      update();
      if (pin().length === 6) submit();
    });
    b.addEventListener('keydown', (e) => {
      if (e.key === 'Backspace' && !b.value && i > 0) { boxes[i - 1].focus(); boxes[i - 1].value = ''; update(); }
      if (e.key === 'Enter') submit();
    });
    b.addEventListener('paste', (e) => {
      const d = (e.clipboardData.getData('text') || '').replace(/\D/g, '').slice(0, 6);
      if (!d) return;
      e.preventDefault();
      boxes.forEach((x, k) => { x.value = d[k] || ''; });
      update();
      if (d.length === 6) submit();
      else boxes[d.length].focus();
    });
  });

  async function submit() {
    if (go.disabled || pin().length !== 6) return;
    go.disabled = true;
    err.textContent = '';
    try {
      await api.pair(pin());
      startDesktop();
    } catch (e) {
      boxes.forEach((b) => { b.value = ''; });
      boxes[0].focus();
      if (e.code === 'locked') {
        let s = Math.ceil((e.data?.retry_ms || 30000) / 1000);
        boxes.forEach((b) => { b.disabled = true; });
        const tick = () => {
          err.textContent = `Too many tries. Wait ${s} s.`;
          if (s-- <= 0) { clearInterval(lockTimer); err.textContent = ''; boxes.forEach((b) => { b.disabled = false; }); boxes[0].focus(); }
        };
        tick();
        lockTimer = setInterval(tick, 1000);
      } else {
        err.textContent = e.code === 'bad_pin' ? 'Wrong PIN. Check the watch screen.'
          : e.code === 'pairing_closed' ? 'Pairing is closed. Open Settings › Web desktop on the watch.' : e.message;
      }
      update();
    }
  }
  go.addEventListener('click', submit);

  root.replaceChildren(h('div', { class: 'pair' }, h('div', { class: 'pair-card' },
    h('img', { src: 'icons/watch.svg', alt: '' }),
    h('h1', {}, 'Pair with your watch'),
    h('p', {}, 'On the watch, open Settings › Web desktop and enter the PIN shown there.'),
    h('div', { class: 'pin' }, boxes),
    err,
    go)));
  boxes[0].focus();
}

// ---------------------------------------------------------------- desktop

let barEls = null;

function startDesktop() {
  const desk = h('div', { class: 'desk' });
  const launcher = h('div', { class: 'launcher' });
  const linkDot = h('span', { class: 'link-dot', title: 'Connection to the watch' });
  const sdStat = h('span', { class: 'stat' });
  const netStat = h('span', { class: 'stat' });
  const batStat = h('span', { class: 'stat' });
  const clock = h('span', { class: 'stat clock' });
  const bar = h('header', { class: 'bar' },
    h('div', { class: 'brand', onclick: () => focusLauncher() }, h('img', { src: 'icons/watch.svg', alt: '' }), h('span', {}, 'T-Watch Ultra'), linkDot),
    h('div', { class: 'spacer' }), sdStat, netStat, batStat, clock);
  barEls = { linkDot, sdStat, netStat, batStat, clock, desk };

  for (const app of APPS) {
    if (!allowed(app)) continue;
    launcher.append(h('button', { class: 'tile', onclick: () => openApp(app.id) },
      h('span', { class: 'tile-icon' }, icon(app.icon)), h('span', {}, app.title)));
  }
  desk.append(launcher);
  root.replaceChildren(h('div', { class: 'shell' }, bar, desk));

  refreshDevice();
  events.start();
  events.onStatus(onLinkStatus);
  events.on('battery', (d) => { if (state.device) { state.device.battery = d; renderBar(); } });
  events.on('sd.changed', (d) => { if (state.device) { state.device.sd = { present: d.present, mounted: d.mounted }; renderBar(); } });
  events.on('net', (d) => { if (state.device) { state.device.net = { ...state.device.net, ...d }; renderBar(); } });
  events.on('hello', () => refreshDevice());
  events.on('theme', (t) => { applyTheme(t); if (state.device) state.device.theme = t; });
  setInterval(renderClock, 1000);
  setInterval(refreshDevice, 60000);

  const want = new URLSearchParams(location.hash.slice(1)).get('app');
  if (want) openApp(want);
}

function focusLauncher() {
  if (window.matchMedia('(max-width: 720px)').matches) for (const w of state.windows.values()) w.close();
}

async function refreshDevice() {
  try {
    state.device = await api.device();
    state.deviceAt = Date.now();
    applyTheme(state.device.theme);
    renderBar();
  } catch (e) {
    if (!(e instanceof ApiError) || e.status !== 401) console.warn(e);
  }
}

function renderBar() {
  const d = state.device;
  if (!d || !barEls) return;
  const { sdStat, netStat, batStat } = barEls;
  sdStat.replaceChildren(icon('sd'), h('span', { class: 'label' }, d.sd?.mounted ? 'SD' : d.sd?.present ? 'SD not mounted' : 'No SD'));
  sdStat.classList.toggle('warn', !d.sd?.mounted);
  sdStat.title = d.sd?.mounted ? 'SD card mounted' : d.sd?.present ? 'SD card inserted, not mounted' : 'No SD card';
  const net = d.net || {};
  netStat.replaceChildren(icon(net.mode === 'off' ? 'wifiOff' : 'wifi'), h('span', { class: 'label' }, net.ssid || net.mode || ''));
  netStat.title = net.ip ? `${net.ssid || ''} ${net.ip}${net.rssi ? ` (${net.rssi} dBm)` : ''}` : '';
  const b = d.battery || {};
  batStat.replaceChildren(icon(b.charging ? 'charging' : 'battery'), h('span', {}, b.percent >= 0 ? `${b.percent}%` : ''));
  batStat.classList.toggle('warn', b.percent >= 0 && b.percent <= 15 && !b.charging);
  batStat.title = b.mv ? `${(b.mv / 1000).toFixed(2)} V${b.charging ? ', charging' : ''}` : '';
  renderClock();
}

// The watch's own clock, advanced locally between refreshes.
function renderClock() {
  const t = state.device?.time;
  if (!t || !barEls) return;
  const epoch = t.epoch + (Date.now() - state.deviceAt) / 1000;
  const local = new Date((epoch + (t.offset_min || 0) * 60) * 1000);
  barEls.clock.textContent = local.toISOString().slice(11, 16);
  barEls.clock.title = t.synced ? 'Time on the watch' : 'The watch clock is not set (Settings › Time)';
  barEls.clock.classList.toggle('warn', !t.synced);
}

let linkBanner = null;
let closedSince = 0;
function onLinkStatus(s) {
  if (!barEls) return;
  barEls.linkDot.className = `link-dot ${s === 'open' ? 'on' : s === 'connecting' ? '' : 'off'}`;
  if (linkBanner) { linkBanner.remove(); linkBanner = null; }
  if (s === 'seat-lost') {
    linkBanner = h('div', { class: 'banner' }, h('span', {}, 'The desktop is open in another tab or browser.'),
      h('button', { class: 'btn primary', onclick: () => events.reconnect() }, 'Use here'));
    barEls.desk.append(linkBanner);
  } else if (s === 'closed') {
    closedSince = closedSince || Date.now();
    setTimeout(() => {
      if (events.status === 'closed' && Date.now() - closedSince >= 3000 && !linkBanner && barEls) {
        linkBanner = h('div', { class: 'banner' }, h('span', {}, 'Reconnecting to the watch…'));
        barEls.desk.append(linkBanner);
      }
    }, 3100);
  } else if (s === 'open') {
    closedSince = 0;
  }
}

// ---------------------------------------------------------------- windows

function setHash(id) {
  const url = id ? `#app=${id}` : location.pathname + location.search;
  history.replaceState(null, '', url);
}

export async function openApp(id) {
  const app = APPS.find((a) => a.id === id);
  if (!app || !allowed(app)) return;
  if (state.windows.has(id)) { state.windows.get(id).focus(); return; }

  const title = h('span', { class: 't-text' }, app.title);
  const body = h('div', { class: 'win-body' });
  const win = h('section', { class: 'win', 'aria-label': app.title });
  const n = state.windows.size;
  const deskRect = barEls.desk.getBoundingClientRect();
  const w = Math.max(320, Math.min(820, deskRect.width - 40)), hgt = Math.max(260, Math.min(560, deskRect.height - 40));
  Object.assign(win.style, { left: `${20 + n * 28}px`, top: `${20 + n * 28}px`, width: `${w}px`, height: `${hgt}px` });

  let mounted = null;
  const handle = {
    focus() { win.style.zIndex = ++state.z; for (const x of state.windows.values()) x.el.classList.remove('focused'); win.classList.add('focused'); setHash(id); },
    close(silent) {
      try { mounted?.unmount?.(); } catch (e) { console.error(e); }
      win.remove();
      state.windows.delete(id);
      if (!silent) setHash(null);
    },
    el: win,
  };
  const bar = h('div', { class: 'win-title' },
    h('button', { class: 'btn icon ghost back', 'aria-label': 'Back', onclick: () => handle.close() }, icon('back')),
    h('span', { class: 't-icon' }, icon(app.icon)), title,
    h('button', { class: 'btn icon ghost close', 'aria-label': `Close ${app.title}`, onclick: () => handle.close() }, icon('close')));
  win.append(bar, body);
  win.addEventListener('pointerdown', () => handle.focus());
  dragWindow(win, bar);
  barEls.desk.append(win);
  state.windows.set(id, handle);
  handle.focus();

  try {
    const mod = await app.load();
    mounted = await mod.mount(body, {
      api, events, caps: state.caps,
      device: () => state.device,
      refreshDevice,
      openApp,
      setTitle: (t) => { title.textContent = t ? `${app.title} — ${t}` : app.title; },
    });
  } catch (e) {
    console.error(e);
    body.replaceChildren(h('div', { class: 'empty' }, `Could not open ${app.title}: ${e.message}`));
  }
}

function dragWindow(win, handle) {
  let sx, sy, ox, oy, dragging = false;
  handle.addEventListener('pointerdown', (e) => {
    if (e.target.closest('button') || window.matchMedia('(max-width: 720px)').matches) return;
    dragging = true;
    sx = e.clientX; sy = e.clientY; ox = win.offsetLeft; oy = win.offsetTop;
    handle.setPointerCapture(e.pointerId);
  });
  handle.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const desk = win.parentElement.getBoundingClientRect();
    const x = Math.max(-win.offsetWidth + 80, Math.min(desk.width - 80, ox + e.clientX - sx));
    const y = Math.max(0, Math.min(desk.height - 44, oy + e.clientY - sy));
    win.style.left = `${x}px`;
    win.style.top = `${y}px`;
  });
  handle.addEventListener('pointerup', () => { dragging = false; });
  handle.addEventListener('pointercancel', () => { dragging = false; });
}

window.addEventListener('unhandledrejection', (e) => {
  if (e.reason instanceof ApiError && e.reason.status === 401) return;
  toast(e.reason?.message || String(e.reason), 'error');
});

boot();
