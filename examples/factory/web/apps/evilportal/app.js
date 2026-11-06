// Evil Portal: rogue AP + captive portal for credential harvesting (security audit tool).
// WARNING: Only use on networks you own or have explicit permission to test.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, captures: [] };

  const ssidInput = h('input', { class: 'field', placeholder: 'SSID (default: Free Wi-Fi)', 'aria-label': 'SSID', value: 'Free Wi-Fi' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start AP');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const clearBtn = h('button', { class: 'btn', onclick: clearCap }, icon('delete'), 'Clear');
  const info = h('div', { class: 'tool' });
  const caps = h('div', { class: 'notes' });

  el.append(h('div', { class: 'app evilportal' },
    h('div', { class: 'toolbar' }, ssidInput, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Captured Credentials', ' ', clearBtn), caps)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'ACTIVE')),
          h('div', { class: 'srow' }, h('span', {}, 'SSID'), h('span', { class: 'mono' }, state.ssid || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Clients'), h('span', {}, String(state.clients || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Attempts'), h('span', {}, String(state.attempts || 0))))
      : h('div', { class: 'empty' }, '⚠ Security audit tool. Only use on authorized networks.'));
    caps.replaceChildren(state.captures.length
      ? state.captures.slice(-20).reverse().map((c) => h('div', { class: 'note' },
          h('div', {}, h('b', {}, c.username || '?'), ' : ', c.password || '?'),
          h('div', { class: 'sub' }, c.url || '', ' · ', fmtTime(c.ts))))
      : h('p', { class: 'muted' }, 'No credentials captured yet.'));
  }

  async function start() {
    try { state = await api.evilportalStart(ssidInput.value.trim() || 'Free Wi-Fi'); toast('Portal active'); } catch (e) { toast(errorText(e, 'Portal'), 'error'); }
    load();
  }
  async function stop() {
    try { await api.evilportalStop(); toast('Portal stopped'); } catch (e) { toast(errorText(e), 'error'); }
    load();
  }
  async function load() {
    try {
      state = await api.evilportal();
      const c = await api.evilportalCaptures();
      if (!alive) return;
      state.captures = c.captures || [];
      render();
    } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }
  async function clearCap() { try { await api.evilportalClear(); toast('Captures cleared'); load(); } catch (e) { toast(errorText(e), 'error'); } }
  load();
  return { unmount() { alive = false; } };
}