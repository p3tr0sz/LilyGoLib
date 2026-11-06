// Presence: ESP-NOW nearby devices — extends the existing Link tool with persistent peer tracking.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = { active: false, peers: [], history: [] };

  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Scan');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const info = h('div', { class: 'tool' });
  const peers = h('div', { class: 'notes' });
  const hist = h('div', { class: 'notes' });

  el.append(h('div', { class: 'app presence' },
    h('div', { class: 'toolbar' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Nearby Peers'), peers),
    h('div', { class: 'panel' }, h('h3', {}, 'History'), hist)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'Scanning'),
        h('span', {}, `${state.peers_ever || 0} peers ever`))
      : h('div', { class: 'empty' }, 'Tap Scan to discover nearby ESP-NOW devices.'));
    peers.replaceChildren((state.peers || []).length
      ? h('table', { class: 'tbl' },
          h('thead', {}, h('tr', {}, h('th', {}, 'Name'), h('th', {}, 'MAC'), h('th', {}, 'Signal'), h('th', {}, 'Status'))),
          h('tbody', {}, (state.peers || []).map((p) => h('tr', {},
            h('td', {}, p.name || '—'), h('td', { class: 'mono' }, p.mac),
            h('td', { class: 'num' }, `${p.rssi} dBm`),
            h('td', {}, p.present ? h('span', { class: 'tag ok' }, 'here') : h('span', { class: 'tag' }, 'gone'))))))
      : h('p', { class: 'muted' }, 'No peers found.'));
    hist.replaceChildren((state.history || []).length
      ? state.history.slice(-10).reverse().map((h) => h('div', { class: 'srow' }, fmtTime(h.ts), h.name || h.mac, h.event))
      : h('p', { class: 'muted' }, 'No history yet.'));
  }

  async function start() { try { await api.presenceStart(); toast('Scanning for peers'); } catch (e) { toast(errorText(e), 'error'); } load(); }
  async function stop() { try { await api.presenceStop(); toast('Stopped'); } catch (e) { toast(errorText(e), 'error'); } load(); }
  async function load() { try { const s = await api.presence(); if (!alive) return; state = s; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); } }
  const off = events.on('presence', () => { if (alive) load(); });
  load();
  return { unmount() { alive = false; off(); } };
}