// Passive Wi-Fi capture to a radiotap PCAP on the watch SD card.
import { h, icon, errorText } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

const FILTER_LABELS = {
  all: 'All frames',
  management: 'Management only',
  data: 'Data only',
  beacons: 'Beacons only',
  probes: 'Probe requests only',
  eapol: 'EAPOL / WPA handshake only',
};

export function mount(el, { api, events }) {
  let state = {
    active: false, channel: null, filter: 'all', packets: 0, drops: 0, bytes: 0,
    seen: 0, management: 0, control: 0, data: 0, beacons: 0, probes: 0,
    deauth: 0, eapol: 0, handshakes: 0, aps: [], error: null, file: null,
  };

  const channel = h('select', { class: 'field', 'aria-label': 'Capture channel' },
    h('option', { value: '-1' }, 'All channels (hop 1-13)'),
    ...Array.from({ length: 13 }, (_, i) => h('option', { value: String(i + 1) }, `Channel ${i + 1}`)));
  const filter = h('select', { class: 'field', 'aria-label': 'Capture filter' },
    ...Object.entries(FILTER_LABELS).map(([value, label]) => h('option', { value }, label)));
  const maxBytes = h('input', { class: 'field', type: 'number', min: '0', step: '1', value: '0', 'aria-label': 'Maximum capture bytes; zero means unlimited' });
  const startBtn = h('button', { class: 'btn primary', type: 'button', onclick: start }, icon('play'), 'Start capture');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.snifferStop(), 'Capture saved'), disabled: true }, icon('stop'), 'Stop and save');
  const info = h('div', { class: 'tool' });
  const apList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app sniffer' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Authorized packet capture'),
      h('p', { class: 'muted' }, 'Passive capture only. Frames can contain private data; capture only networks you may inspect.')),
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'Channel', channel),
        h('label', {}, 'Filter', filter),
        h('label', {}, 'Byte limit (0 = unlimited)', maxBytes)),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Capture status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Access points seen'), apList)));

  const countRow = (label, value, warning = false) =>
    h('div', { class: 'srow' }, h('span', {}, label), h('span', { class: warning ? 'tag alert' : '' }, String(value || 0)));

  function render() {
    startBtn.disabled = app.busy || state.active;
    stopBtn.disabled = app.busy || !state.active;
    channel.disabled = state.active;
    filter.disabled = state.active;
    const rows = [
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'ok' : ''}` }, state.active ? 'CAPTURING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, state.channel > 0 ? String(state.channel) : 'Hopping 1-13')),
      h('div', { class: 'srow' }, h('span', {}, 'Filter'), h('span', {}, FILTER_LABELS[state.filter] || state.filter || 'All frames')),
      countRow('Frames saved to PCAP', state.packets),
      countRow('Frames seen', state.seen),
      countRow('Dropped packets', state.drops, (state.drops || 0) > 0),
      countRow('Bytes written', state.bytes),
      countRow('Beacons', state.beacons),
      countRow('Probe requests', state.probes),
      countRow('Deauth / disassociation frames', state.deauth, (state.deauth || 0) > 0),
      countRow('EAPOL frames', state.eapol),
      countRow('Complete 4-way handshakes', state.handshakes),
    ];
    if (state.file) rows.push(h('div', { class: 'srow' }, h('span', {}, 'PCAP file'), h('a', { href: api.contentUrl(state.file), download: '' }, state.file)));
    if (state.error) rows.push(h('p', { class: 'inline-error', role: 'alert' }, state.error));
    info.replaceChildren(...rows);

    const aps = Array.isArray(state.aps) ? state.aps.slice(0, 8) : [];
    apList.replaceChildren(...(aps.length
      ? aps.map(ap => h('div', { class: 'note' },
        h('div', {}, h('b', {}, ap.ssid || '(hidden SSID)'), h('p', {}, `${ap.bssid} · channel ${ap.channel || '?'} · ${ap.rssi ?? '?'} dBm`)),
        h('span', { class: 'when' }, `${ap.beacons || 0} beacon${ap.beacons === 1 ? '' : 's'}`)))
      : [h('p', { class: 'muted' }, 'No access points seen yet.')]));
  }

  const app = toolApp({
    name: 'Sniffer',
    read: () => api.sniffer(),
    apply: (next) => { state = { ...state, ...next }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Sniffer'))),
    interval: 2000,
    pollWhen: () => state.active,
  });

  function start() {
    return app.action(() => api.snifferStart({ channel: Number(channel.value), filter: filter.value, max_bytes: Number(maxBytes.value) || 0 }), 'Capture started');
  }

  const off = events.on('sniffer', () => app.load(false));
  app.start();
  return { unmount: () => app.unmount(off) };
}
