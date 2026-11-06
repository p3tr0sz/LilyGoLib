// Wi-Fi Sniffer: passive 802.11 capture → .pcap file on SD card.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false };

  const chInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Ch', min: '-1', max: '13', value: '-1', 'aria-label': 'Channel (-1=hop all)' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start Capture');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });

  el.append(h('div', { class: 'app sniffer' },
    h('div', { class: 'panel' },
      h('h4', {}, 'Capture Settings'),
      h('div', { class: 'row gap' },
        h('label', { class: 'sub' }, 'Channel (-1 = hop all)'), chInput),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'CAPTURING')),
          h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, state.channel > 0 ? String(state.channel) : 'hopping')),
          h('div', { class: 'srow' }, h('span', {}, 'Packets'), h('span', {}, String(state.packets || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Bytes'), h('span', {}, String(state.bytes || 0))),
          state.file ? h('div', { class: 'srow' }, h('span', {}, 'File'), h('span', { class: 'mono' }, state.file)) : null)
      : h('div', { class: 'empty' }, 'Passive Wi-Fi capture to .pcap. Open in Wireshark.'));
  }

  async function start() {
    try {
      state = await api.snifferStart({ channel: Number(chInput.value) || -1 });
      toast('Capture started');
    } catch (e) { toast(errorText(e, 'Sniffer'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.snifferStop(); toast('Capture stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.sniffer(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 3000);
  return { unmount() { alive = false; clearInterval(timer); } };
}