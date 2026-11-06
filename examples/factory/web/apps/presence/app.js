// Wi-Fi CSI motion/presence sensing, with separate ESP-NOW nearby-peer discovery.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, { api, events }) {
  let alive = true, requestSeq = 0, busy = false;
  let state = {
    active: false, peers_ever: 0, peers: [], history: [], error: null,
    csi: { active: false, state: 'stopped', calibrating: false, present: false, motion: false,
      score: 0, threshold: 0.35, samples: 0, rate: 0, drops: 0, rssi: -90, channel: null, ssid: null, error: null },
  };
  const csiStartBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => startCsi() }, icon('play'), 'Start sensing');
  const csiStopBtn = h('button', { class: 'btn', type: 'button', onclick: () => stopCsi(), disabled: true }, icon('stop'), 'Stop sensing');
  const calibrateBtn = h('button', { class: 'btn', type: 'button', onclick: () => calibrate(), disabled: true }, 'Recalibrate');
  const threshold = h('input', { type: 'range', min: 0.05, max: 0.95, step: 0.05, value: 0.35,
    'aria-label': 'Motion detection threshold', oninput: () => { thresholdValue.textContent = Number(threshold.value).toFixed(2); } });
  const thresholdValue = h('span', { class: 'tag' }, '0.35');
  const csiInfo = h('div', {});
  const scoreBar = h('i', { style: { width: '0%' } });
  const peerStartBtn = h('button', { class: 'btn primary', type: 'button', onclick: () => startPeers() }, icon('play'), 'Start discovery');
  const peerStopBtn = h('button', { class: 'btn', type: 'button', onclick: () => stopPeers(), disabled: true }, icon('stop'), 'Stop discovery');
  const peerInfo = h('div', {});
  const peerList = h('div', { class: 'list' });
  const historyList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app presence' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Wi-Fi CSI motion and presence'),
      h('p', { class: 'muted' }, 'Estimates motion from changes in the channel response of frames from your connected 2.4 GHz router. Keep the watch and router fixed. This is not a camera, person counter or precise location sensor.'),
      h('p', { class: 'muted' }, 'Start with an empty, still room. Calibration needs at least 32 CSI frames; presence remains latched for 10 seconds after motion.')),
    h('div', { class: 'panel' },
      h('h3', {}, 'Detection'),
      h('div', { class: 'frow' }, h('label', {}, 'Threshold '), threshold, thresholdValue),
      h('div', { class: 'row gap' }, csiStartBtn, csiStopBtn, calibrateBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Live status'), csiInfo,
      h('div', { class: 'progress', 'aria-label': 'Motion score' }, scoreBar)),
    h('details', { class: 'panel' }, h('summary', {}, 'Nearby ESP-NOW peers'),
      h('p', { class: 'muted' }, 'Separate discovery for nearby watches using the same ESP-NOW service as Link.'),
      h('div', { class: 'row gap' }, peerStartBtn, peerStopBtn), peerInfo,
      h('h3', {}, 'Nearby peers'), peerList,
      h('h3', {}, 'Arrival and departure history'), historyList)));

  const row = (label, value, className = '') => h('div', { class: 'srow' }, h('span', {}, label), h('span', { class: className }, String(value)));
  function render() {
    const csi = state.csi || {};
    const csiState = !csi.active ? 'STOPPED' : csi.calibrating ? 'CALIBRATING' : csi.motion ? 'MOTION' : csi.present ? 'PRESENT' : 'CLEAR';
    csiStartBtn.disabled = busy || csi.active;
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
  }

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const next = await api.presence();
      if (!alive || seq !== requestSeq) return;
      state = { ...state, ...next, csi: { ...state.csi, ...(next.csi || {}) } };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) csiInfo.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Presence'))); }
  }
  async function action(fn, message) {
    busy = true; render();
    try { await fn(); toast(message); await load(); }
    catch (e) { toast(errorText(e, 'Presence'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  const startCsi = () => action(() => api.presenceCsiStart(Number(threshold.value)), 'CSI sensing started; keep the room still for calibration');
  const stopCsi = () => action(() => api.presenceCsiStop(), 'CSI sensing stopped');
  const calibrate = () => action(() => api.presenceCsiCalibrate(), 'Recalibrating; keep the room still');
  const startPeers = () => action(() => api.presenceStart(), 'Peer discovery started');
  const stopPeers = () => action(() => api.presenceStop(), 'Peer discovery stopped');

  const off = events.on('presence.csi', () => { if (alive) load(false); });
  const timer = setInterval(() => { if (state.active || state.csi?.active) load(false); }, 2000);
  render(); load();
  return { unmount() { alive = false; off(); clearInterval(timer); } };
}
