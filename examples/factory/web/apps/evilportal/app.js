// Authorized captive-portal testing. Unsupported KARMA/cloning/coherent modes are not shown.
import { h, icon, toast, errorText, fmtTime, fmtDuration, confirmDialog } from '../../js/ui.js';

export function mount(el, { api, caps: deviceCaps }) {
  let alive = true, requestSeq = 0, busy = false, mode = 'standalone';
  let state = { active: false, captures: [], templates: [], ssidPresets: [], settings: {}, error: null };
  const canTwin = deviceCaps.includes('evil.portal.twin');

  const standaloneTab = h('button', { class: 'tab sel', type: 'button', role: 'tab', 'aria-selected': 'true', 'data-mode': 'standalone', onclick: () => pickMode('standalone') }, 'Standalone AP');
  const twinTab = canTwin ? h('button', { class: 'tab', type: 'button', role: 'tab', 'aria-selected': 'false', 'data-mode': 'twin', onclick: () => pickMode('twin') }, 'Open Evil Twin') : null;
  const modeTabs = h('div', { class: 'tabs', role: 'tablist', 'aria-label': 'Portal mode' }, standaloneTab, twinTab);
  const ssidInput = h('input', { class: 'field', maxlength: '32', placeholder: 'Free Wi-Fi', 'aria-label': 'Portal SSID', autocomplete: 'off' });
  const ssidPreset = h('select', { class: 'field', 'aria-label': 'SSID presets', onchange: e => { if (e.target.value) ssidInput.value = e.target.value; } });
  const twinBssid = h('input', { class: 'field', placeholder: 'aa:bb:cc:dd:ee:ff', 'aria-label': 'Target AP BSSID', autocomplete: 'off' });
  const twinChannel = h('input', { class: 'field', type: 'number', min: '1', max: '13', value: '1', 'aria-label': 'Target AP channel' });
  const twinSection = h('div', { class: 'panel' },
    h('h4', {}, 'Evil-twin target'),
    h('p', { class: 'muted' }, 'Open twin only. WPA2-coherent cloning and PSK verification are not available in this firmware.'),
    h('div', { class: 'form' }, h('label', {}, 'Target BSSID', twinBssid), h('label', {}, 'Channel', twinChannel)));
  const templatePicker = h('select', { class: 'field', 'aria-label': 'Portal template' });
  const gatewayInput = h('input', { class: 'field', value: '192.168.4.1', inputmode: 'decimal', 'aria-label': 'Captive gateway IPv4 address' });
  const startBtn = h('button', { class: 'btn danger', type: 'button', onclick: start }, icon('play'), 'Start portal');
  const stopBtn = h('button', { class: 'btn', type: 'button', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const clearBtn = h('button', { class: 'btn danger', type: 'button', onclick: clearCaptures }, icon('delete'), 'Clear captures');
  const info = h('div', {});
  const capturesEl = h('div', { class: 'list' });
  const pwdMode = h('select', { class: 'field', 'aria-label': 'Password display mode' },
    h('option', { value: '2' }, 'Hide all'), h('option', { value: '1' }, 'Mask middle characters'),
    h('option', { value: '3' }, 'Show length only'), h('option', { value: '0' }, 'Show full password'));
  const autoBase = h('input', { class: 'field', type: 'number', min: '0', max: '3600', value: '0', 'aria-label': 'Idle auto-stop seconds' });
  const autoExtended = h('input', { class: 'field', type: 'number', min: '0', max: '3600', value: '0', 'aria-label': 'Active auto-stop seconds' });
  const debugCheck = h('input', { type: 'checkbox', 'aria-label': 'Write non-credential diagnostics to SD' });
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
        h('label', {}, 'Gateway IPv4', gatewayInput))),
    twinSection,
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Portal status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Captured credentials'), clearBtn, capturesEl),
    h('details', { class: 'panel' }, h('summary', {}, 'Privacy and runtime settings'),
      h('div', { class: 'form' },
        h('label', {}, 'Password display', pwdMode),
        h('label', {}, 'Idle stop (0 = off)', autoBase),
        h('label', {}, 'Active stop (0 = off)', autoExtended)),
      h('label', { class: 'check' }, debugCheck, 'Write diagnostics to SD (no credentials)'), saveSettingsBtn)));

  function pickMode(next) {
    mode = next;
    for (const tab of [standaloneTab, twinTab].filter(Boolean)) {
      const selected = tab.dataset.mode === mode;
      tab.classList.toggle('sel', selected);
      tab.setAttribute('aria-selected', String(selected));
    }
    twinSection.style.display = mode === 'twin' ? '' : 'none';
  }
  function render() {
    startBtn.disabled = busy || state.active;
    stopBtn.disabled = busy || !state.active;
    clearBtn.disabled = busy || !(state.captures || []).length;
    if (state.active) {
      const rows = [
        h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', { class: 'tag alert' }, 'ACTIVE')),
        h('div', { class: 'srow' }, h('span', {}, 'Mode / SSID'), h('span', {}, `${state.mode === 1 ? 'Open twin' : 'Standalone'} · ${state.ssid || '—'}`)),
        h('div', { class: 'srow' }, h('span', {}, 'Clients'), h('span', {}, String(state.clients || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'Attempts / repeated'), h('span', {}, `${state.attempts || 0} / ${state.confirmed || 0}`)),
        h('div', { class: 'srow' }, h('span', {}, 'DHCP'), h('span', { class: state.dhcpsOk ? 'tag ok' : 'tag alert' }, state.dhcpsOk ? 'RUNNING' : 'CHECK DHCP')),
        h('div', { class: 'srow' }, h('span', {}, 'Uptime'), h('span', {}, fmtDuration(state.started ? Date.now() / 1000 - state.started : 0))),
      ];
      if (state.twinBssid) rows.push(h('div', { class: 'srow' }, h('span', {}, 'Target / accepted deauth'), h('span', { class: 'mono' }, `${state.twinBssid} / ${state.deauthFrames || 0}`)));
      info.replaceChildren(...rows);
    } else info.replaceChildren(h('p', { class: 'muted' }, 'Portal stopped.'));
    if (state.error) info.append(h('p', { class: 'inline-error', role: 'alert' }, state.error));

    capturesEl.replaceChildren(...((state.captures || []).length
      ? state.captures.slice(-20).reverse().map(c => h('div', { class: 'note' },
          h('div', {}, h('b', {}, c.username || '?'), h('p', {}, `${c.password || '?'} · ${c.url || '/login'}`)),
          h('span', { class: 'when' }, fmtTime(c.ts))))
      : [h('p', { class: 'muted' }, 'No credentials captured.')]));

    const selectedTemplate = templatePicker.value;
    templatePicker.replaceChildren(...(state.templates || []).map(t => h('option', { value: String(t.id) }, t.name)));
    if ([...templatePicker.options].some(o => o.value === selectedTemplate)) templatePicker.value = selectedTemplate;
    ssidPreset.replaceChildren(h('option', { value: '' }, 'Choose a preset'), ...(state.ssidPresets || []).map(s => h('option', { value: s }, s)));
    const settings = state.settings || {};
    if (document.activeElement !== gatewayInput) gatewayInput.value = settings.gateway || gatewayInput.value;
    pwdMode.value = String(settings.pwdMode ?? 2);
    if (document.activeElement !== autoBase) autoBase.value = String(settings.autoStopBase || 0);
    if (document.activeElement !== autoExtended) autoExtended.value = String(settings.autoStopExtended || 0);
    debugCheck.checked = !!settings.debug;
  }

  async function load(showError = true) {
    const seq = ++requestSeq;
    try {
      const [status, captures] = await Promise.all([api.evilportal(), api.evilportalCaptures()]);
      if (!alive || seq !== requestSeq) return;
      state = { ...status, captures: captures.captures || [] };
      render();
    } catch (e) { if (alive && seq === requestSeq && showError) info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'Evil Portal'))); }
  }
  async function start() {
    const ssid = ssidInput.value.trim() || 'Free Wi-Fi';
    if (mode === 'twin' && !twinBssid.value.trim()) { toast('Enter the target AP BSSID.', 'error'); return; }
    if (!await confirmDialog('Start credential-capture portal?', `Start ${mode === 'twin' ? 'an open evil twin of' : 'the portal'} “${ssid}”? Confirm that you own or have written permission to test this network and will protect captured credentials.`, 'I am authorized — start', true)) return;
    busy = true; render();
    try {
      const templateId = Number(templatePicker.value) || 0, gateway = gatewayInput.value.trim();
      if (mode === 'twin') await api.evilportalStartTwin(ssid, twinBssid.value.trim(), Number(twinChannel.value), false, templateId, gateway);
      else await api.evilportalStart(ssid, templateId, gateway);
      toast('Portal started'); await load();
    } catch (e) { toast(errorText(e, 'Evil Portal'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function stop() {
    busy = true; render();
    try { await api.evilportalStop(); toast('Portal stopped; Wi-Fi reconnect requested'); await load(); }
    catch (e) { toast(errorText(e, 'Evil Portal'), 'error'); await load(false); }
    finally { busy = false; if (alive) render(); }
  }
  async function clearCaptures() {
    if (!await confirmDialog('Clear captured credentials?', 'Remove all captures from memory and delete the current SD capture log?', 'Clear captures', true)) return;
    busy = true; render();
    try { await api.evilportalClear(); toast('Captures cleared'); await load(); }
    catch (e) { toast(errorText(e, 'Evil Portal'), 'error'); }
    finally { busy = false; if (alive) render(); }
  }
  async function saveSettings() {
    busy = true; render();
    try {
      await api.evilportalSettings({ pwdMode: Number(pwdMode.value), autoStopBase: Number(autoBase.value), autoStopExtended: Number(autoExtended.value), debug: debugCheck.checked });
      toast('Settings saved'); await load();
    } catch (e) { toast(errorText(e, 'Portal settings'), 'error'); }
    finally { busy = false; if (alive) render(); }
  }

  pickMode('standalone');
  const timer = setInterval(() => { if (state.active) load(false); }, 2000);
  load();
  return { unmount() { alive = false; clearInterval(timer); } };
}
