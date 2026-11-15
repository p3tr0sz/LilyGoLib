// Wardrive: catalogue the Wi-Fi access points around the watch and tag each with its GNSS fix,
// then export the list as a WiGLE-1.6 CSV for upload. The watch scans and keeps the catalogue; the
// browser sets the cadence and shows the map-ready table. Passive — it only logs what the Wi-Fi
// scan and the GPS already see, and never transmits.
import { h, icon, toast, errorText, fmtTime, confirmDialog } from '../../js/ui.js';

const pct = (rssi) => Math.max(0, Math.min(100, Math.round((rssi + 95) * 100 / 60)));
const signal = (rssi) => h('span', { class: 'sig' }, h('span', { class: 'bar-meter' }, h('i', { style: { width: `${pct(rssi)}%` } })), `${rssi} dBm`);
const coord = (o) => o.has_fix ? `${o.lat.toFixed(5)}, ${o.lon.toFixed(5)}` : '—';

export function mount(el, ctx) {
  const { api, events, setTitle } = ctx;
  setTitle?.('');
  let alive = true, st = null, scanTimer = null, fbTimer = null, auto = false, busy = false;

  const btnStart = h('button', { class: 'btn primary' }, icon('play'), h('span', {}, 'Start'));
  const btnScan = h('button', { class: 'btn' }, icon('refresh'), 'Scan now');
  const autoBox = h('input', { type: 'checkbox', 'aria-label': 'Scan automatically' });
  const everySel = h('select', { class: 'field', style: { width: 'auto' }, 'aria-label': 'Scan interval' },
    [3, 5, 10, 15].map((s) => h('option', { value: s, selected: s === 5 }, `every ${s}s`)));
  const btnExport = h('button', { class: 'btn' }, icon('save'), 'Export CSV');
  const btnClear = h('button', { class: 'btn ghost' }, icon('trash'), 'Clear');
  const status = h('span', { class: 'muted wd-status' });
  const body = h('div', { class: 'tool' });

  const running = () => !!st?.running;

  function renderToolbar() {
    btnStart.replaceChildren(icon(running() ? 'stop' : 'play'), h('span', {}, running() ? 'Stop' : 'Start'));
    btnStart.classList.toggle('primary', !running());
    btnScan.disabled = !running() || busy;
    btnExport.disabled = !st || !st.count;
    btnClear.disabled = !st || !st.count || busy;
    everySel.disabled = !running();
    autoBox.disabled = !running();
    const bits = [];
    if (st) {
      bits.push(`${st.count} network${st.count === 1 ? '' : 's'}`);
      bits.push(`${st.with_fix} with a fix`);
      if (st.scanning) bits.push('scanning…');
      else if (st.ts) bits.push(`last scan ${fmtTime(st.ts)}`);
    }
    status.textContent = bits.join(' · ');
  }

  function renderBody() {
    if (!st) { body.replaceChildren(h('div', { class: 'empty' }, 'Loading…')); return; }
    if (!running() && !st.count) {
      body.replaceChildren(h('div', { class: 'empty' },
        h('p', {}, 'Start a session, then scan as you move. Each scan records the access points in range with the current GPS position.'),
        h('p', { class: 'muted' }, 'Export writes a WiGLE-1.6 CSV to /sd/wardrive for upload. Networks without a GPS fix are kept here but left out of the CSV.')));
      return;
    }
    const items = [...(st.networks || [])].sort((a, b) => b.rssi - a.rssi);
    if (!items.length) { body.replaceChildren(h('div', { class: 'empty' }, st.scanning ? 'Scanning…' : 'No access points logged yet.')); return; }
    body.replaceChildren(h('table', { class: 'tbl wd-list' },
      h('thead', {}, h('tr', {},
        ['Network', 'Security', 'Ch', 'Signal', 'Seen', 'Position'].map((t) => h('th', {}, t)))),
      h('tbody', {}, items.map((n) => h('tr', {},
        h('td', {}, n.ssid || h('span', { class: 'muted' }, '(hidden)'), h('span', { class: 'sub' }, n.bssid)),
        h('td', {}, n.auth),
        h('td', { class: 'num' }, n.channel),
        h('td', { class: 'num' }, signal(n.rssi)),
        h('td', { class: 'num' }, String(n.count)),
        h('td', { class: n.has_fix ? 'wd-fix' : 'muted' }, coord(n))))),
      st.truncated ? h('tfoot', {}, h('tr', {}, h('td', { colspan: 6, class: 'muted' }, `Showing the first ${items.length}. Export writes them all.`))) : null));
  }

  function render() { renderToolbar(); renderBody(); }

  async function refresh() {
    try { st = await api.get('/api/wardrive'); if (alive) render(); }
    catch (e) { if (alive) body.replaceChildren(h('div', { class: 'empty notice' }, h('p', {}, errorText(e)))); }
  }

  async function scanOnce() {
    if (!running() || busy) return;
    busy = true; renderToolbar();
    try { await api.post('/api/wardrive/scan', {}); } catch (e) { busy = false; toast(errorText(e), 'error'); return; }
    // The scan runs on the watch and publishes 'wardrive' when it lands (handled below). Refresh
    // anyway after a grace period in case that event is missed, so the table never stalls.
    clearTimeout(fbTimer);
    fbTimer = setTimeout(() => { if (busy) { busy = false; refresh(); } }, 2500);
  }

  function scheduleAuto() {
    clearTimeout(scanTimer);
    if (!alive || !auto || !running()) return;
    scanTimer = setTimeout(async () => { await scanOnce(); scheduleAuto(); }, Number(everySel.value) * 1000);
  }

  btnStart.onclick = async () => {
    try {
      if (running()) { st = await api.post('/api/wardrive/stop', {}); clearTimeout(scanTimer); }
      else { st = await api.post('/api/wardrive/start', {}); if (auto) { await scanOnce(); scheduleAuto(); } }
      render();
    } catch (e) { toast(errorText(e), 'error'); }
  };
  btnScan.onclick = scanOnce;
  autoBox.onchange = () => { auto = autoBox.checked; if (auto && running()) { scanOnce(); } scheduleAuto(); };
  everySel.onchange = scheduleAuto;
  btnClear.onclick = async () => {
    if (!await confirmDialog('Clear the catalogue?', 'The logged access points are discarded. Export first if you want to keep them.', 'Clear', true)) return;
    try { st = await api.post('/api/wardrive/clear', {}); render(); } catch (e) { toast(errorText(e), 'error'); }
  };
  btnExport.onclick = async () => {
    btnExport.disabled = true;
    try {
      const r = await api.post('/api/wardrive/export', {});
      toast(`Saved ${r.rows} network${r.rows === 1 ? '' : 's'} to ${r.path}${r.skipped ? ` · ${r.skipped} without a fix skipped` : ''}`);
    } catch (e) { toast(errorText(e), 'error'); }
    finally { renderToolbar(); }
  };

  el.append(h('div', { class: 'app wardrive' },
    h('div', { class: 'toolbar' }, btnStart, btnScan,
      h('label', { class: 'wd-auto' }, autoBox, h('span', {}, 'Auto'), everySel),
      btnExport, btnClear, status),
    body));

  // The watch publishes 'wardrive' when a scan completes — pull the full catalogue (the event
  // carries only the counts, not the network list) and clear the in-flight flag.
  const off = events.on('wardrive', () => { if (!alive) return; busy = false; clearTimeout(fbTimer); refresh(); });
  refresh();
  return { unmount() { alive = false; clearTimeout(scanTimer); clearTimeout(fbTimer); off(); } };
}
