// Beacon Spam: flood fake SSIDs into the airspace (security audit tool).
// WARNING: Only use in controlled environments you own.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false };

  const prefixInput = h('input', { class: 'field', placeholder: 'SSID prefix', 'aria-label': 'Prefix', value: 'FreeNet' });
  const countInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Count', min: '1', max: '1000', 'aria-label': 'SSID count', value: '20' });
  const chInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Ch', min: '0', max: '13', 'aria-label': 'Channel', value: '0' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Flood');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });

  el.append(h('div', { class: 'app beaconspam' },
    h('div', { class: 'toolbar' }, prefixInput, countInput, chInput, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'FLOODING')),
          h('div', { class: 'srow' }, h('span', {}, 'Prefix'), h('span', {}, state.prefix || 'FreeNet')),
          h('div', { class: 'srow' }, h('span', {}, 'SSIDs'), h('span', {}, String(state.ssids || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Beacons sent'), h('span', {}, String(state.beacons || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, String(state.channel || 0))))
      : h('div', { class: 'empty' }, '⚠ Security audit. Floods fake SSID beacons.'));
  }

  async function start() {
    try {
      state = await api.beaconspamStart({
        prefix: prefixInput.value.trim() || 'FreeNet',
        count: Number(countInput.value) || 20,
        channel: Number(chInput.value) || 0
      });
      toast('Beacon flood started');
    } catch (e) { toast(errorText(e, 'Beacon'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.beaconspamStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.beaconspam(); if (!alive) return; render(); }
    catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  return { unmount() { alive = false; } };
}