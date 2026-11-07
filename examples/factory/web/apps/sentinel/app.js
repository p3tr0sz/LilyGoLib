// Sentinel: passive detection of persistent BLE location trackers.
import { h, icon, errorText } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

export function mount(el, { api, events, notify }) {
  let state = { active: false, scanning: false, device_count: 0, devices: [], error: null };
  const startBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => app.action(() => api.sentinelStart(), 'Passive BLE scan started') }, icon('play'), 'Start passive scan');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.sentinelStop(), 'BLE scan stopped'), disabled: true }, icon('stop'), 'Stop');
  const info = h('div', {});
  const tagList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app sentinel' },
    h('div', { class: 'panel' },
      h('h3', {}, 'BLE tracker safety check'),
      h('p', { class: 'muted' }, 'Recognizes Find My, Samsung SmartTag, Tile and Chipolo advertisements without sending scan requests. “Persistent” means nearby for several minutes; it does not prove stalking.')),
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Detected trackers'), tagList)));

  function render() {
    startBtn.disabled = app.busy || state.active;
    stopBtn.disabled = app.busy || !state.active;
    const persistent = (state.devices || []).filter(d => d.stalking).length;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'ok' : ''}` }, state.active ? (state.scanning ? 'LISTENING' : 'ACTIVE') : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Known trackers nearby'), h('span', {}, String(state.device_count || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Persistent trackers'), h('span', { class: persistent ? 'tag alert' : '' }, String(persistent))),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    tagList.replaceChildren(...((state.devices || []).length
      ? state.devices.map(d => h('div', { class: 'note' },
          h('div', {}, h('b', {}, d.kind || 'Tracker'),
            h('p', {}, `${d.stalking ? 'Persistent — check your belongings' : 'Nearby'} · ${d.sightings || 0} sightings${d.separated ? ' · separated/lost mode' : ''}`)),
          h('span', { class: 'when' }, `${d.rssi || 0} dBm`)))
      : [h('p', { class: 'muted' }, state.active ? 'No known trackers detected yet.' : 'Start a scan to check nearby trackers.')]));
  }

  const app = toolApp({
    name: 'Sentinel',
    read: () => api.sentinel(),
    apply: (next) => { state = { ...state, ...next }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Sentinel'))),
    interval: 3000,
    pollWhen: () => state.active,
  });

  const off = events.on('sentinel', d => {
    if (!app.alive) return;
    notify('Persistent tracker', d.detail || `${d.kind || 'Tracker'} remains nearby`, 'warn');
    app.load(false);
  });
  app.start();
  return { unmount: () => app.unmount(off) };
}
