// Deauth Flood: 802.11 deauth injection for security testing.
// WARNING: Only use on networks you own or have explicit permission to test.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false };

  const bssidInput = h('input', { class: 'field', placeholder: 'Target BSSID (e.g., aa:bb:cc:dd:ee:ff)', 'aria-label': 'Target BSSID' });
  const stationInput = h('input', { class: 'field', placeholder: 'Station MAC (ff:ff:ff:ff:ff:ff = all)', 'aria-label': 'Station MAC', value: 'ff:ff:ff:ff:ff:ff' });
  const chInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Ch', min: '1', max: '13', 'aria-label': 'Channel', value: '1' });
  const rateInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Rate', min: '1', 'aria-label': 'Pkts/s', value: '10' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Flood');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });

  el.append(h('div', { class: 'app deauthflood' },
    h('div', { class: 'toolbar' }, bssidInput, stationInput, chInput, rateInput, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'FLOODING')),
          h('div', { class: 'srow' }, h('span', {}, 'Target'), h('span', { class: 'mono' }, state.target_bssid || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Station'), h('span', { class: 'mono' }, state.target_station || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Packets sent'), h('span', {}, String(state.sent || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, String(state.channel || 1))))
      : h('div', { class: 'empty' }, '⚠ Security audit. Injects 802.11 deauth frames.'));
  }

  async function start() {
    try {
      state = await api.deauthfloodStart({
        bssid: bssidInput.value.trim(),
        station: stationInput.value.trim(),
        channel: Number(chInput.value) || 1,
        rate: Number(rateInput.value) || 10
      });
      toast('Deauth flood started');
    } catch (e) { toast(errorText(e, 'Deauth'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.deauthfloodStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.deauthflood(); if (!alive) return; render(); }
    catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  return { unmount() { alive = false; } };
}