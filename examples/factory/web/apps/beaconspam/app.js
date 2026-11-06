// Beacon Spam: flood fake SSID beacons for security testing.
// WARNING: Only use in controlled environments you own.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, customSsids: [] };

  const prefixInput = h('input', { class: 'field', placeholder: 'SSID prefix', value: 'FreeNet', 'aria-label': 'SSID prefix' });
  const countInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Count', min: '1', value: '50', 'aria-label': 'SSID count' });
  const chInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Ch', min: '1', max: '13', value: '1', 'aria-label': 'Channel' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Spam');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const customArea = h('textarea', { class: 'field', rows: 4, placeholder: 'Custom SSIDs (one per line)', 'aria-label': 'Custom SSIDs' });

  el.append(h('div', { class: 'app beaconspam' },
    h('div', { class: 'panel' },
      h('h4', {}, 'Configuration'),
      h('div', { class: 'row gap' }, prefixInput, countInput, chInput),
      h('div', { class: 'row gap' }, startBtn, stopBtn),
      h('details', {},
        h('summary', {}, 'Custom SSID List'),
        customArea,
        h('button', { class: 'btn', onclick: setCustom }, 'Set Custom SSIDs'))),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'SPAMMING')),
          h('div', { class: 'srow' }, h('span', {}, 'Prefix'), h('span', { class: 'mono' }, state.prefix || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Beacons'), h('span', {}, String(state.beacons || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'SSIDs'), h('span', {}, String(state.ssids || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, String(state.channel || 1))))
      : h('div', { class: 'empty' }, '⚠ Beacon flood — authorized use only.'));
    if (state.customSsids && state.customSsids.length) {
      customArea.value = state.customSsids.join('\n');
    }
  }

  async function start() {
    try {
      state = await api.beaconspamStart({
        prefix: prefixInput.value.trim() || 'FreeNet',
        count: Number(countInput.value) || 50,
        channel: Number(chInput.value) || 1
      });
      toast('Beacon spam started');
    } catch (e) { toast(errorText(e, 'Beacon'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.beaconspamStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function setCustom() {
    const lines = customArea.value.split('\n').filter(l => l.trim());
    try { await api.beaconspamSetSsids(lines); toast(`Set ${lines.length} custom SSIDs`); } catch (e) { toast(errorText(e), 'error'); }
  }

  async function load() {
    try { state = await api.beaconspam(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 2000);
  return { unmount() { alive = false; clearInterval(timer); } };
}