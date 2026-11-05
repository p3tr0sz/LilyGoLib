// Net Tools: the watch's own connection, DNS and TCP reachability, and a 2.4 GHz channel overview.
// Read-only diagnostics — it looks at the air and reaches out from the watch, nothing more.
import { h, icon, errorText, notice, toast } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true;

  const info = h('div', { class: 'tool' });
  const host = h('input', { class: 'field', placeholder: 'example.com', 'aria-label': 'Host' });
  const port = h('input', { class: 'field w3', type: 'number', value: '443', min: '1', max: '65535', 'aria-label': 'Port' });
  const dnsBtn = h('button', { class: 'btn', onclick: resolve }, 'Resolve');
  const tcpBtn = h('button', { class: 'btn', onclick: tcp }, 'Connect');
  const diagOut = h('div', { class: 'tool' });
  const chans = h('div', { class: 'chart' });
  const chanInfo = h('span', { class: 'muted' });

  el.append(h('div', { class: 'app wifitools' },
    h('div', { class: 'panel' }, h('h3', {}, 'Connection'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Reach a host'),
      h('div', { class: 'toolbar' }, host, port, dnsBtn, tcpBtn),
      diagOut),
    h('div', { class: 'panel' }, h('h3', {}, '2.4 GHz channels', ' ', chanInfo), chans)));

  async function loadInfo() {
    try {
      const n = await api.net();
      if (!alive) return;
      const row = (k, v) => h('div', { class: 'srow' }, h('span', { class: 'muted' }, k), h('span', { class: 'mono' }, v || '—'));
      info.replaceChildren(
        n.connected
          ? h('div', {}, row('Network', n.ssid), row('IP', n.ip), row('Gateway', n.gateway), row('Netmask', n.netmask),
            row('DNS', n.dns), row('Signal', `${n.rssi} dBm`), row('Channel', String(n.channel)), row('MAC', n.mac))
          : h('div', { class: 'empty' }, 'The watch is not on a Wi-Fi network.'));
    } catch (e) { if (alive) info.replaceChildren(notice(errorText(e, 'Net Tools'))); }
  }

  const result = (text, ok) => diagOut.replaceChildren(h('div', { class: `srow ${ok ? 'ok' : 'error'}` }, text));

  async function resolve() {
    const hv = host.value.trim();
    if (!hv) return;
    dnsBtn.disabled = true;
    try { const r = await api.netResolve(hv); result(r.ok ? `${hv} → ${r.ip}  (${r.ms} ms)` : `${hv}: no answer  (${r.ms} ms)`, r.ok); }
    catch (e) { result(errorText(e, 'DNS'), false); } finally { dnsBtn.disabled = false; }
  }

  async function tcp() {
    const hv = host.value.trim(), pv = Number(port.value);
    if (!hv || !(pv >= 1 && pv <= 65535)) { toast('Enter a host and a port 1–65535', 'error'); return; }
    tcpBtn.disabled = true;
    try { const r = await api.netTcp(hv, pv); result(r.ok ? `TCP ${hv}:${pv} open  (${r.ms} ms)` : `TCP ${hv}:${pv} refused  (${r.ms} ms)`, r.ok); }
    catch (e) { result(errorText(e, 'TCP'), false); } finally { tcpBtn.disabled = false; }
  }

  async function loadChannels() {
    try {
      const c = await api.netChannels();
      if (!alive) return;
      chanInfo.textContent = `best: ${c.best}`;
      const max = Math.max(1, ...c.channels.map((x) => x.score));
      chans.replaceChildren(h('div', { class: 'chans' }, c.channels.map((x) => h('div', { class: `chanbar${x.channel === c.best ? ' best' : ''}`, title: `Channel ${x.channel}: ${x.count} AP(s), load ${x.score}` },
        h('i', { style: { height: `${Math.round(8 + x.score / max * 92)}%` } }),
        h('span', { class: 'sub' }, String(x.channel))))));
    } catch (e) { if (alive) chans.replaceChildren(notice(errorText(e, 'the analyzer'))); }
  }

  loadInfo();
  loadChannels();
  return { unmount() { alive = false; } };
}
