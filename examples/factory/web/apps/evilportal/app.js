// Evil Portal: rogue AP + captive portal for credential harvesting (security audit tool).
// WARNING: Only use on networks you own or have explicit permission to test.
//
// Three modes:
//   standalone — own AP with chosen SSID and portal template
//   evil-twin  — clone a real AP, deauth its clients onto our twin
//   karma      — probe-response lure for nearby devices
import { h, icon, toast, errorText, fmtTime, fmtDuration } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, captures: [], templates: [], ssidPresets: [], settings: {} };

  // ── mode selector ──
  let mode = 'standalone';
  const modeTabs = h('div', { class: 'tabs' },
    h('button', { class: 'tab sel', 'data-mode': 'standalone', onclick: pickMode }, 'Standalone AP'),
    h('button', { class: 'tab', 'data-mode': 'twin', onclick: pickMode }, 'Evil Twin'),
    h('button', { class: 'tab', 'data-mode': 'karma', onclick: pickMode }, 'KARMA'),
  );

  // ── SSID input ──
  const ssidInput = h('input', { class: 'field', placeholder: 'SSID (default: Free Wi-Fi)', 'aria-label': 'SSID' });
  const ssidPreset = h('select', { class: 'field', onchange: (e) => { ssidInput.value = e.target.value; } },
    h('option', {}, '— SSID presets —'),
    ...([]));

  // ── Twin-specific ──
  const twinBssid = h('input', { class: 'field', placeholder: 'Target BSSID (aa:bb:cc:dd:ee:ff)', 'aria-label': 'Target BSSID' });
  const twinChannel = h('input', { class: 'field', type: 'number', min: 1, max: 14, value: 1, 'aria-label': 'Channel' });
  const twinCoherent = h('label', { class: 'check' }, h('input', { type: 'checkbox' }), ' Coherent (match WPA2 — no downgrade)');
  const twinSection = h('div', { class: 'card' },
    h('h4', {}, 'Evil-Twin Target'),
    h('div', { class: 'row gap' }, h('div', { class: 'grow' }, twinBssid), h('div', { style: 'width:80px' }, twinChannel)),
    twinCoherent);

  // ── Template picker ──
  const templatePicker = h('select', { class: 'field' }, h('option', { value: '0' }, 'Default (Wi-Fi Login)'));

  // ── Gateway ──
  const gatewayInput = h('input', { class: 'field', value: '192.168.4.1', placeholder: 'Gateway IP', 'aria-label': 'Gateway IP' });

  // ── Settings ──
  const pwdModePicker = h('select', { class: 'field' },
    h('option', { value: '0' }, 'Show full password'),
    h('option', { value: '1' }, 'Mask middle chars'),
    h('option', { value: '2' }, 'Hide all'),
    h('option', { value: '3' }, 'Show length only'));
  const verifyCheck = h('label', { class: 'check' }, h('input', { type: 'checkbox' }), ' Verify password against real AP (twin only)');

  // ── Buttons ──
  const startBtn = h('button', { class: 'btn primary', onclick: start }, icon('play'), 'Start Portal');
  const stopBtn = h('button', { class: 'btn', onclick: stop, disabled: true }, icon('stop'), 'Stop');
  const clearBtn = h('button', { class: 'btn', onclick: clearCap }, icon('delete'), 'Clear Captures');

  // ── KARMA ──
  const karmaScanBtn = h('button', { class: 'btn', onclick: karmaScan }, icon('search'), 'Scan for Probes');
  const karmaList = h('div', { class: 'notes' });

  // ── Status panel ──
  const info = h('div', { class: 'tool' });

  // ── Captures panel ──
  const caps = h('div', { class: 'notes' });

  // ── Clone ──
  const cloneInput = h('input', { class: 'field', placeholder: 'Open network SSID to clone', 'aria-label': 'Clone SSID' });
  const cloneBtn = h('button', { class: 'btn', onclick: clonePage }, icon('copy'), 'Clone Page');

  el.append(h('div', { class: 'app evilportal' },
    h('div', { class: 'panel' },
      h('h3', {}, '⚠ Authorized Use Only'),
      h('p', { class: 'muted' }, 'This tool creates a rogue access point to capture credentials. Only use on networks you OWN or have explicit permission to test. Unauthorized use is illegal in most jurisdictions.')),
    modeTabs,
    h('div', { class: 'panel' },
      h('h4', {}, 'Configuration'),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' },
          h('label', { class: 'sub' }, 'SSID'), ssidInput, ssidPreset)),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' }, h('label', { class: 'sub' }, 'Portal Template'), templatePicker),
        h('div', { style: 'width:160px' }, h('label', { class: 'sub' }, 'Gateway IP'), gatewayInput))),
    twinSection,
    h('div', { class: 'row gap' }, startBtn, stopBtn),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' },
      h('h3', {}, 'Captured Credentials', ' ', clearBtn), caps),
    h('details', { class: 'panel' },
      h('summary', {}, 'Settings'),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' }, h('label', { class: 'sub' }, 'Password Display'), pwdModePicker)),
      verifyCheck),
    h('details', { class: 'panel' },
      h('summary', {}, 'KARMA Probe Scanner'),
      h('div', { class: 'row gap' }, karmaScanBtn),
      karmaList),
    h('details', { class: 'panel' },
      h('summary', {}, 'Live Page Cloning'),
      h('p', { class: 'muted' }, 'Connect to an open network and clone its captive-portal login page as a new template.'),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' }, cloneInput), cloneBtn))));

  // ── Functions ──

  function pickMode(e) {
    mode = e.target.dataset.mode;
    modeTabs.querySelectorAll('.tab').forEach(t => t.classList.toggle('sel', t.dataset.mode === mode));
    twinSection.style.display = mode === 'twin' ? '' : 'none';
    render();
  }

  function render() {
    // Mode tabs
    modeTabs.querySelectorAll('.tab').forEach(t => t.classList.toggle('sel', t.dataset.mode === mode));
    twinSection.style.display = mode === 'twin' ? '' : 'none';

    startBtn.disabled = state.active;
    stopBtn.disabled = !state.active;

    // Status
    if (state.active) {
      let rows = [
        h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag alert' }, 'ACTIVE')),
        h('div', { class: 'srow' }, h('span', {}, 'Mode'), h('span', {}, ['standalone', 'twin', 'karma'][state.mode || 0] || '?')),
        h('div', { class: 'srow' }, h('span', {}, 'SSID'), h('span', { class: 'mono' }, state.ssid || '—')),
      ];
      if (state.twinBssid) rows.push(h('div', { class: 'srow' }, h('span', {}, 'Twin BSSID'), h('span', { class: 'mono' }, state.twinBssid)));
      rows.push(
        h('div', { class: 'srow' }, h('span', {}, 'Clients'), h('span', {}, String(state.clients || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'Attempts'), h('span', {}, String(state.attempts || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'Confirmed'), h('span', {}, String(state.confirmed || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'Probes'), h('span', {}, String(state.probesTotal || 0))),
        h('div', { class: 'srow' }, h('span', {}, 'DHCP'), h('span', { class: state.dhcpsOk !== false ? 'tag ok' : 'tag err' }, state.dhcpsOk !== false ? 'OK' : 'FAIL')),
        h('div', { class: 'srow' }, h('span', {}, 'Uptime'), h('span', {}, fmtDuration(state.started ? (Date.now()/1000 - state.started) : 0))),
      );
      if (state.deauthFrames > 0) rows.push(h('div', { class: 'srow' }, h('span', {}, 'Deauths'), h('span', {}, String(state.deauthFrames))));
      if (state.vendors) {
        const vd = [];
        for (const [k, v] of Object.entries(state.vendors)) { if (v > 0) vd.push(`${k}:${v}`); }
        if (vd.length) rows.push(h('div', { class: 'srow' }, h('span', {}, 'Clients'), h('span', {}, vd.join(', '))));
      }
      info.replaceChildren(...rows);
    } else {
      info.replaceChildren(h('div', { class: 'empty' }, '⚠ Security audit tool. Only use on authorized networks.'));
    }

    // Captures
    caps.replaceChildren(state.captures && state.captures.length
      ? state.captures.slice(-20).reverse().map((c) => h('div', { class: 'note' },
          h('div', {}, h('b', {}, c.username || '?'), ' : ', h('code', {}, c.password || '?')),
          h('div', { class: 'sub' }, c.url || '', ' · ', c.mac ? `${c.mac} · ` : '', fmtTime(c.ts))))
      : h('p', { class: 'muted' }, 'No credentials captured yet.'));

    // Templates
    if (state.templates) {
      templatePicker.innerHTML = '';
      state.templates.forEach((t, i) => {
        templatePicker.append(h('option', { value: String(i) }, t.name));
      });
    }

    // SSID presets
    if (state.ssidPresets) {
      ssidPreset.innerHTML = '';
      ssidPreset.append(h('option', {}, '— SSID presets —'));
      state.ssidPresets.forEach(s => ssidPreset.append(h('option', { value: s }, s)));
    }
  }

  async function start() {
    const ssid = ssidInput.value.trim() || 'Free Wi-Fi';
    const templateId = parseInt(templatePicker.value) || 0;
    const gateway = gatewayInput.value.trim() || '192.168.4.1';

    try {
      if (mode === 'twin') {
        const bssid = twinBssid.value.trim();
        const channel = parseInt(twinChannel.value) || 1;
        const coherent = twinCoherent.querySelector('input').checked;
        if (!bssid) { toast('BSSID required for evil-twin mode', 'error'); return; }
        state = await api.evilportalStartTwin(ssid, bssid, channel, coherent, templateId, gateway);
      } else if (mode === 'karma') {
        state = await api.evilportalStartKarma(templateId, gateway);
      } else {
        state = await api.evilportalStart(ssid, templateId, gateway);
      }
      toast('Portal active');
    } catch (e) { toast(errorText(e, 'Portal'), 'error'); }
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

  async function karmaScan() {
    karmaScanBtn.disabled = true;
    try {
      await api.evilportalKarmaScan(30);
      const k = await api.evilportalKarma();
      karmaList.replaceChildren(
        k.probes && k.probes.length
          ? k.probes.map(p => h('div', { class: 'note', style: 'cursor:pointer', onclick: () => { ssidInput.value = p.ssid; mode = 'karma'; render(); } },
              h('b', {}, p.ssid), ' (', p.hits, ' hits)'))
          : h('p', { class: 'muted' }, 'No probes found.')
      );
    } catch (e) { toast(errorText(e), 'error'); }
    karmaScanBtn.disabled = false;
  }

  async function clonePage() {
    const ssid = cloneInput.value.trim();
    if (!ssid) { toast('SSID required', 'error'); return; }
    cloneBtn.disabled = true;
    try {
      await api.evilportalClone(ssid);
      toast('Page cloned! Refresh to see it in template list.');
    } catch (e) { toast(errorText(e), 'error'); }
    cloneBtn.disabled = false;
  }

  load();

  // Auto-refresh while active
  let timer = setInterval(() => { if (state.active) load(); }, 2000);

  return {
    unmount() { alive = false; clearInterval(timer); }
  };
}