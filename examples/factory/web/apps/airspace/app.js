// Airspace: passive Wi-Fi attack detection with explicit, textual alerts.
import { h, icon, errorText, fmtTime } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

export function mount(el, { api, events, notify }) {
  let state = { active: false, deauth_total: 0, beacon_total: 0, deauth_floods: 0, beacon_floods: 0, evil_twins: 0, broadcast_deauth: 0, error: null };
  const alerts = [];
  const startBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => app.action(() => api.airspaceStart(), 'Monitoring started') }, icon('play'), 'Start monitor');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.airspaceStop(), 'Monitoring stopped'), disabled: true }, icon('stop'), 'Stop');
  const info = h('div', {});
  const alertList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app airspace' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Passive Wi-Fi attack detection'),
      h('p', { class: 'muted' }, 'Listens only; never injects frames. Alerts are indicators to investigate, not proof of an attacker.')),
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Live counters'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Recent alerts'), alertList)));

  const countRow = (label, value, warning = false) => h('div', { class: 'srow' }, h('span', {}, label), h('span', { class: warning ? 'tag alert' : '' }, String(value || 0)));
  function render() {
    startBtn.disabled = app.busy || state.active;
    stopBtn.disabled = app.busy || !state.active;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'ok' : ''}` }, state.active ? 'MONITORING' : 'STOPPED')),
      countRow('Deauth/disassociation frames', state.deauth_total),
      countRow('Beacon frames', state.beacon_total),
      countRow('Deauth flood alerts', state.deauth_floods, state.deauth_floods > 0),
      countRow('Broadcast deauth alerts', state.broadcast_deauth, state.broadcast_deauth > 0),
      countRow('Beacon flood alerts', state.beacon_floods, state.beacon_floods > 0),
      countRow('Possible evil twins', state.evil_twins, state.evil_twins > 0),
      countRow('Analysis queue drops', state.drops),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    alertList.replaceChildren(...(alerts.length
      ? alerts.map(a => h('div', { class: 'note' }, h('div', {}, h('b', {}, a.type.replaceAll('_', ' ')), h('p', {}, a.detail || 'Investigate the nearby access points.')), h('span', { class: 'when' }, fmtTime(a.ts))))
      : [h('p', { class: 'muted' }, 'No alerts in this session.')]));
  }

  const app = toolApp({
    name: 'Airspace',
    read: () => api.airspace(),
    apply: (next) => { state = { ...state, ...next }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Airspace'))),
    interval: 2000,
    pollWhen: () => state.active,
  });

  const off = events.on('airspace', d => {
    if (!app.alive) return;
    alerts.unshift({ ...d, ts: Math.floor(Date.now() / 1000) });
    if (alerts.length > 10) alerts.length = 10;
    notify('Airspace alert', d.detail || d.type || 'Wi-Fi anomaly detected', 'warn');
    app.load(false);
  });
  app.start();
  return { unmount: () => app.unmount(off) };
}
