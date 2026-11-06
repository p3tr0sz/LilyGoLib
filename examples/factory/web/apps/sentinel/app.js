// Sentinel: passive detection of persistent BLE location trackers.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, { api, events, notify }) {
  let alive = true, requestSeq = 0, busy = false;
  let state = { active: false, scanning: false, device_count: 0, devices: [], error: null };
  const startBtn = h('button', { class: 'btn primary', type: 'button', onclick: start }, icon('play'), 'Start passive scan');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: stop, disabled: true }, icon('stop'), 'Stop');
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
    startBtn.disabled = busy || state.active;
    stopBtn.disabled = busy || !state.active;
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

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const next = await api.sentinel();
      if (!alive || seq !== requestSeq) return;
      state = { ...state, ...next };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Sentinel'))); }
  }
  async function start() {
    busy = true; render();
    try { await api.sentinelStart(); toast('Passive BLE scan started'); await load(); }
    catch (e) { toast(errorText(e, 'Sentinel'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function stop() {
    busy = true; render();
    try { await api.sentinelStop(); toast('BLE scan stopped'); await load(); }
    catch (e) { toast(errorText(e, 'Sentinel'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }

  const off = events.on('sentinel', d => {
    if (!alive) return;
    notify('Persistent tracker', d.detail || `${d.kind || 'Tracker'} remains nearby`, 'warn');
    load(false);
  });
  const timer = setInterval(() => { if (state.active) load(false); }, 3000);
  render(); load();
  return { unmount() { alive = false; off(); clearInterval(timer); } };
}
