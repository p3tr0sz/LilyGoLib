// Nearby Peers: ESP-NOW discovery (not Wi-Fi CSI human-motion sensing).
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, { api }) {
  let alive = true, requestSeq = 0, busy = false;
  let state = { active: false, peers_ever: 0, peers: [], history: [], error: null };
  const startBtn = h('button', { class: 'btn primary', type: 'button', onclick: start }, icon('play'), 'Start discovery');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', {});
  const peerList = h('div', { class: 'list' });
  const historyList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app presence' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Nearby ESP-NOW peers'),
      h('p', { class: 'muted' }, 'Discovers nearby watches using the same ESP-NOW service as Link. This does not detect people or motion through Wi-Fi CSI.')),
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Nearby peers'), peerList),
    h('details', { class: 'panel' }, h('summary', {}, 'Arrival and departure history'), historyList)));

  function render() {
    startBtn.disabled = busy || state.active;
    stopBtn.disabled = busy || !state.active;
    const peers = [...(state.peers || [])].sort((a, b) => Number(b.present) - Number(a.present) || b.rssi - a.rssi);
    const present = peers.filter(p => p.present).length;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'ok' : ''}` }, state.active ? 'DISCOVERING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Peers now'), h('span', {}, String(present))),
      h('div', { class: 'srow' }, h('span', {}, 'Unique peers this session'), h('span', {}, String(state.peers_ever || 0))),
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
      state = { ...state, ...next };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Nearby Peers'))); }
  }
  async function start() {
    busy = true; render();
    try { await api.presenceStart(); toast('Peer discovery started'); await load(); }
    catch (e) { toast(errorText(e, 'Nearby Peers'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function stop() {
    busy = true; render();
    try { await api.presenceStop(); toast('Peer discovery stopped'); await load(); }
    catch (e) { toast(errorText(e, 'Nearby Peers'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }

  const timer = setInterval(() => { if (state.active) load(false); }, 3000);
  render(); load();
  return { unmount() { alive = false; clearInterval(timer); } };
}
