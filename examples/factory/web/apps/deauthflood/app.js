// Authorized 802.11 deauthentication testing.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

const MAC = /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i;

export function mount(el, { api, caps }) {
  let scanning = false;
  const selectedTargets = new Set();
  const canCampaign = caps.includes('deauth.campaign');
  let state = { active: false, targets: [], sent: 0, txErrors: 0, campaign: {}, progress: [], reconnect: {}, error: null };
  const bssidInput = h('input', { class: 'field', placeholder: 'aa:bb:cc:dd:ee:ff', 'aria-label': 'Target AP BSSID', autocomplete: 'off' });
  const stationInput = h('input', { class: 'field', value: 'ff:ff:ff:ff:ff:ff', 'aria-label': 'Target station MAC; broadcast means all stations', autocomplete: 'off' });
  const channelInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '13', value: '1', 'aria-label': 'Target channel' });
  const rateInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '100', value: '10', 'aria-label': 'Packets per second' });
  const campaignFrames = h('input', { class: 'field w3', type: 'number', min: '1', max: '100000', value: '100', 'aria-label': 'Frames per campaign target' });
  const campaignBtn = canCampaign ? h('button', { class: 'btn danger', type: 'button', onclick: startCampaign }, icon('play'), 'Start selected campaign') : null;
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start test');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.deauthfloodStop(), 'Test stopped; Wi-Fi reconnect requested'), disabled: true }, icon('stop'), 'Stop');
  const scanBtn = h('button', { class: 'btn', type: 'button', onclick: scan }, icon('search'), 'Scan APs');
  const scanStatus = h('p', { class: 'muted' }, 'Scan to choose a network instead of typing its BSSID.');
  const info = h('div', {});
  const targetList = h('div', { class: 'list' });
  const campaignList = h('div', { class: 'list' });

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
    canCampaign ? h('div', { class: 'panel' }, h('h3', {}, 'Finite multi-target campaign'),
      h('p', { class: 'muted' }, 'Click scan rows to select up to 8 authorized targets. Each runs independently to its accepted-frame goal; repeated injection failures skip only that target. WPA3/PMF targets report limited outcomes, never guaranteed disconnection.'),
      h('label', {}, 'Accepted-frame goal per target', campaignFrames), campaignBtn, campaignList) : null,
    h('div', { class: 'panel' }, h('h3', {}, 'Test status'), info)));

  function render() {
    startBtn.disabled = app.busy || state.active;
    stopBtn.disabled = app.busy || !state.active;
    scanBtn.disabled = app.busy || scanning || state.active;
    if (campaignBtn) campaignBtn.disabled = app.busy || state.active || !selectedTargets.size;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'alert' : ''}` }, state.active ? 'INJECTING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Target'), h('span', { class: 'mono' }, state.targetSsid || state.targetBssid || '—')),
      h('div', { class: 'srow' }, h('span', {}, 'BSSID / station'), h('span', { class: 'mono' }, `${state.targetBssid || '—'} / ${state.targetStation || '—'}`)),
      h('div', { class: 'srow' }, h('span', {}, 'Accepted frames'), h('span', {}, String(state.sent || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Injection errors'), h('span', { class: state.txErrors ? 'tag alert' : '' }, String(state.txErrors || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Channel / rate'), h('span', {}, `${state.channel || 1} / ${state.ratePps || 0} pkt/s`)),
      state.reconnect?.attempts ? h('div', { class: 'srow' }, h('span', {}, 'Reconnect'), h('span', {}, `${state.reconnect.reconnected ? 'connected' : state.reconnect.pending ? 'retrying' : 'not connected'} · ${state.reconnect.elapsedMs || 0} ms · ${state.reconnect.attempts} attempt(s)`)) : null,
      state.campaign?.count ? h('div', { class: 'srow' }, h('span', {}, 'Campaign progress'), h('span', {}, `${state.campaign.completed || 0}/${state.campaign.count} complete`)) : null,
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    targetList.replaceChildren(...((state.targets || []).length
      ? state.targets.map(t => h('button', { class: `note${selectedTargets.has(t.bssid.toLowerCase()) ? ' sel' : ''}`, type: 'button', 'aria-pressed': String(selectedTargets.has(t.bssid.toLowerCase())), 'aria-label': `Select ${t.ssid || 'hidden network'} on channel ${t.channel}`, onclick: () => {
          bssidInput.value = t.bssid; channelInput.value = t.channel;
          const key = t.bssid.toLowerCase();
          if (selectedTargets.has(key)) selectedTargets.delete(key);
          else if (selectedTargets.size < 8) selectedTargets.add(key);
          else { toast('Campaigns are limited to 8 targets.', 'error'); return; }
          render();
        } }, h('div', {}, h('b', {}, `${selectedTargets.has(t.bssid.toLowerCase()) ? '✓ ' : ''}${t.ssid || '(hidden)'}`), h('p', {}, `${t.bssid} · ${t.auth}`)), h('span', { class: 'when' }, `ch ${t.channel} · ${t.rssi} dBm`)))
      : [h('p', { class: 'muted' }, 'No scan results.')]));
    campaignList.replaceChildren(...((state.progress || []).length
      ? state.progress.map(t => h('div', { class: 'note' },
          h('div', {}, h('b', {}, t.ssid || t.bssid), h('p', {}, `${t.state} · ${t.accepted || 0}/${t.goal || 0} accepted · ${t.failed || 0} failed${t.pmfLimited ? ' · PMF-protected clients may ignore frames' : ''}`)),
          h('span', { class: 'when' }, `${t.auth || ''} · ch ${t.channel || '?'}`)))
      : [h('p', { class: 'muted' }, 'No campaign progress yet.')]));
  }

  const app = toolApp({
    name: 'Deauth Flood',
    read: () => api.deauthflood(),
    apply: (next) => { state = { ...state, ...next }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Deauth Flood'))),
    interval: 1000,
    pollWhen: () => state.active,
  });

  async function start() {
    const bssid = bssidInput.value.trim(), station = stationInput.value.trim();
    if (!MAC.test(bssid) || !MAC.test(station)) { toast('Enter valid BSSID and station MAC addresses.', 'error'); return; }
    if (!await confirmDialog('Start deauthentication test?', `Inject frames at ${bssid} on channel ${channelInput.value}? Only continue if you own or may disrupt this network. The watch’s normal Wi-Fi will disconnect.`, 'Start test', true)) return;
    return app.action(() => api.deauthfloodStart({ bssid, station, channel: Number(channelInput.value), rate: Number(rateInput.value), authorized: true }), 'Deauthentication test started');
  }
  async function startCampaign() {
    const targets = [...selectedTargets];
    if (!targets.length || targets.length > 8) { toast('Select 1-8 authorized scan targets first.', 'error'); return; }
    if (!await confirmDialog('Start multi-target deauthentication campaign?', `Inject at ${targets.length} selected APs sequentially, up to ${campaignFrames.value} accepted frames each? Confirm every target is yours or explicitly authorized. PMF-protected clients may ignore the frames.`, 'Start campaign', true)) return;
    return app.action(() => api.deauthfloodCampaignStart({ targets, station: stationInput.value.trim(), frames: Number(campaignFrames.value), rate: Number(rateInput.value), authorized: true }), 'Campaign started');
  }
  async function scan() {
    selectedTargets.clear();
    scanning = true; scanStatus.textContent = 'Scanning nearby channels…'; render();
    try {
      await api.deauthfloodScan();
      const deadline = Date.now() + 17000;
      let result;
      do {
        await new Promise(r => setTimeout(r, 400));
        result = await api.deauthfloodTargets();
        if (!app.alive) return;
      } while (result.scanning && Date.now() < deadline);
      if (result.scanning) throw new Error('Scan timed out; try again');
      if (result.error) throw new Error(result.error);
      state.targets = result.targets || [];
      scanStatus.textContent = `${state.targets.length} networks found. Select a row to fill the target.`;
      render();
    } catch (e) { scanStatus.textContent = errorText(e, 'AP scan'); toast(errorText(e, 'AP scan'), 'error'); }
    finally { scanning = false; if (app.alive) render(); }
  }

  app.start();
  return { unmount: () => app.unmount() };
}
