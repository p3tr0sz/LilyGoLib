// Deauth Flood: 802.11 deauth injection for security testing.
// WARNING: Only use on networks you own or have explicit permission to test.
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, targets: [] };

  const bssidInput = h('input', { class: 'field', placeholder: 'Target BSSID (aa:bb:cc:dd:ee:ff)', 'aria-label': 'Target BSSID' });
  const stationInput = h('input', { class: 'field', placeholder: 'Station MAC (ff:ff:ff:ff:ff:ff = all)', 'aria-label': 'Station MAC', value: 'ff:ff:ff:ff:ff:ff' });
  const chInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Ch', min: '1', max: '13', 'aria-label': 'Channel', value: '1' });
  const rateInput = h('input', { class: 'field w3', type: 'number', placeholder: 'Rate', min: '1', 'aria-label': 'Pkts/s', value: '10' });
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Flood');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const scanBtn = h('button', { class: 'btn', onclick: scan }, icon('search'), 'Scan APs');
  const info = h('div', { class: 'tool' });
  const targetList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app deauthflood' },
    h('div', { class: 'panel' },
      h('h4', {}, 'Target Configuration'),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' }, bssidInput),
        h('div', { class: 'grow' }, stationInput)),
      h('div', { class: 'row gap' },
        chInput, rateInput, scanBtn, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Nearby APs'), targetList),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'FLOODING')),
          h('div', { class: 'srow' }, h('span', {}, 'Target'), h('span', { class: 'mono' }, state.targetBssid || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Station'), h('span', { class: 'mono' }, state.targetStation || '—')),
          h('div', { class: 'srow' }, h('span', {}, 'Sent'), h('span', {}, String(state.sent || 0))),
          h('div', { class: 'srow' }, h('span', {}, 'Channel'), h('span', {}, String(state.channel || 1))),
          h('div', { class: 'srow' }, h('span', {}, 'Rate'), h('span', {}, (state.rateKbps || 0) + ' pkts/s')))
      : h('div', { class: 'empty' }, '⚠ 802.11 deauth injection — authorized use only.'));

    targetList.replaceChildren(
      state.targets && state.targets.length
        ? state.targets.map(t => h('div', { class: 'note', style: 'cursor:pointer',
            onclick: () => { bssidInput.value = t.bssid; chInput.value = t.channel; } },
            h('b', {}, t.ssid || '(hidden)'), ' · ch ', t.channel, ' · ', t.rssi, 'dBm · ', t.auth))
        : h('p', { class: 'muted' }, 'Click "Scan APs" to discover nearby networks.'));
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

  async function scan() {
    scanBtn.disabled = true;
    try {
      await api.deauthfloodScan();
      const t = await api.deauthfloodTargets();
      state.targets = t.targets || [];
      render();
      toast(`Found ${state.targets.length} APs`);
    } catch (e) { toast(errorText(e), 'error'); }
    scanBtn.disabled = false;
  }

  async function load() {
    try { state = await api.deauthflood(); if (!alive) return; render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  let timer = setInterval(() => { if (state.active) load(); }, 1000);
  return { unmount() { alive = false; clearInterval(timer); } };
}