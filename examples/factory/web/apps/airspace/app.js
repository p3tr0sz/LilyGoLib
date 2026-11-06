// Airspace: Wi-Fi attack detection from passive capture (deauth floods, evil-twin APs, beacon floods).
// The blue-team counterpart of offensive Wi-Fi tools — fully passive detection only.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = { active: false };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Monitor');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const alerts = h('div', { class: 'notes' });
  let alertList = [];

  el.append(h('div', { class: 'app airspace' },
    h('div', { class: 'toolbar' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Overview'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Alerts'), alerts)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'Monitoring')),
          h('div', { class: 'srow' }, h('span', {}, 'Deauths'), h('span', {}, String(state.deauth_total || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Deauth floods'), h('span', {}, String(state.deauth_floods || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Beacons'), h('span', {}, String(state.beacon_total || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Beacon floods'), h('span', {}, String(state.beacon_floods || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Evil-twin candidates'), h('span', {}, String(state.evil_twins || 0))))
      : h('div', { class: 'empty' }, 'Tap Monitor to start passive airspace analysis.'));
    alerts.replaceChildren(alertList.length
      ? alertList.slice(-20).reverse().map((a) => h('div', { class: `note ${a.type}` },
          h('span', { class: 'tag warn' }, a.type.replace('_', ' ')),
          h('div', {}, h('b', {}, a.detail)),
          a.bssid ? h('div', { class: 'mono sub' }, a.bssid) : null))
      : h('p', { class: 'muted' }, 'No alerts yet.'));
  }

  async function start() {
    try { await api.airspaceStart(); toast('Monitoring started'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }
  async function stop() {
    try { await api.airspaceStop(); toast('Monitoring stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }
  async function load() {
    try { const s = await api.airspace(); if (!alive) return; state = s; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }
  const off = events.on('airspace', (d) => { if (!alive) return; alertList = [...alertList, { ...d, ts: Date.now() / 1000 }]; render(); });
  load();
  return { unmount() { alive = false; off(); } };
}