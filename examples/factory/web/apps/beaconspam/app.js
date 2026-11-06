// Authorized fake-beacon injection with generated or custom SSIDs.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';

export function mount(el, { api }) {
  let alive = true, requestSeq = 0, busy = false;
  let state = { active: false, channel: 0, beacons: 0, ssids: 0, txErrors: 0, customSsids: [], error: null };
  const mode = h('select', { class: 'field', 'aria-label': 'Beacon source' },
    h('option', { value: 'generated' }, 'Generated SSIDs'), h('option', { value: 'custom' }, 'Custom SSID list'));
  const prefixInput = h('input', { class: 'field', value: 'FreeNet', maxlength: '20', 'aria-label': 'Generated SSID prefix' });
  const countInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '10000', value: '20', 'aria-label': 'Generated SSID count' });
  const channelInput = h('input', { class: 'field w3', type: 'number', min: '0', max: '13', value: '0', 'aria-label': 'Channel; zero hops all channels' });
  const customArea = h('textarea', { class: 'field', rows: 4, placeholder: 'One SSID per line, 1-32 bytes each', 'aria-label': 'Custom SSIDs, one per line' });
  const saveCustomBtn = h('button', { class: 'btn', type: 'button', onclick: setCustom }, 'Save custom list');
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start test');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', {});

  el.append(h('div', { class: 'app beaconspam' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Authorized radio test only'),
      h('p', { class: 'muted' }, 'Broadcasts fake networks and can disrupt nearby Wi-Fi use. Starting also drops the watch’s normal Wi-Fi link.')),
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'Source', mode),
        h('label', {}, 'Prefix', prefixInput),
        h('label', {}, 'SSID count', countInput),
        h('label', {}, 'Channel (0 = hop)', channelInput)),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('details', { class: 'panel' },
      h('summary', {}, 'Custom SSID list'), customArea,
      h('p', { class: 'muted' }, 'Up to 64 entries. Save the list, then choose “Custom SSID list” as the source.'),
      saveCustomBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Test status'), info)));

  function render() {
    startBtn.disabled = busy || state.active;
    stopBtn.disabled = busy || !state.active;
    saveCustomBtn.disabled = busy || state.active;
    prefixInput.disabled = mode.value !== 'generated';
    countInput.disabled = mode.value !== 'generated';
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'alert' : ''}` }, state.active ? 'INJECTING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Source'), h('span', {}, state.mode === 2 ? 'Custom list' : `Generated: ${state.prefix || '—'}`)),
      h('div', { class: 'srow' }, h('span', {}, 'Accepted beacons'), h('span', {}, String(state.beacons || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Injection errors'), h('span', { class: state.txErrors ? 'tag alert' : '' }, String(state.txErrors || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'SSIDs / channel'), h('span', {}, `${state.ssids || 0} / ${state.channel || 'hop'}`)),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    if (document.activeElement !== customArea) customArea.value = (state.customSsids || []).join('\n');
  }

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const next = await api.beaconspam();
      if (!alive || seq !== requestSeq) return;
      state = { ...state, ...next };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Beacon Spam'))); }
  }
  function customLines() {
    return customArea.value.split('\n').map(x => x.trim()).filter(Boolean);
  }
  async function setCustom() {
    const lines = customLines();
    if (!lines.length || lines.length > 64 || lines.some(x => new TextEncoder().encode(x).length > 32)) {
      toast('Use 1-64 non-empty SSIDs, each at most 32 UTF-8 bytes.', 'error'); return;
    }
    busy = true; render();
    try { await api.beaconspamSetSsids(lines); toast(`Saved ${lines.length} custom SSIDs`); await load(); }
    catch (e) { toast(errorText(e, 'Custom SSIDs'), 'error'); }
    finally { busy = false; if (alive) render(); }
  }
  async function start() {
    if (mode.value === 'custom' && !customLines().length && !(state.customSsids || []).length) {
      toast('Add and save at least one custom SSID first.', 'error'); return;
    }
    if (!await confirmDialog('Start beacon injection?', `Broadcast ${mode.value === 'custom' ? 'custom' : 'generated'} fake networks on ${Number(channelInput.value) ? 'channel ' + channelInput.value : 'all channels'}? Only continue in an environment you control.`, 'Start test', true)) return;
    busy = true; render();
    try {
      if (mode.value === 'custom' && customArea.value !== (state.customSsids || []).join('\n')) await api.beaconspamSetSsids(customLines());
      await api.beaconspamStart({ mode: mode.value, prefix: prefixInput.value.trim(), count: Number(countInput.value), channel: Number(channelInput.value), authorized: true });
      toast('Beacon test started'); await load();
    } catch (e) { toast(errorText(e, 'Beacon Spam'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function stop() {
    busy = true; render();
    try { await api.beaconspamStop(); toast('Beacon test stopped; Wi-Fi reconnect requested'); await load(); }
    catch (e) { toast(errorText(e, 'Beacon Spam'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }

  mode.addEventListener('change', render);
  const timer = setInterval(() => { if (state.active) load(false); }, 1500);
  render(); load();
  return { unmount() { alive = false; clearInterval(timer); } };
}
