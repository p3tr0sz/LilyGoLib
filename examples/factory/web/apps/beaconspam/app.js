// Authorized fake-beacon injection with generated or custom SSIDs.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

export function mount(el, { api, caps }) {
  const canCampaign = caps.includes('beacon.campaign');
  let state = { active: false, channel: 0, beacons: 0, ssids: 0, txErrors: 0, customSsids: [], campaign: {}, progress: [], reconnect: {}, error: null };
  const mode = h('select', { class: 'field', 'aria-label': 'Beacon source' },
    h('option', { value: 'generated' }, 'Generated SSIDs'), h('option', { value: 'custom' }, 'Custom SSID list'));
  const prefixInput = h('input', { class: 'field', value: 'FreeNet', maxlength: '20', 'aria-label': 'Generated SSID prefix' });
  const countInput = h('input', { class: 'field w3', type: 'number', min: '1', max: '10000', value: '20', 'aria-label': 'Generated SSID count' });
  const channelInput = h('input', { class: 'field w3', type: 'number', min: '0', max: '13', value: '0', 'aria-label': 'Channel; zero hops all channels' });
  const customArea = h('textarea', { class: 'field', rows: 4, placeholder: 'One SSID per line, 1-32 bytes each', 'aria-label': 'Custom SSIDs, one per line' });
  const saveCustomBtn = h('button', { class: 'btn', type: 'button', onclick: setCustom }, 'Save custom list');
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start test');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.beaconspamStop(), 'Beacon test stopped; Wi-Fi reconnect requested'), disabled: true }, icon('stop'), 'Stop');
  const framesPerSsid = h('input', { class: 'field w3', type: 'number', min: '1', max: '10000', value: '10', 'aria-label': 'Beacon frames per SSID' });
  const campaignBtn = canCampaign ? h('button', { class: 'btn danger', type: 'button', onclick: startCampaign }, icon('play'), 'Run finite campaign') : null;
  const campaignList = h('div', { class: 'list' });
  const info = h('div', {});

  el.append(h('div', { class: 'app beaconspam' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Authorized radio test only'),
      h('p', { class: 'muted' }, 'Broadcasts fake networks and can disrupt nearby Wi-Fi use. Starting also drops the watch’s normal Wi-Fi link.')),
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'Source', mode),
        h('label', {}, 'Prefix', prefixInput),
        h('label', {}, 'SSID count', countInput),
        h('label', {}, 'Channel (0 = hop)', channelInput)),
      h('div', { class: 'row gap' }, startBtn, stopBtn)),
    h('details', { class: 'panel' },
      h('summary', {}, 'Custom SSID list'), customArea,
      h('p', { class: 'muted' }, 'Up to 64 entries. Save the list, then choose “Custom SSID list” as the source.'),
      saveCustomBtn),
    canCampaign ? h('div', { class: 'panel' }, h('h3', {}, 'Finite SSID campaign'),
      h('p', { class: 'muted' }, 'Runs each generated or custom SSID to its accepted-frame goal, then stops and reconnects. Injection failures are reported per SSID. Maximum 64 SSIDs.'),
      h('label', {}, 'Accepted-frame goal per SSID', framesPerSsid), campaignBtn, campaignList) : null,
    h('div', { class: 'panel' }, h('h3', {}, 'Test status'), info)));

  function render() {
    startBtn.disabled = app.busy || state.active;
    stopBtn.disabled = app.busy || !state.active;
    saveCustomBtn.disabled = app.busy || state.active;
    if (campaignBtn) campaignBtn.disabled = app.busy || state.active;
    prefixInput.disabled = mode.value !== 'generated';
    countInput.disabled = mode.value !== 'generated';
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: `tag ${state.active ? 'alert' : ''}` }, state.active ? 'INJECTING' : 'STOPPED')),
      h('div', { class: 'srow' }, h('span', {}, 'Source'), h('span', {}, state.mode === 2 ? 'Custom list' : `Generated: ${state.prefix || '—'}`)),
      h('div', { class: 'srow' }, h('span', {}, 'Accepted beacons'), h('span', {}, String(state.beacons || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'Injection errors'), h('span', { class: state.txErrors ? 'tag alert' : '' }, String(state.txErrors || 0))),
      h('div', { class: 'srow' }, h('span', {}, 'SSIDs / channel'), h('span', {}, `${state.ssids || 0} / ${state.channel || 'hop'}`)),
      state.reconnect?.attempts ? h('div', { class: 'srow' }, h('span', {}, 'Reconnect'), h('span', {}, `${state.reconnect.reconnected ? 'connected' : state.reconnect.pending ? 'retrying' : 'not connected'} · ${state.reconnect.elapsedMs || 0} ms · ${state.reconnect.attempts} attempt(s)`)) : null,
      state.campaign?.count ? h('div', { class: 'srow' }, h('span', {}, 'Campaign progress'), h('span', {}, `${state.campaign.completed || 0}/${state.campaign.count} complete`)) : null,
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    campaignList.replaceChildren(...((state.progress || []).length
      ? state.progress.map(t => h('div', { class: 'note' },
          h('div', {}, h('b', {}, t.ssid), h('p', {}, `${t.state} · ${t.accepted || 0}/${t.goal || 0} accepted · ${t.failed || 0} failed`)),
          h('span', { class: 'when' }, `${t.elapsedMs || 0} ms`)))
      : [h('p', { class: 'muted' }, 'No campaign progress yet.')]));
    if (document.activeElement !== customArea) customArea.value = (state.customSsids || []).join('\n');
  }

  const app = toolApp({
    name: 'Beacon Spam',
    read: () => api.beaconspam(),
    apply: (next) => { state = { ...state, ...next }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Beacon Spam'))),
    interval: 1500,
    pollWhen: () => state.active,
  });

  function customLines() {
    return customArea.value.split('\n').map(x => x.trim()).filter(Boolean);
  }
  function setCustom() {
    const lines = customLines();
    if (!lines.length || lines.length > 64 || lines.some(x => new TextEncoder().encode(x).length > 32)) {
      toast('Use 1-64 non-empty SSIDs, each at most 32 UTF-8 bytes.', 'error'); return;
    }
    return app.action(() => api.beaconspamSetSsids(lines), `Saved ${lines.length} custom SSIDs`);
  }
  async function start() {
    if (mode.value === 'custom' && !customLines().length && !(state.customSsids || []).length) {
      toast('Add and save at least one custom SSID first.', 'error'); return;
    }
    if (!await confirmDialog('Start beacon injection?', `Broadcast ${mode.value === 'custom' ? 'custom' : 'generated'} fake networks on ${Number(channelInput.value) ? 'channel ' + channelInput.value : 'all channels'}? Only continue in an environment you control.`, 'Start test', true)) return;
    return app.action(async () => {
      if (mode.value === 'custom' && customArea.value !== (state.customSsids || []).join('\n')) await api.beaconspamSetSsids(customLines());
      await api.beaconspamStart({ mode: mode.value, prefix: prefixInput.value.trim(), count: Number(countInput.value), channel: Number(channelInput.value), authorized: true });
    }, 'Beacon test started');
  }
  async function startCampaign() {
    if (mode.value === 'custom' && !customLines().length && !(state.customSsids || []).length) {
      toast('Add and save at least one custom SSID first.', 'error'); return;
    }
    if (mode.value === 'generated' && Number(countInput.value) > 64) { toast('Campaigns support at most 64 generated SSIDs.', 'error'); return; }
    if (!await confirmDialog('Run finite beacon campaign?', `Inject at every selected SSID until each reaches ${framesPerSsid.value} accepted frames, then stop and reconnect? Only continue in an environment you control.`, 'Start campaign', true)) return;
    return app.action(async () => {
      if (mode.value === 'custom' && customArea.value !== (state.customSsids || []).join('\n')) await api.beaconspamSetSsids(customLines());
      await api.beaconspamCampaignStart({ mode: mode.value, prefix: prefixInput.value.trim(), count: Number(countInput.value), channel: Number(channelInput.value), framesPerSsid: Number(framesPerSsid.value), authorized: true });
    }, 'Beacon campaign started');
  }

  mode.addEventListener('change', render);
  app.start();
  return { unmount: () => app.unmount() };
}
