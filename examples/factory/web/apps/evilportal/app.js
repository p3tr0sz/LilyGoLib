// Authorized captive-portal testing with explicit consent and redacted captures by default.
import { h, icon, toast, errorText, fmtTime, fmtDuration, confirmDialog } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

export function mount(el, { api, caps: deviceCaps }) {
  let mode = 'standalone';
  let state = { active: false, captures: [], templates: [], ssidPresets: [], settings: {}, karma: [], karmaScanning: false, cloning: false, error: null };
  const canTwin = deviceCaps.includes('evil.portal.twin');
  const canKarma = deviceCaps.includes('evil.portal.karma');
  const canCustom = deviceCaps.includes('evil.portal.customTemplates');
  const canClone = deviceCaps.includes('evil.portal.clone');
  const canVerify = deviceCaps.includes('evil.portal.verify');
  const canCoherent = deviceCaps.includes('evil.portal.coherentTwin');

  const standaloneTab = h('button', { class: 'tab sel', type: 'button', role: 'tab', 'aria-selected': 'true', 'data-mode': 'standalone', onclick: () => pickMode('standalone') }, 'Standalone AP');
  const twinTab = canTwin ? h('button', { class: 'tab', type: 'button', role: 'tab', 'aria-selected': 'false', 'data-mode': 'twin', onclick: () => pickMode('twin') }, 'Evil Twin') : null;
  const karmaTab = canKarma ? h('button', { class: 'tab', type: 'button', role: 'tab', 'aria-selected': 'false', 'data-mode': 'karma', onclick: () => pickMode('karma') }, 'KARMA') : null;
  const modeTabs = h('div', { class: 'tabs', role: 'tablist', 'aria-label': 'Portal mode' }, standaloneTab, twinTab, karmaTab);
  const ssidInput = h('input', { class: 'field', maxlength: '32', placeholder: 'Free Wi-Fi', 'aria-label': 'Portal SSID', autocomplete: 'off' });
  const ssidPreset = h('select', { class: 'field', 'aria-label': 'SSID presets', onchange: e => { if (e.target.value) ssidInput.value = e.target.value; } });
  const twinBssid = h('input', { class: 'field', placeholder: 'aa:bb:cc:dd:ee:ff', 'aria-label': 'Target AP BSSID', autocomplete: 'off' });
  const twinChannel = h('input', { class: 'field', type: 'number', min: '1', max: '13', value: '1', 'aria-label': 'Target AP channel' });
  const twinPsk = h('input', { class: 'field', type: 'password', minlength: '8', maxlength: '63', autocomplete: 'off', 'aria-label': 'Authorized lab AP PSK' });
  const coherentCheck = h('input', { type: 'checkbox', 'aria-label': 'Start WPA2-coherent twin', onchange: () => { twinPsk.closest('label').style.display = coherentCheck.checked ? '' : 'none'; } });
  const twinSection = h('div', { class: 'panel' },
    h('h4', {}, 'Evil-twin target'),
    h('p', { class: 'muted' }, 'Open mode creates an open AP. Coherent mode uses the operator-supplied PSK for your authorized WPA2 lab AP; it does not silently downgrade security.'),
    h('div', { class: 'form' }, h('label', {}, 'Target BSSID', twinBssid), h('label', {}, 'Channel', twinChannel)),
    canCoherent ? h('label', { class: 'check' }, coherentCheck, 'WPA2-coherent twin') : null,
    canCoherent ? h('label', { style: 'display:none' }, 'Known lab AP PSK', twinPsk) : null);
  const templatePicker = h('select', { class: 'field', 'aria-label': 'Portal template' });
  const templateFile = h('input', { class: 'field', type: 'file', accept: '.html,text/html', 'aria-label': 'Custom portal HTML file' });
  const uploadTemplateBtn = h('button', { class: 'btn', type: 'button', onclick: uploadTemplate }, 'Upload template');
  const cloneBtn = h('button', { class: 'btn', type: 'button', onclick: clonePage }, 'Clone current SSID');
  const cloneInfo = h('p', { class: 'muted' }, 'No page cloned this session.');
  // Custom SD templates and live cloning are both ways to supply the portal page — grouped
  // into one collapsed panel so the main flow stays short.
  const templateSources = (canCustom || canClone) ? h('details', { class: 'panel' },
    h('summary', {}, 'Template sources (custom / clone)'),
    canCustom ? h('h4', {}, 'Custom SD templates') : null,
    canCustom ? h('p', { class: 'muted' }, 'Self-contained HTML only: a POST /login form with username and password fields. Maximum 64 KiB. Scripts, external resources and invalid paths are rejected. Files persist in /sd/data/evilportal/portals/.') : null,
    canCustom ? templateFile : null,
    canCustom ? uploadTemplateBtn : null,
    canClone ? h('h4', { style: canCustom ? 'margin-top:14px' : '' }, 'Live page cloning') : null,
    canClone ? h('p', { class: 'muted' }, 'Join the authorized open lab SSID and fetch its captive page. Only an already-safe, self-contained POST /login page is saved as cloned.html; active or external content is rejected.') : null,
    canClone ? cloneBtn : null,
    canClone ? cloneInfo : null) : null;
  const gatewayInput = h('input', { class: 'field', value: '192.168.4.1', inputmode: 'decimal', 'aria-label': 'Portal IP address (IPv4)' });
  const ipHint = h('p', { class: 'field-hint' }, 'Clients reach the captive portal at this address. Default 192.168.4.1.');
  const scanSeconds = h('input', { class: 'field', type: 'number', min: '1', max: '120', value: '10', 'aria-label': 'Probe discovery seconds' });
  const scanBtn = h('button', { class: 'btn', type: 'button', onclick: scanKarma }, icon('search'), 'Scan probes');
  const probesEl = h('div', { class: 'list' });
  const karmaSection = canKarma ? h('div', { class: 'panel' },
    h('h4', {}, 'Passive probe discovery'),
    h('p', { class: 'muted' }, 'Listens on channels 1–13 for directed probe requests. KARMA core opens the selected discovered SSID; it does not yet answer every probe dynamically. Wi-Fi may disconnect while scanning.'),
    h('div', { class: 'form' }, h('label', {}, 'Scan seconds', scanSeconds)), scanBtn, probesEl) : null;
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start portal');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: () => app.action(() => api.evilportalStop(), 'Portal or scan stopped; Wi-Fi reconnect requested'), disabled: true }, icon('stop'), 'Stop');
  const clearBtn = h('button', { class: 'btn danger', type: 'button', onclick: clearCaptures }, icon('delete'), 'Clear captures');
  const info = h('div', {});
  const capturesEl = h('div', { class: 'list' });
  const pwdMode = h('select', { class: 'field', 'aria-label': 'Password display mode' },
    h('option', { value: '2' }, 'Hide all'), h('option', { value: '1' }, 'Mask middle characters'),
    h('option', { value: '3' }, 'Show length only'), h('option', { value: '0' }, 'Show full password'));
  const autoBase = h('input', { class: 'field', type: 'number', min: '0', max: '3600', value: '0', 'aria-label': 'Idle auto-stop seconds' });
  const autoExtended = h('input', { class: 'field', type: 'number', min: '0', max: '3600', value: '0', 'aria-label': 'Active auto-stop seconds' });
  const debugCheck = h('input', { type: 'checkbox', 'aria-label': 'Write non-credential diagnostics to SD' });
  const verifyCheck = h('input', { type: 'checkbox', 'aria-label': 'Verify captured PSKs against target', disabled: !canVerify });
  const saveSettingsBtn = h('button', { class: 'btn', type: 'button', onclick: saveSettings }, 'Save settings');

  el.append(h('div', { class: 'app evilportal' },
    h('div', { class: 'panel' },
      h('h3', {}, 'Authorized use only'),
      h('p', { class: 'muted' }, 'Creates a rogue access point and records entered credentials. Use only on networks you own or have explicit permission to test. Captures are sensitive data.')),
    modeTabs,
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'SSID', ssidInput),
        h('label', {}, 'Preset', ssidPreset),
        h('label', {}, 'Portal template', templatePicker),
        h('label', {}, 'Portal IP address', gatewayInput)),
      ipHint),
    twinSection,
    karmaSection,
    templateSources,
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Portal status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Captured credentials'), clearBtn, capturesEl),
    h('details', { class: 'panel' }, h('summary', {}, 'Privacy and runtime settings'),
      h('div', { class: 'form' },
        h('label', {}, 'Password display', pwdMode),
        h('label', {}, 'Idle stop (0 = off)', autoBase),
        h('label', {}, 'Active stop (0 = off)', autoExtended)),
      canVerify ? h('label', { class: 'check' }, verifyCheck, 'Verify captured PSKs against the selected twin BSSID') : null,
      h('label', { class: 'check' }, debugCheck, 'Write diagnostics to SD (no credentials)'), saveSettingsBtn)));

  function pickMode(next) {
    mode = next;
    for (const tab of [standaloneTab, twinTab, karmaTab].filter(Boolean)) {
      const selected = tab.dataset.mode === mode;
      tab.classList.toggle('sel', selected);
      tab.setAttribute('aria-selected', String(selected));
    }
    twinSection.style.display = mode === 'twin' ? '' : 'none';
    if (karmaSection) karmaSection.style.display = mode === 'karma' ? '' : 'none';
  }
  function render() {
    startBtn.disabled = app.busy || state.active || state.karmaScanning || state.cloning || (mode === 'karma' && !(state.karma || []).length);
    stopBtn.disabled = app.busy || (!state.active && !state.karmaScanning && !state.cloning);
    scanBtn.disabled = app.busy || state.active || state.karmaScanning || state.cloning;
    uploadTemplateBtn.disabled = app.busy || state.active || state.cloning;
    cloneBtn.disabled = app.busy || state.active || state.karmaScanning || state.cloning;
    clearBtn.disabled = app.busy || !(state.captures || []).length;
    if (state.active) {
      const modeName = state.mode === 2 ? 'KARMA core' : state.mode === 1 ? (state.coherent ? 'WPA2-coherent twin' : 'Open twin') : 'Standalone';
      const rows = [
        h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: 'tag alert' }, 'ACTIVE')),
        h('div', { class: 'srow' }, h('span', {}, 'Mode / SSID'), h('span', {}, `${modeName} · ${state.ssid || '—'}`)),
        h('div', { class: 'srow' }, h('span', {}, 'Clients'), h('span', {}, String(state.clients || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'Attempts / repeated'), h('span', {}, `${state.attempts || 0} / ${state.confirmed || 0}`)),
        h('div', { class: 'srow' }, h('span', {}, 'DHCP'), h('span', { class: state.dhcpsOk ? 'tag ok' : 'tag alert' }, state.dhcpsOk ? 'RUNNING' : 'CHECK DHCP')),
        h('div', { class: 'srow' }, h('span', {}, 'Uptime'), h('span', {}, fmtDuration(state.started ? Date.now() / 1000 - state.started : 0))),
      ];
      if (state.twinBssid) rows.push(h('div', { class: 'srow' }, h('span', {}, 'Target / accepted deauth'), h('span', { class: 'mono' }, `${state.twinBssid} / ${state.deauthFrames || 0}`)));
      info.replaceChildren(...rows);
    } else if (state.karmaScanning) info.replaceChildren(h('p', { class: 'muted' }, 'Discovering nearby probe requests…'));
    else if (state.cloning) info.replaceChildren(h('p', { class: 'muted' }, 'Joining the open lab AP and fetching its captive page…'));
    else info.replaceChildren(h('p', { class: 'muted' }, 'Portal stopped.'));
    if (state.error) info.append(h('p', { class: 'inline-error', role: 'alert' }, state.error));
    if (state.templateError) info.append(h('p', { class: 'inline-error', role: 'alert' }, state.templateError));
    cloneInfo.textContent = state.cloning ? `Cloning ${state.cloneSsid || 'the selected open lab SSID'}…` : state.cloneName ? `Saved ${state.cloneName} (${state.cloneBytes || 0} bytes). Reloaded in the template picker.` : 'No page cloned this session.';

    capturesEl.replaceChildren(...((state.captures || []).length
      ? state.captures.slice(-20).reverse().map(c => h('div', { class: 'note' },
          h('div', {}, h('b', {}, c.username || '?'), h('p', {}, `${c.password || '?'} · ${c.url || '/login'}${c.verification && c.verification !== 'off' ? ` · PSK ${c.verification}` : ''}`)),
          h('span', { class: 'when' }, fmtTime(c.ts))))
      : [h('p', { class: 'muted' }, 'No credentials captured.')]));

    probesEl.replaceChildren(...((state.karma || []).length
      ? state.karma.map(p => h('button', { class: 'note', type: 'button', onclick: () => { ssidInput.value = p.ssid; pickMode('karma'); } },
          h('div', {}, h('b', {}, p.ssid), h('p', {}, `${p.hits} hits · ${p.clients || 0} clients · channel ${p.channel || '?'} · ${p.rssi || '?'} dBm`)),
          h('span', { class: 'when' }, p.lastClient || '')))
      : [h('p', { class: 'muted' }, state.karmaScanning ? 'Listening for directed probe requests…' : 'No probe SSIDs discovered.')]));

    // Don't rebuild a <select> while the user has it open (the 2 s poll would reset/close it).
    if (document.activeElement !== templatePicker) {
      const selectedTemplate = templatePicker.value;
      templatePicker.replaceChildren(...(state.templates || []).map(t => h('option', { value: String(t.id) }, `${t.name}${t.builtin === false ? ' · SD' : ''}`)));
      if ([...templatePicker.options].some(o => o.value === selectedTemplate)) templatePicker.value = selectedTemplate;
    }
    if (document.activeElement !== ssidPreset) {
      const selectedPreset = ssidPreset.value;
      ssidPreset.replaceChildren(h('option', { value: '' }, 'Choose a preset'), ...(state.ssidPresets || []).map(s => h('option', { value: s }, s)));
      if ([...ssidPreset.options].some(o => o.value === selectedPreset)) ssidPreset.value = selectedPreset;
    }
    const settings = state.settings || {};
    if (document.activeElement !== gatewayInput) gatewayInput.value = settings.gateway || gatewayInput.value;
    pwdMode.value = String(settings.pwdMode ?? 2);
    if (document.activeElement !== autoBase) autoBase.value = String(settings.autoStopBase || 0);
    if (document.activeElement !== autoExtended) autoExtended.value = String(settings.autoStopExtended || 0);
    verifyCheck.checked = !!settings.verify;
    debugCheck.checked = !!settings.debug;
  }

  const app = toolApp({
    name: 'Evil Portal',
    read: async () => {
      const [status, captures, karma] = await Promise.all([
        api.evilportal(), api.evilportalCaptures(), canKarma ? api.evilportalKarma() : Promise.resolve({ probes: [] }),
      ]);
      return { status, captures, karma };
    },
    apply: ({ status, captures, karma }) => {
      state = { ...status, captures: captures.captures || [], karma: karma.probes || [], karmaScanning: status.karmaScanning || karma.scanning };
    },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Evil Portal'))),
    interval: 2000,
    pollWhen: () => state.active || state.karmaScanning || state.cloning,
  });

  async function clonePage() {
    const ssid = ssidInput.value.trim();
    if (!ssid) { toast('Enter the authorized open lab SSID first.', 'error'); return; }
    if (!await confirmDialog('Clone an authorized lab portal?', `Join “${ssid}” and fetch its captive page? Only safe static HTML will be saved to cloned.html on SD. Wi-Fi will reconnect afterwards.`, 'I am authorized — clone')) return;
    return app.action(() => api.evilportalClone(ssid), 'Page cloning started');
  }
  async function start() {
    const ssid = ssidInput.value.trim() || 'Free Wi-Fi';
    if (mode === 'twin' && !twinBssid.value.trim()) { toast('Enter the target AP BSSID.', 'error'); return; }
    const description = mode === 'karma' ? `open the discovered network “${ssid}”` : mode === 'twin' ? `open an evil twin of “${ssid}”` : `open “${ssid}”`;
    if (!await confirmDialog('Start credential-capture portal?', `Start ${description}? Confirm that you own or have written permission to test this network and will protect captured credentials.`, 'I am authorized — start', true)) return;
    const templateId = Number(templatePicker.value) || 0, gateway = gatewayInput.value.trim();
    return app.action(() => {
      if (mode === 'twin') return api.evilportalStartTwin(ssid, twinBssid.value.trim(), Number(twinChannel.value), coherentCheck.checked, templateId, gateway, twinPsk.value);
      if (mode === 'karma') return api.evilportalStartKarma(templateId, gateway, ssid);
      return api.evilportalStart(ssid, templateId, gateway);
    }, 'Portal started');
  }
  async function scanKarma() {
    if (!await confirmDialog('Scan nearby probe requests?', 'Passively collect SSIDs that nearby devices ask for. Confirm this is your authorized test environment. Channel hopping can interrupt the current Wi-Fi connection.', 'I am authorized — scan')) return;
    return app.action(() => api.evilportalKarmaScan(Number(scanSeconds.value)), 'Probe discovery started');
  }
  function uploadTemplate() {
    const file = templateFile.files?.[0];
    if (!file) { toast('Choose an HTML template first.', 'error'); return; }
    if (!/^[A-Za-z0-9_.-]{6,64}$/.test(file.name) || file.name.includes('..') || !file.name.endsWith('.html')) {
      toast('Use a simple .html filename without path segments.', 'error'); return;
    }
    return app.action(async () => {
      await api.write(`/sd/data/evilportal/portals/${file.name}`, file);
      const result = await api.evilportalReloadTemplates();
      if (result.rejected) throw new Error('Template rejected: require a safe, self-contained POST /login form.');
    }, 'Template saved to SD');
  }
  async function clearCaptures() {
    if (!await confirmDialog('Clear captured credentials?', 'Remove all captures from memory and delete the current SD capture log?', 'Clear captures', true)) return;
    return app.action(() => api.evilportalClear(), 'Captures cleared');
  }
  function saveSettings() {
    return app.action(() => api.evilportalSettings({ pwdMode: Number(pwdMode.value), autoStopBase: Number(autoBase.value), autoStopExtended: Number(autoExtended.value), verify: canVerify && verifyCheck.checked, debug: debugCheck.checked }), 'Settings saved');
  }

  pickMode('standalone');
  app.start();
  return { unmount: () => app.unmount() };
}
