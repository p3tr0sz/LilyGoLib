// Sentinel: BLE anti-stalking — detect nearby tracking tags (passive).
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, tags: [] };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start Scan');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const tagList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app sentinel' },
    h('div', { class: 'panel' },
      h('h4', {}, 'BLE Anti-Stalking'),
      h('p', { class: 'muted' }, 'Passively scans for nearby Bluetooth tracking tags (AirTags, Tiles, etc.) without alerting the owner.'),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Detected Tags'), tagList),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'SCANNING')),
          h('div', { class: 'srow' }, h('span', {}, 'Tags found'), h('span', {}, String(state.detected || 0))))
      : h('div', { class: 'empty' }, 'BLE anti-stalking scan. Identifies nearby tracking devices.'));
    tagList.replaceChildren(
      state.tags && state.tags.length
        ? state.tags.map(t => h('div', { class: 'note' },
            h('b', {}, t.name || 'Unknown'), ' · ', t.addr, ' · ', t.rssi, 'dBm'))
        : h('p', { class: 'muted' }, state.active ? 'Scanning...' : 'No tags detected.'));
  }

  async function start() {
    try { await api.sentinelStart(); toast('BLE scan started'); } catch (e) { toast(errorText(e, 'Sentinel'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.sentinelStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.sentinel(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 3000);
  return { unmount() { alive = false; clearInterval(timer); } };
}