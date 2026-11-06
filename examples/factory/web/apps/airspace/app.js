// Airspace: Wi-Fi attack detection — deauth flood, beacon flood, evil-twin detection.
import { h, icon, toast, errorText, fmtDuration } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start Monitor');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });

  el.append(h('div', { class: 'app airspace' },
    h('div', { class: 'panel' },
      h('h4', {}, 'Attack Detection'),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'MONITORING')),
          h('div', { class: 'srow' }, h('span', {}, 'Deauth pkts'), h('span', {}, String(state.deauthCount || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Beacon pkts'), h('span', {}, String(state.beaconCount || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Deauth floods'), h('span', { class: state.deauthFloods > 0 ? 'tag alert' : '' }, String(state.deauthFloods || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Beacon floods'), h('span', { class: state.beaconFloods > 0 ? 'tag alert' : '' }, String(state.beaconFloods || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Evil twins'), h('span', { class: state.evilTwins > 0 ? 'tag warn' : '' }, String(state.evilTwins || 0))))
      : h('div', { class: 'empty' }, 'Passive Wi-Fi attack detection. Monitors for deauth floods, beacon floods, and evil-twin APs.'));
  }

  async function start() {
    try { await api.airspaceStart(); toast('Monitoring started'); } catch (e) { toast(errorText(e, 'Airspace'), 'error'); }
    load();
  }

  async function stop() {
    try { await api.airspaceStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }

  async function load() {
    try { state = await api.airspace(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 2000);
  return { unmount() { alive = false; clearInterval(timer); } };
}