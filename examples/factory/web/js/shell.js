// The desktop shell: pairing, status bar, launcher and windows.
import { api, events, onUnpaired, ApiError } from './api.js';
import { h, icon, toast } from './ui.js';
import { searchFiles, searchSettings } from './search.js';

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
  { id: 'sniffer', title: 'Wi-Fi Sniffer', icon: 'radar', caps: ['wifi.sniffer'], load: () => import('../apps/sniffer/app.js') },
  { id: 'airspace', title: 'Airspace', icon: 'shield', caps: ['airspace'], load: () => import('../apps/airspace/app.js') },
  { id: 'sentinel', title: 'Sentinel', icon: 'bluetooth', caps: ['sentinel'], load: () => import('../apps/sentinel/app.js') },
  { id: 'presence', title: 'Presence', icon: 'radar', capsAny: ['presence.csi', 'presence.peers'], load: () => import('../apps/presence/app.js') },
  { id: 'fido2', title: 'FIDO2 Key', icon: 'key', caps: ['fido2'], load: () => import('../apps/fido2/app.js') },
  { id: 'evilportal', title: 'Evil Portal', icon: 'wifi', caps: ['evil.portal'], load: () => import('../apps/evilportal/app.js') },
  { id: 'deauthflood', title: 'Deauth Flood', icon: 'wifiOff', caps: ['deauth.flood'], load: () => import('../apps/deauthflood/app.js') },
  { id: 'beaconspam', title: 'Beacon Spam', icon: 'wifi', caps: ['beacon.spam'], load: () => import('../apps/beaconspam/app.js') },
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
// Teardown callbacks for everything startDesktop() wires up (event subs, intervals, document
// listeners), so a re-pair can re-enter startDesktop() without leaking a second copy of each.
const teardown = [];

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
  for (const off of teardown.splice(0)) { try { off(); } catch {} }
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
let launcherEl = null;       // the launchpad surface
let dockTasksEl = null;      // running-app buttons in the dock
let launchpadOpen = true;    // the launchpad is shown over the desktop
const notes = [];            // notification tray history
let trayPanel = null, sysMenu = null, trayBadge = null;
let lowBatteryNoted = false;
const isPhone = () => window.matchMedia('(max-width: 720px)').matches;

function startDesktop() {
  state.focusedId = null;
  const desk = h('div', { class: 'desk' });

  // Launchpad: global search over apps, settings and files on the watch.
  const search = h('input', {
    class: 'lp-search', type: 'search', placeholder: 'Search apps, files, settings…', 'aria-label': 'Global search',
    oninput: () => filterLaunchpad(search.value),
  });
  const grid = h('div', { class: 'lp-grid' });
  const results = h('div', { class: 'lp-results' });
  const noHits = h('div', { class: 'lp-empty muted hidden' }, 'No apps, files, or settings match.');
  const launcher = h('div', { class: 'launcher' }, h('div', { class: 'lp-head' }, icon('search'), search), grid, results, noHits);
  launcher._noHits = noHits;
  launcher._results = results;
  launcherEl = launcher;
  for (const app of APPS) {
    if (!allowed(app)) continue;
    grid.append(h('button', { class: 'tile', 'data-title': app.title.toLowerCase(), 'data-id': app.id, onclick: () => openApp(app.id) },
      h('span', { class: 'tile-icon' }, icon(app.icon)), h('span', {}, app.title)));
  }

  // Top bar: identity + link dot, live status (opens the system menu), notifications tray.
  const linkDot = h('span', { class: 'link-dot', title: 'Connection to the watch' });
  const sdStat = h('span', { class: 'stat' });
  const netStat = h('span', { class: 'stat' });
  const batStat = h('span', { class: 'stat' });
  const clock = h('span', { class: 'stat clock' });
  const statuses = h('button', { class: 'statuses', 'aria-label': 'System status', onclick: toggleSysMenu }, sdStat, netStat, batStat, clock);
  trayBadge = h('span', { class: 'tray-badge hidden' });
  const trayBtn = h('button', { class: 'btn icon ghost tray-btn', 'aria-label': 'Notifications', onclick: toggleTray }, icon('bell'), trayBadge);
  const bar = h('header', { class: 'bar' },
    h('div', { class: 'brand', onclick: toggleLaunchpad }, h('img', { src: 'icons/watch.svg', alt: '' }), h('span', {}, 'T-Watch Ultra'), linkDot),
    h('div', { class: 'spacer' }), statuses, trayBtn);

  // Dock: a launchpad button and one button per running app.
  const appsBtn = h('button', { class: 'dock-apps', 'aria-label': 'Apps', onclick: toggleLaunchpad }, icon('apps'), h('span', { class: 'lbl' }, 'Apps'));
  const tasks = h('div', { class: 'dock-tasks' });
  dockTasksEl = tasks;
  const dock = h('footer', { class: 'dock' }, appsBtn, tasks);

  barEls = { linkDot, sdStat, netStat, batStat, clock, desk, appsBtn };
  desk.append(launcher);
  root.replaceChildren(h('div', { class: 'shell' }, bar, desk, dock));
  updateLaunchpad();

  // Clear any wiring left over from a previous session before re-wiring (re-pair re-enters here).
  for (const off of teardown.splice(0)) { try { off(); } catch {} }

  refreshDevice();
  events.start();
  teardown.push(events.onStatus(onLinkStatus));
  teardown.push(events.on('battery', (d) => { if (state.device) { state.device.battery = d; renderBar(); } }));
  teardown.push(events.on('sd.changed', (d) => {
    if (state.device) { state.device.sd = { present: d.present, mounted: d.mounted }; renderBar(); }
    pushNote('SD card', d.present ? (d.mounted ? 'Card mounted.' : 'Card inserted (not mounted).') : 'Card removed.');
  }));
  teardown.push(events.on('net', (d) => { if (state.device) { state.device.net = { ...state.device.net, ...d }; renderBar(); } }));
  teardown.push(events.on('notify', (d) => pushNote(d.title || 'Notification', d.text || d.body || '')));
  teardown.push(events.on('hello', () => refreshDevice()));
  teardown.push(events.on('theme', (t) => { applyTheme(t); if (state.device) state.device.theme = t; }));
  const clockTimer = setInterval(renderClock, 1000);
  const deviceTimer = setInterval(refreshDevice, 60000);
  teardown.push(() => clearInterval(clockTimer), () => clearInterval(deviceTimer));

  const onKeydown = (e) => { if (e.key === 'Escape') { closeSysMenu(); closeTray(); } };
  const onPointerdown = (e) => {
    if (sysMenu && !e.target.closest('.sysmenu') && !e.target.closest('.statuses')) closeSysMenu();
    if (trayPanel && !e.target.closest('.tray-panel') && !e.target.closest('.tray-btn')) closeTray();
  };
  document.addEventListener('keydown', onKeydown);
  document.addEventListener('pointerdown', onPointerdown);
  teardown.push(() => document.removeEventListener('keydown', onKeydown),
                () => document.removeEventListener('pointerdown', onPointerdown));

  const want = new URLSearchParams(location.hash.slice(1)).get('app');
  if (want) openApp(want);
}

// ---------------------------------------------------------------- launchpad

let searchTimer = null;
let searchGeneration = 0;
let searchAppHits = 0;
let searchSettingHits = [];
let searchFileHits = [];
let searchFilesPending = false;

function renderSearchResults(q) {
  if (!launcherEl) return;
  const sections = [];
  const section = (title, items, make) => {
    if (!items.length) return;
    sections.push(h('section', { class: 'lp-result-group' }, h('h2', {}, title), items.map(make)));
  };
  section('Settings', searchSettingHits, (s) => h('button', {
    class: 'lp-result', 'data-kind': 'setting', 'data-id': s.id,
    onclick: () => openApp('settings', { setting: s.id }),
  }, icon('settings'), h('span', { class: 'lp-result-text' }, h('b', {}, s.title), h('small', {}, s.section))));
  section('Files', searchFileHits, (f) => h('button', {
    class: 'lp-result', 'data-kind': 'file', 'data-path': f.path,
    onclick: () => openApp('files', { path: f.path, dir: f.dir, preview: !f.dir }),
  }, icon(f.dir ? 'folder' : 'file'), h('span', { class: 'lp-result-text' }, h('b', {}, f.name), h('small', {}, f.path))));
  if (searchFilesPending) sections.push(h('div', { class: 'lp-searching muted', role: 'status' }, 'Searching files…'));
  launcherEl._results.replaceChildren(...sections);
  const hits = searchAppHits + searchSettingHits.length + searchFileHits.length;
  launcherEl._noHits.classList.toggle('hidden', !q || hits > 0 || searchFilesPending);
}

function filterLaunchpad(value) {
  const q = (value || '').trim().toLowerCase();
  const generation = ++searchGeneration;
  clearTimeout(searchTimer);
  searchAppHits = 0;
  for (const t of launcherEl.querySelectorAll('.tile')) {
    const show = !q || t.dataset.title.includes(q) || t.dataset.id.includes(q);
    t.classList.toggle('hidden', !show);
    if (show) searchAppHits++;
  }
  searchSettingHits = q && state.caps.includes('settings') ? searchSettings(q) : [];
  searchFileHits = [];
  searchFilesPending = q.length >= 2 && state.caps.includes('files');
  renderSearchResults(q);
  if (!searchFilesPending) return;
  searchTimer = setTimeout(async () => {
    const found = await searchFiles(api, q, state.caps, () => generation === searchGeneration);
    if (generation !== searchGeneration) return;
    searchFileHits = found.results;
    searchFilesPending = false;
    renderSearchResults(q);
  }, 180);
}

function openVisibleCount() {
  let n = 0;
  for (const w of state.windows.values()) if (!w.minimized) n++;
  return n;
}

// The launchpad shows when the person asks for it, or whenever no window is up.
function updateLaunchpad() {
  if (!launcherEl) return;
  const show = launchpadOpen || openVisibleCount() === 0;
  launcherEl.classList.toggle('hidden', !show);
  barEls?.appsBtn?.classList.toggle('on', show);
  if (show && launchpadOpen && !isPhone()) { const s = launcherEl.querySelector('.lp-search'); s?.focus(); }
}

function toggleLaunchpad() {
  launchpadOpen = !launchpadOpen;
  if (launchpadOpen && isPhone()) for (const w of state.windows.values()) w.minimize();
  updateLaunchpad();
}

// ---------------------------------------------------------------- dock / taskbar

function renderTasks() {
  if (!dockTasksEl) return;
  dockTasksEl.replaceChildren();
  for (const [id, w] of state.windows) {
    const app = APPS.find((a) => a.id === id);
    const btn = h('button', {
      class: `dock-task${state.focusedId === id && !w.minimized ? ' active' : ''}${w.minimized ? ' min' : ''}`,
      title: app?.title || id,
      onclick: () => { if (w.minimized) w.restore(); else if (state.focusedId === id) w.minimize(); else w.focus(); },
    }, h('span', { class: 't-ic' }, icon(app?.icon || 'file')), h('span', { class: 'lbl' }, app?.title || id));
    dockTasksEl.append(btn);
  }
}

// ---------------------------------------------------------------- system menu

function closeSysMenu() { if (sysMenu) { sysMenu.remove(); sysMenu = null; } }
function toggleSysMenu() {
  if (sysMenu) { closeSysMenu(); return; }
  closeTray();
  const d = state.device || {};
  const b = d.battery || {}, net = d.net || {}, sd = d.sd || {}, t = d.time || {};
  const row = (label, val) => h('div', { class: 'sm-row' }, h('span', {}, label), h('b', {}, val || '—'));
  sysMenu = h('div', { class: 'popover sysmenu' },
    h('div', { class: 'sm-grid' },
      row('Battery', b.percent >= 0 ? `${b.percent}%${b.charging ? ' ⚡' : ''}${b.mv ? ` · ${(b.mv / 1000).toFixed(2)} V` : ''}` : ''),
      row('Network', net.mode === 'off' ? 'Off' : `${net.ssid || net.mode || ''}${net.ip ? ` · ${net.ip}` : ''}${net.rssi ? ` · ${net.rssi} dBm` : ''}`),
      row('SD card', sd.mounted ? 'Mounted' : sd.present ? 'Inserted, not mounted' : 'None'),
      row('Time', t.epoch ? (t.synced ? 'Synced' : 'Not set') : '')),
    state.caps.includes('settings')
      ? h('button', { class: 'btn', onclick: () => { closeSysMenu(); openApp('settings'); } }, icon('settings'), 'Settings')
      : null);
  document.querySelector('.shell').append(sysMenu);
}

// ---------------------------------------------------------------- notifications tray

function closeTray() { if (trayPanel) { trayPanel.remove(); trayPanel = null; } }
function buildTray() {
  const list = notes.length
    ? notes.map((n) => h('div', { class: 'note' }, h('div', {}, h('b', {}, n.title), n.text ? h('p', {}, n.text) : null), h('span', { class: 'when' }, n.when)))
    : [h('div', { class: 'empty' }, 'No notifications.')];
  return h('div', { class: 'popover tray-panel' },
    h('div', { class: 'tray-head' }, h('b', {}, 'Notifications'),
      notes.length ? h('button', { class: 'btn ghost', onclick: () => { notes.length = 0; updateTrayBadge(); closeTray(); } }, 'Clear') : null),
    h('div', { class: 'tray-list' }, ...list));
}
function toggleTray() {
  if (trayPanel) { closeTray(); return; }
  closeSysMenu();
  for (const n of notes) n.read = true;
  updateTrayBadge();
  trayPanel = buildTray();
  document.querySelector('.shell').append(trayPanel);
}
function updateTrayBadge() {
  if (!trayBadge) return;
  const unread = notes.filter((n) => !n.read).length;
  trayBadge.textContent = unread > 9 ? '9+' : String(unread);
  trayBadge.classList.toggle('hidden', unread === 0);
}
function pushNote(title, text, kind = 'info') {
  notes.unshift({ title, text, kind, read: !!trayPanel, when: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) });
  if (notes.length > 50) notes.length = 50;
  updateTrayBadge();
  if (trayPanel) { const fresh = buildTray(); trayPanel.replaceWith(fresh); trayPanel = fresh; }
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
  const low = b.percent >= 0 && b.percent <= 15 && !b.charging;
  batStat.classList.toggle('warn', low);
  batStat.title = b.mv ? `${(b.mv / 1000).toFixed(2)} V${b.charging ? ', charging' : ''}` : '';
  if (low && !lowBatteryNoted) { lowBatteryNoted = true; pushNote('Battery low', `${b.percent}% remaining.`, 'warn'); }
  else if (b.charging || b.percent > 20) lowBatteryNoted = false;
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
    pushNote('Desktop moved', 'The desktop was opened in another tab or browser.', 'warn');
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

export async function openApp(id, options = {}) {
  const app = APPS.find((a) => a.id === id);
  if (!app || !allowed(app)) return;
  // Opening a result ends the query; do not keep walking folders behind the app.
  searchGeneration++;
  clearTimeout(searchTimer);
  searchFilesPending = false;
  renderSearchResults(launcherEl?.querySelector('.lp-search')?.value.trim().toLowerCase() || '');
  if (state.windows.has(id)) {
    const existing = state.windows.get(id);
    existing.restore();
    existing.navigate(options);
    return;
  }

  const title = h('span', { class: 't-text' }, app.title);
  const body = h('div', { class: 'win-body' });
  const win = h('section', { class: 'win', 'aria-label': app.title });
  const n = state.windows.size;
  const deskRect = barEls.desk.getBoundingClientRect();
  const w = Math.max(320, Math.min(820, deskRect.width - 40)), hgt = Math.max(260, Math.min(560, deskRect.height - 40));
  Object.assign(win.style, { left: `${20 + n * 28}px`, top: `${20 + n * 28}px`, width: `${w}px`, height: `${hgt}px` });

  let mounted = null;
  let pendingOptions = Object.keys(options).length ? options : null;
  const handle = {
    minimized: false,
    el: win,
    navigate(next) {
      if (!next || !Object.keys(next).length) return;
      if (mounted?.open) mounted.open(next);
      else pendingOptions = next;
    },
    focus() {
      this.minimized = false; win.classList.remove('min');
      win.style.zIndex = ++state.z;
      for (const x of state.windows.values()) x.el.classList.remove('focused');
      win.classList.add('focused');
      state.focusedId = id;
      launchpadOpen = false;
      setHash(id); renderTasks(); updateLaunchpad();
    },
    restore() { win.classList.remove('min'); this.minimized = false; this.focus(); },
    minimize() {
      this.minimized = true; win.classList.add('min'); win.classList.remove('focused');
      if (state.focusedId === id) state.focusedId = null;
      renderTasks(); updateLaunchpad();
    },
    toggleMax() { win.classList.toggle('max'); this.focus(); },
    close(silent) {
      try { mounted?.unmount?.(); } catch (e) { console.error(e); }
      win.remove();
      state.windows.delete(id);
      if (state.focusedId === id) state.focusedId = null;
      if (!silent) setHash(null);
      renderTasks(); updateLaunchpad();
    },
  };
  const bar = h('div', { class: 'win-title' },
    h('button', { class: 'btn icon ghost back', 'aria-label': 'Back', onclick: () => handle.close() }, icon('back')),
    h('span', { class: 't-icon' }, icon(app.icon)), title,
    h('button', { class: 'btn icon ghost win-min', 'aria-label': 'Minimize', onclick: () => handle.minimize() }, icon('min')),
    h('button', { class: 'btn icon ghost win-max', 'aria-label': 'Maximize', onclick: () => handle.toggleMax() }, icon('max')),
    h('button', { class: 'btn icon ghost close', 'aria-label': `Close ${app.title}`, onclick: () => handle.close() }, icon('close')));
  win.append(bar, body);
  win.addEventListener('pointerdown', () => { if (state.focusedId !== id || handle.minimized) handle.focus(); });
  dragWindow(win, bar, handle);
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
      notify: (t, text, kind) => pushNote(t, text, kind),
      setTitle: (t) => { title.textContent = t ? `${app.title} — ${t}` : app.title; },
    });
    if (pendingOptions && mounted?.open) await mounted.open(pendingOptions);
    pendingOptions = null;
  } catch (e) {
    console.error(e);
    body.replaceChildren(h('div', { class: 'empty' }, `Could not open ${app.title}: ${e.message}`));
  }
}

function dragWindow(win, handle, win_handle) {
  let sx, sy, ox, oy, dragging = false;
  handle.addEventListener('pointerdown', (e) => {
    if (e.target.closest('button') || win.classList.contains('max') || window.matchMedia('(max-width: 720px)').matches) return;
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
  handle.addEventListener('dblclick', (e) => { if (!e.target.closest('button') && !isPhone()) win_handle.toggleMax(); });
}

window.addEventListener('unhandledrejection', (e) => {
  if (e.reason instanceof ApiError && e.reason.status === 401) return;
  toast(e.reason?.message || String(e.reason), 'error');
});

boot();
