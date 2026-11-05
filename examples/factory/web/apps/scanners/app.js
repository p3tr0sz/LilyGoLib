// Scanners: Wi-Fi networks and Bluetooth LE devices around the watch.
import { h, icon, errorText, fmtTime } from '../../js/ui.js';

const pct = (rssi) => Math.max(0, Math.min(100, Math.round((rssi + 95) * 100 / 60)));
const signal = (rssi) => h('span', { class: 'sig' }, h('span', { class: 'bar-meter' }, h('i', { style: { width: `${pct(rssi)}%` } })), `${rssi} dBm`);

const KINDS = {
  wifi: {
    cap: 'wifi.scan', label: 'Wi-Fi', topic: 'wifi.scan', list: 'networks',
    start: (api) => api.wifiScan(), result: (api) => api.wifiResult(),
    head: ['Network', 'Signal', 'Channel', 'Security'],
    row: (n) => [h('td', {}, n.ssid || h('span', { class: 'muted' }, '(hidden)'), h('span', { class: 'sub' }, n.bssid)),
      h('td', { class: 'num' }, signal(n.rssi)), h('td', { class: 'num' }, n.channel), h('td', {}, n.auth)],
  },
  ble: {
    cap: 'ble.scan', label: 'Bluetooth', topic: 'ble.scan', list: 'devices',
    start: (api, s) => api.bleScan(s), result: (api) => api.bleResult(),
    head: ['Device', 'Signal', 'Data'],
    row: (d) => [h('td', {}, d.name || h('span', { class: 'muted' }, '(no name)'), h('span', { class: 'sub' }, `${d.addr}${d.connectable ? ' · connectable' : ''}`)),
      h('td', { class: 'num' }, signal(d.rssi)),
      h('td', { style: { wordBreak: 'break-all' } }, (d.services || []).length ? h('div', {}, `Services: ${d.services.join(', ')}`) : null,
        d.mfr ? h('div', { class: 'sub' }, `Maker data: ${d.mfr.length > 40 ? `${d.mfr.slice(0, 40)}…` : d.mfr}`) : null)],
  },
};

export function mount(el, ctx) {
  const { api, events, caps } = ctx;
  const kinds = Object.entries(KINDS).filter(([, k]) => caps.includes(k.cap)).map(([id]) => id);
  let cur = kinds[0], alive = true, poll = null;
  const data = {}, scanning = {};
  const tabs = h('div', { class: 'seg' }, kinds.map((id) => h('button', { 'data-tab': id, onclick: () => show(id) }, KINDS[id].label)));
  const secs = h('select', { class: 'field', style: { width: 'auto' }, 'aria-label': 'Scan for' },
    [3, 5, 10, 15].map((s) => h('option', { value: s, selected: s === 5 }, `${s} s`)));
  const scanBtn = h('button', { class: 'btn primary', onclick: () => scan(cur) }, icon('refresh'), 'Scan');
  const status = h('span', { class: 'muted' });
  const body = h('div', { class: 'tool' });
  el.append(h('div', { class: 'app scanners' }, h('div', { class: 'toolbar' }, kinds.length > 1 ? tabs : null, scanBtn, secs, status), body));

  function render() {
    tabs.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b.dataset.tab === cur));
    secs.hidden = cur !== 'ble';
    const k = KINDS[cur], d = data[cur];
    scanBtn.disabled = !!scanning[cur];
    status.textContent = scanning[cur] ? 'Scanning…' : d?.ts ? `${(d[k.list] || []).length} found · ${fmtTime(d.ts)}` : '';
    if (d?.error) { body.replaceChildren(h('div', { class: 'empty notice' }, h('p', {}, d.error))); return; }
    const items = [...(d?.[k.list] || [])].sort((a, b) => b.rssi - a.rssi);
    if (!items.length) { body.replaceChildren(h('div', { class: 'empty' }, scanning[cur] || !d ? 'Scanning…' : 'Nothing found.')); return; }
    body.replaceChildren(h('table', { class: `tbl ${cur}-list` },
      h('thead', {}, h('tr', {}, k.head.map((t) => h('th', {}, t)))),
      h('tbody', {}, items.map((x) => h('tr', { class: 'scan-row' }, k.row(x))))));
  }

  function got(id, d) {
    data[id] = d;
    scanning[id] = !!d.scanning;
    if (id === cur) render();
  }

  async function scan(id) {
    scanning[id] = true;
    if (data[id]) delete data[id].error;
    render();
    try {
      await KINDS[id].start(api, Number(secs.value));
      // The event brings the result; poll as well in case it is missed.
      clearInterval(poll);
      poll = setInterval(async () => {
        if (!scanning[id]) { clearInterval(poll); return; }
        try { const r = await KINDS[id].result(api); if (!r.scanning) got(id, r); } catch {}
      }, 1500);
    } catch (e) {
      scanning[id] = false;
      data[id] = { ...(data[id] || {}), error: errorText(e, KINDS[id].label) };
      render();
    }
  }

  async function show(id) {
    cur = id;
    render();
    if (data[id]) return;
    try {
      const r = await KINDS[id].result(api);
      if (r.ts && !r.scanning) got(id, r); else if (r.scanning) { scanning[id] = true; render(); } else scan(id);
    } catch (e) { data[id] = { error: errorText(e, KINDS[id].label) }; render(); }
  }

  const offs = kinds.map((id) => events.on(KINDS[id].topic, (d) => { if (alive) got(id, d); }));
  if (cur) show(cur);
  return { unmount() { alive = false; clearInterval(poll); offs.forEach((f) => f()); } };
}
