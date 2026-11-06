// Wi-Fi Sniffer: passive promiscuous capture → .pcap on the SD
// Captures 802.11 packets and writes them to a pcap file for Wireshark analysis.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = { active: false, packets: 0, bytes: 0, file: null, channel: null };

  const chSel = h('select', { class: 'field', 'aria-label': 'Channel' },
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13].map((ch) =>
      h('option', { value: String(ch) }, ch === 0 ? 'All channels' : `CH ${ch}`)));
  const maxB = h('input', { class: 'field w4', type: 'number', value: '0', min: '0', placeholder: 'Max bytes', 'aria-label': 'Max bytes' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const log = h('div', { class: 'notes' });

  el.append(h('div', { class: 'app sniffer' },
    h('div', { class: 'toolbar' }, chSel, maxB, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Packets'), log)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(
      state.active
        ? h('div', {},
            h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'Capturing')),
            h('div', { class: 'srow' }, h('span', {}, 'Packets'), h('span', {}, String(state.packets))),
            h('div', { class: 'srow' }, h('span', {}, 'Bytes'), h('span', {}, state.bytes > 1024 ? `${Math.round(state.bytes/1024)} KB` : String(state.bytes))),
            state.file ? h('div', { class: 'srow' }, h('span', {}, 'File'), h('span', { class: 'mono' }, state.file)) : null,
            state.channel ? h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, state.channel > 0 ? String(state.channel) : 'All')) : null)
        : h('div', { class: 'empty' }, 'Configure and tap Start. Captures are saved as .pcap on the SD card.'));
    log.replaceChildren(state.packets > 0
      ? h('p', { class: 'muted' }, `${state.packets} packets captured, ${state.bytes} bytes written`)
      : h('p', { class: 'muted' }, 'No packets yet.'));
  }

  async function start() {
    try { await api.snifferStart({ channel: Number(chSel.value) || null, max_bytes: Number(maxB.value) || null }); toast('Capture started'); } catch (e) { toast(errorText(e, 'Sniffer'), 'error'); }
    load();
  }
  async function stop() {
    try { await api.snifferStop(); toast('Capture stopped'); } catch (e) { toast(errorText(e, 'Sniffer'), 'error'); }
    load();
  }
  async function load() {
    try {
      const s = await api.sniffer();
      if (!alive) return;
      state = s;
      render();
    } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }
  const off = events.on('sniffer', (d) => { if (alive) { Object.assign(state, d); render(); } });
  load();
  return { unmount() { alive = false; off(); } };
}