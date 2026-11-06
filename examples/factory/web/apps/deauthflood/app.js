// Authorized 802.11 deauthentication testing.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';

const MAC = /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i;

export function mount(el, { api }) {
  let alive = true, requestSeq = 0, busy = false, scanning = false;
  let state = { active: false, targets: [], sent: 0, txErrors: 0, error: null };
  const bssidInput = h('input', { class: 'field', placeholder: 'aa:bb:cc:dd:ee:ff', 'aria-label': 'Target AP BSSID', autocomplete: 'off' });
  const stationInput = h('input', { class: 'field', value: 'ff:ff:ff:ff:ff:ff', 'aria-label': 'Target station MAC; broadcast means all stations', autocomplete: 'off' });
  const channelInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '13', value: '1', 'aria-label': 'Target channel' });
  const rateInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '100', value: '10', 'aria-label': 'Packets per second' });
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start test');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const scanBtn = h('button', { class: 'btn', type: 'button', onclick: scan }, icon('search'), 'Scan APs');
  const scanStatus = h('p', { class: 'muted' }, 'Scan to choose a network instead of typing its BSSID.');
  const info = h('div', {});
  const targetList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app deauthflood' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Authorized disruption test only'),
      h('p', { class: 'muted' }, 'Disconnects clients from the target AP. Starting also drops the watch’s normal Wi-Fi link, so use the on-device Stop control or reconnect through its access point.')),
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'Target AP BSSID', bssidInput),
        h('label', {}, 'Station (broadcast = all)', stationInput),
        h('label', {}, 'Channel', channelInput),
        h('label', {}, 'Packets/second', rateInput)),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Nearby APs'), scanBtn, scanStatus, targetList),
    h('div', { class: 'panel' }, h('h3', {}, 'Test status'), info)));

  function render() {
    startBtn.disabled = busy || state.active;
    stopBtn.disabled = busy || !state.active;
    scanBtn.disabled = busy || scanning || state.active;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'alert' : ''}` }, state.active ? 'INJECTING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Target'), h('span', { class: 'mono' }, state.targetSsid || state.targetBssid || '—')),
      h('div', { class: 'srow' }, h('span', {}, 'BSSID / station'), h('span', { class: 'mono' }, `${state.targetBssid || '—'} / ${state.targetStation || '—'}`)),
      h('div', { class: 'srow' }, h('span', {}, 'Accepted frames'), h('span', {}, String(state.sent || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Injection errors'), h('span', { class: state.txErrors ? 'tag alert' : '' }, String(state.txErrors || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Channel / rate'), h('span', {}, `${state.channel || 1} / ${state.ratePps || 0} pkt/s`)),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    targetList.replaceChildren(...((state.targets || []).length
      ? state.targets.map(t => h('button', { class: 'note', type: 'button', 'aria-label': `Select ${t.ssid || 'hidden network'} on channel ${t.channel}`, onclick: () => {
          bssidInput.value = t.bssid; channelInput.value = t.channel;
        } }, h('div', {}, h('b', {}, t.ssid || '(hidden)'), h('p', {}, `${t.bssid} · ${t.auth}`)), h('span', { class: 'when' }, `ch ${t.channel} · ${t.rssi} dBm`)))
      : [h('p', { class: 'muted' }, 'No scan results.')]));
  }

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const next = await api.deauthflood();
      if (!alive || seq !== requestSeq) return;
      state = { ...state, ...next };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Deauth Flood'))); }
  }
  async function start() {
    const bssid = bssidInput.value.trim(), station = stationInput.value.trim();
    if (!MAC.test(bssid) || !MAC.test(station)) { toast('Enter valid BSSID and station MAC addresses.', 'error'); return; }
    if (!await confirmDialog('Start deauthentication test?', `Inject frames at ${bssid} on channel ${channelInput.value}? Only continue if you own or may disrupt this network. The watch’s normal Wi-Fi will disconnect.`, 'Start test', true)) return;
    busy = true; render();
    try {
      await api.deauthfloodStart({ bssid, station, channel: Number(channelInput.value), rate: Number(rateInput.value), authorized: true });
      toast('Deauthentication test started'); await load();
    } catch (e) { toast(errorText(e, 'Deauth Flood'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function stop() {
    busy = true; render();
    try { await api.deauthfloodStop(); toast('Test stopped; Wi-Fi reconnect requested'); await load(); }
    catch (e) { toast(errorText(e, 'Deauth Flood'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function scan() {
    scanning = true; scanStatus.textContent = 'Scanning nearby channels…'; render();
    try {
      await api.deauthfloodScan();
      const deadline = Date.now() + 17000;
      let result;
      do {
        await new Promise(r => setTimeout(r, 400));
        result = await api.deauthfloodTargets();
        if (!alive) return;
      } while (result.scanning && Date.now() < deadline);
      if (result.scanning) throw new Error('Scan timed out; try again');
      if (result.error) throw new Error(result.error);
      state.targets = result.targets || [];
      scanStatus.textContent = `${state.targets.length} networks found. Select a row to fill the target.`;
      render();
    } catch (e) { scanStatus.textContent = errorText(e, 'AP scan'); toast(errorText(e, 'AP scan'), 'error'); }
    finally { scanning = false; if (alive) render(); }
  }

  const timer = setInterval(() => { if (state.active) load(false); }, 1000);
  render(); load();
  return { unmount() { alive = false; clearInterval(timer); } };
}
