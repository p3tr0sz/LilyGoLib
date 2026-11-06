// Presence: ESP-NOW nearby peer discovery.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, peers: [] };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start Discovery');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const peerList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app presence' },
    h('div', { class: 'panel' },
      h('h4', {}, 'ESP-NOW Peer Discovery'),
      h('p', { class: 'muted' }, 'Discover nearby watches and ESP32 devices using ESP-NOW broadcast.'),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Nearby Peers'), peerList),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'DISCOVERING')),
          h('div', { class: 'srow' }, h('span', {}, 'Peers'), h('span', {}, String(state.peers ? state.peers.length : 0))))
      : h('div', { class: 'empty' }, 'ESP-NOW peer discovery. Find nearby T-Watch devices.'));
    peerList.replaceChildren(
      state.peers && state.peers.length
        ? state.peers.map(p => h('div', { class: 'note' },
            h('b', {}, p.name || 'Unknown'), ' · ', p.mac, ' · ', p.rssi, 'dBm'))
        : h('p', { class: 'muted' }, 'No peers discovered.'));
  }

  async function start() {
    try { await api.presenceStart(); toast('Discovery started'); } catch (e) { toast(errorText(e, 'Presence'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.presenceStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.presence(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 3000);
  return { unmount() { alive = false; clearInterval(timer); } };
}