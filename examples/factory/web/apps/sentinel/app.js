// Sentinel: BLE anti-stalking. Fully passive — flags BLE devices that appear persistently.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = { active: false, device_count: 0, devices: [] };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Watch');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const devs = h('div', { class: 'notes' });

  el.append(h('div', { class: 'app sentinel' },
    h('div', { class: 'toolbar' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Devices'), devs)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'Watching')),
          h('div', { class: 'srow' }, h('span', {}, 'Devices tracked'), h('span', {}, String(state.device_count))))
      : h('div', { class: 'empty' }, 'Tap Watch to start BLE anti-stalking monitor.'));
    devs.replaceChildren((state.devices || []).length
      ? state.devices.map((d) => h('div', { class: `srow${d.stalking ? ' alert' : ''}` },
          d.stalking ? h('span', { class: 'tag alert' }, 'STALKING') : null,
          h('span', {}, d.name || d.addr),
          h('span', { class: 'num' }, `${d.rssi} dBm`),
          h('span', { class: 'num' }, `${d.sightings}x`)))
      : h('p', { class: 'muted' }, 'No devices seen yet.'));
  }

  async function start() { try { await api.sentinelStart(); toast('Sentinel active'); } catch (e) { toast(errorText(e), 'error'); } load(); }
  async function stop() { try { await api.sentinelStop(); toast('Sentinel stopped'); } catch (e) { toast(errorText(e), 'error'); } load(); }
  async function load() { try { const s = await api.sentinel(); if (!alive) return; state = s; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); } }
  const off = events.on('sentinel', (d) => { if (alive) load(); });
  load();
  return { unmount() { alive = false; off(); } };
}