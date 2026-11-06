// Dual-link Wi-Fi CSI presence, optional Faro transmitter, alarm and nearby peers.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';
import { drawLines, palette } from '../../js/chart.js';

export function mount(el, { api, events }) {
  let alive = true, requestSeq = 0, busy = false;
  let state = {
    active: false, peers_ever: 0, peers: [], history: [], error: null,
    csi: { active: false, state: 'stopped', calibrating: false, present: false, motion: false,
      score: 0, threshold: 0.35, samples: 0, rate: 0, drops: 0, rssi: -90, channel: null, ssid: null,
      zones: [], history: [], error: null },
    faro: { active: false, channel: null, mac: null, sent: 0, rate: 0, tx_errors: 0, error: null },
    alarm: { state: 'off', remaining_s: 0, exit_s: 10, duration_s: 30, triggers: 0, zone: null, error: null },
  };
  const csiStartBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => startCsi() }, icon('play'), 'Start sensing');
  const csiStopBtn = h('button', { class: 'btn', type: 'button', onclick: () => stopCsi(), disabled: true }, icon('stop'), 'Stop sensing');
  const calibrateBtn = h('button', { class: 'btn', type: 'button', onclick: () => calibrate(), disabled: true }, 'Recalibrate both zones');
  const threshold = h('input', { type: 'range', min: 0.05, max: 0.95, step: 0.05, value: 0.35,
    'aria-label': 'Motion detection threshold', oninput: () => { thresholdValue.textContent = Number(threshold.value).toFixed(2); } });
  const thresholdValue = h('span', { class: 'tag' }, '0.35');
  const csiInfo = h('div', {});
  const faroZoneInfo = h('div', {});
  const scoreBar = h('i', { style: { width: '0%' } });
  const historyCanvas = h('canvas', { class: 'chart', 'aria-label': 'Router and Faro motion score over the last 60 seconds' });
  const historyInfo = h('p', { class: 'muted' }, 'No motion buckets yet.');
  const faroStartBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => startFaro() }, icon('play'), 'Start Faro transmitter');
  const faroStopBtn = h('button', { class: 'btn', type: 'button', onclick: () => stopFaro(), disabled: true }, icon('stop'), 'Stop Faro transmitter');
  const faroInfo = h('div', {});
  const alarmExit = h('select', { 'aria-label': 'Alarm exit delay' },
    ...[5, 10, 30, 60].map(n => h('option', { value: n, selected: n === 10 }, `${n} seconds`)));
  const alarmDuration = h('select', { 'aria-label': 'Alarm siren duration' },
    ...[10, 30, 60, 120, 300].map(n => h('option', { value: n, selected: n === 30 }, `${n} seconds`)));
  const alarmArmBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => armAlarm() }, icon('play'), 'Arm alarm');
  const alarmDisarmBtn = h('button', { class: 'btn', type: 'button', onclick: () => disarmAlarm(), disabled: true }, icon('stop'), 'Disarm alarm');
  const alarmInfo = h('div', {});
  const peerStartBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => startPeers() }, icon('play'), 'Start discovery');
  const peerStopBtn = h('button', { class: 'btn', type: 'button', onclick: () => stopPeers(), disabled: true }, icon('stop'), 'Stop discovery');
  const peerInfo = h('div', {});
  const peerList = h('div', { class: 'list' });
  const historyList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app presence' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Wi-Fi CSI motion and presence'),
      h('p', { class: 'muted' }, 'Estimates motion from changes in the channel response of frames from your connected 2.4 GHz router. Keep the watch and router fixed. This is not a camera, person counter or precise location sensor.'),
      h('p', { class: 'muted' }, 'Start with an empty, still room. Each radio link calibrates independently for at least 32 CSI frames; presence remains latched for 10 seconds after motion.')),
    h('div', { class: 'panel' },
      h('h3', {}, 'Detection'),
      h('div', { class: 'frow' }, h('label', {}, 'Threshold '), threshold, thresholdValue),
      h('div', { class: 'row gap' }, csiStartBtn, csiStopBtn, calibrateBtn)),
    h('div', { class: 'grid2' },
      h('div', { class: 'panel' }, h('h3', {}, 'Router zone'), csiInfo,
        h('div', { class: 'progress', 'aria-label': 'Router motion score' }, scoreBar)),
      h('div', { class: 'panel faro-zone' }, h('h3', {}, 'Faro zone'), faroZoneInfo)),
    h('div', { class: 'panel history-panel' }, h('h3', {}, 'Motion history · last 60 seconds'), historyCanvas,
      h('div', { class: 'legend' },
        h('span', {}, h('i', { style: { background: palette()[0] } }), 'Router'),
        h('span', {}, h('i', { style: { background: palette()[1] } }), 'Faro')),
      historyInfo),
    h('details', { class: 'panel faro-transmitter' }, h('summary', {}, 'Faro transmitter · second watch'),
      h('p', { class: 'muted' }, 'Turns this watch into a 100 Hz ESP-NOW transmitter instead of a sensor. Use the same Wi-Fi channel as the receiving watch. A second radio path creates an independent detection zone.'),
      h('div', { class: 'row gap' }, faroStartBtn, faroStopBtn), faroInfo),
    h('div', { class: 'panel alarm-panel' }, h('h3', {}, 'Intrusion alarm'),
      h('p', { class: 'muted' }, 'Only live motion in either zone triggers the alarm; the 10-second presence hold does not. The siren automatically disarms after its configured duration.'),
      h('div', { class: 'form' },
        h('label', {}, 'Exit delay', alarmExit), h('label', {}, 'Siren duration', alarmDuration)),
      h('div', { class: 'row gap' }, alarmArmBtn, alarmDisarmBtn), alarmInfo),
    h('details', { class: 'panel' }, h('summary', {}, 'Nearby ESP-NOW peers'),
      h('p', { class: 'muted' }, 'Separate discovery for nearby watches using the same ESP-NOW service as Link.'),
      h('div', { class: 'row gap' }, peerStartBtn, peerStopBtn), peerInfo,
      h('h3', {}, 'Nearby peers'), peerList,
      h('h3', {}, 'Arrival and departure history'), historyList)));

  const row = (label, value, className = '') => h('div', { class: 'srow' }, h('span', {}, label), h('span', { class: className }, String(value)));
  const zone = id => (state.csi.zones || []).find(z => z.id === id) || {};
  const zoneText = z => !z.linked ? (z.id === 'faro' ? (z.discovered ? 'ABSENT' : 'WAITING') : 'STOPPED') :
    z.calibrating ? 'CALIBRATING' : z.motion ? 'MOTION' : z.present ? 'PRESENT' : 'CLEAR';
  function drawHistory() {
    const colors = palette();
    const samples = state.csi.history || [];
    drawLines(historyCanvas, [
      { values: samples.map(x => x.router ?? 0), color: colors[0] },
      { values: samples.map(x => x.faro ?? 0), color: colors[1] },
    ], { cap: 120, min: 0, max: 100, unit: '%' });
    historyInfo.textContent = samples.length ? `${samples.length}/120 buckets · 500 ms peak motion per bucket · session only; reboot clears history` : 'No motion buckets yet. Start sensing to fill the 60-second graph.';
  }
  function render() {
    const csi = state.csi || {}, router = zone('router'), faroZone = zone('faro');
    const csiState = !csi.active ? 'STOPPED' : csi.calibrating ? 'CALIBRATING' : csi.motion ? 'MOTION' : csi.present ? 'PRESENT' : 'CLEAR';
    csiStartBtn.disabled = busy || csi.active || state.faro.active;
    csiStopBtn.disabled = busy || !csi.active;
    calibrateBtn.disabled = busy || !csi.active;
    threshold.disabled = busy || csi.active;
    if (document.activeElement !== threshold && typeof csi.threshold === 'number') {
      threshold.value = csi.threshold;
      thresholdValue.textContent = csi.threshold.toFixed(2);
    }
    scoreBar.style.width = `${Math.round(Math.max(0, Math.min(1, csi.score || 0)) * 100)}%`;
    csiInfo.replaceChildren(
      row('State', csiState, `tag ${csi.motion ? 'alert' : csi.active ? 'ok' : ''}`),
      row('Motion score', `${Math.round((csi.score || 0) * 100)}% (threshold ${Math.round((csi.threshold || 0) * 100)}%)`),
      row('CSI frames', `${csi.samples || 0} total · ${csi.rate || 0}/s · ${csi.drops || 0} dropped`),
      row('Network', `${csi.ssid || 'Not connected'} · channel ${csi.channel || '—'} · ${csi.rssi || 0} dBm`),
      csi.error ? h('p', { class: 'inline-error', role: 'alert' }, csi.error) : null);
    faroZoneInfo.replaceChildren(
      row('State', zoneText(faroZone), `tag ${faroZone.motion ? 'alert' : faroZone.linked ? 'ok' : ''}`),
      row('Motion score', `${Math.round((faroZone.score || 0) * 100)}%`),
      row('CSI frames', `${faroZone.samples || 0} total · ${faroZone.rate || 0}/s`),
      row('Transmitter', faroZone.mac || 'No Faro discovered'),
      row('Signal', `${faroZone.rssi || 0} dBm · link expires after 3 s without frames`),
      !faroZone.linked ? h('p', { class: 'muted' }, 'Optional. Start Faro transmitter mode on a second watch on this channel.') : null);

    faroStartBtn.disabled = busy || state.faro.active || csi.active;
    faroStopBtn.disabled = busy || !state.faro.active;
    faroInfo.replaceChildren(
      row('Mode', state.faro.active ? 'TRANSMITTING' : 'STOPPED', `tag ${state.faro.active ? 'ok' : ''}`),
      row('Channel', state.faro.channel || '—'),
      row('MAC', state.faro.mac || '—'),
      row('Frames', `${state.faro.sent || 0} accepted · ${state.faro.rate || 0}/s · ${state.faro.tx_errors || 0} errors`),
      h('p', { class: 'muted' }, 'Accepted means queued by ESP-NOW, not confirmed over the air.'),
      state.faro.error ? h('p', { class: 'inline-error', role: 'alert' }, state.faro.error) : null);

    const alarm = state.alarm || {}, alarmActive = alarm.state !== 'off';
    alarmArmBtn.disabled = busy || alarmActive || !csi.active || csi.calibrating;
    alarmDisarmBtn.disabled = busy || !alarmActive;
    alarmExit.disabled = busy || alarmActive;
    alarmDuration.disabled = busy || alarmActive;
    if (!alarmActive && !busy) {
      if (document.activeElement !== alarmExit && [...alarmExit.options].some(o => Number(o.value) === alarm.exit_s)) alarmExit.value = alarm.exit_s;
      if (document.activeElement !== alarmDuration && [...alarmDuration.options].some(o => Number(o.value) === alarm.duration_s)) alarmDuration.value = alarm.duration_s;
    }
    alarmInfo.replaceChildren(
      row('State', String(alarm.state || 'off').toUpperCase(), `tag ${alarm.state === 'triggered' ? 'alert' : alarmActive ? 'warn' : ''}`),
      row('Remaining', alarm.remaining_s ? `${alarm.remaining_s} s` : '—'),
      row('Last trigger zone', alarm.zone || '—'),
      row('Triggers this session', alarm.triggers || 0),
      alarm.state === 'triggered' ? h('p', { class: 'inline-error', role: 'alert' }, `Motion alarm: ${alarm.zone || 'unknown'} zone`) : null,
      alarm.error ? h('p', { class: 'inline-error', role: 'alert' }, alarm.error) : null);

    peerStartBtn.disabled = busy || state.active;
    peerStopBtn.disabled = busy || !state.active;
    const peers = [...(state.peers || [])].sort((a, b) => Number(b.present) - Number(a.present) || b.rssi - a.rssi);
    const present = peers.filter(p => p.present).length;
    peerInfo.replaceChildren(
      row('Discovery', state.active ? 'DISCOVERING' : 'STOPPED', `tag ${state.active ? 'ok' : ''}`),
      row('Peers now', present),
      row('Unique peers this session', state.peers_ever || 0),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    peerList.replaceChildren(...(peers.length ? peers.map(p => h('div', { class: 'note' },
      h('div', {}, h('b', {}, p.name || 'Unknown peer'), h('p', {}, `${p.mac} · ${p.present ? 'Nearby' : 'Last seen ' + fmtTime(p.last_seen)}`)),
      h('span', { class: 'when' }, `${p.rssi || 0} dBm`))) : [h('p', { class: 'muted' }, 'No peers discovered yet.')]));
    const history = [...(state.history || [])].reverse().slice(0, 12);
    historyList.replaceChildren(...(history.length ? history.map(x => h('div', { class: 'note' },
      h('div', {}, h('b', {}, x.event === 'appeared' ? 'Appeared' : 'Disappeared'), h('p', {}, x.name || x.mac)),
      h('span', { class: 'when' }, fmtTime(x.ts)))) : [h('p', { class: 'muted' }, 'No arrivals or departures yet.')]));
    drawHistory();
  }

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const next = await api.presence();
      if (!alive || seq !== requestSeq) return;
      state = { ...state, ...next,
        csi: { ...state.csi, ...(next.csi || {}) },
        faro: { ...state.faro, ...(next.faro || {}) },
        alarm: { ...state.alarm, ...(next.alarm || {}) } };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) csiInfo.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Presence'))); }
  }
  async function action(fn, message) {
    busy = true; render();
    try { await fn(); toast(message); await load(); }
    catch (e) { toast(errorText(e, 'Presence'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  const startCsi = () => action(() => api.presenceCsiStart(Number(threshold.value)), 'CSI sensing started; keep both radio paths still for calibration');
  const stopCsi = () => action(() => api.presenceCsiStop(), 'CSI sensing stopped; alarm disarmed');
  const calibrate = () => action(() => api.presenceCsiCalibrate('all'), 'Recalibrating both zones; keep the room still');
  const startFaro = () => action(() => api.presenceFaroStart(), 'Faro transmitter started at 100 Hz');
  const stopFaro = () => action(() => api.presenceFaroStop(), 'Faro transmitter stopped');
  const armAlarm = () => {
    const exit = Number(alarmExit.value), duration = Number(alarmDuration.value);
    return action(() => api.presenceAlarmArm(exit, duration), 'Alarm arming; leave the detection zones');
  };
  const disarmAlarm = () => action(() => api.presenceAlarmDisarm(), 'Alarm disarmed');
  const startPeers = () => action(() => api.presenceStart(), 'Peer discovery started');
  const stopPeers = () => action(() => api.presenceStop(), 'Peer discovery stopped');

  const offs = ['presence.csi', 'presence.alarm', 'presence.faro'].map(name => events.on(name, () => { if (alive) load(false); }));
  const timer = setInterval(() => {
    if (state.active || state.csi?.active || state.faro?.active || state.alarm?.state !== 'off') load(false);
  }, 2000);
  const ro = new ResizeObserver(drawHistory);
  ro.observe(historyCanvas);
  render(); load();
  return { unmount() { alive = false; offs.forEach(off => off()); clearInterval(timer); ro.disconnect(); } };
}
