// FIDO2: the watch as a USB/BLE FIDO2/U2F authenticator (passkeys / WebAuthn).
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = { available: false, state: 'idle', credentials: 0 };

  const info = h('div', { class: 'tool' });
  const rpInput = h('input', { class: 'field', placeholder: 'Relying Party (e.g., example.com)', 'aria-label': 'Relying Party' });
  const userInput = h('input', { class: 'field', placeholder: 'User ID', 'aria-label': 'User' });
  const regBtn = h('button', { class: 'btn', onclick: doRegister }, 'Register');
  const authBtn = h('button', { class: 'btn', onclick: doAuth }, 'Authenticate');
  const creds = h('div', { class: 'notes' });

  el.append(h('div', { class: 'app fido2' },
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Register / Authenticate'), h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, rpInput, userInput, regBtn, authBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Credentials'), creds)));

  function render() {
    info.replaceChildren(
      h('div', {},
        h('div', { class: 'srow' }, h('span', {}, 'Available'), h('span', {}, state.available ? h('span', { class: 'tag ok' }, 'Ready') : h('span', { class: 'tag' }, 'Not connected'))),
        h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', {}, state.state || 'idle')),
        h('div', { class: 'srow' }, h('span', {}, 'Credentials'), h('span', {}, String(state.credentials)))));
  }

  async function doRegister() {
    const rp = rpInput.value.trim(), user = userInput.value.trim();
    if (!rp || !user) { toast('Enter a relying party and user ID', 'error'); return; }
    regBtn.disabled = true;
    try { await api.fido2Register(rp, user); toast('Credential registered'); loadCreds(); } catch (e) { toast(errorText(e, 'FIDO2'), 'error'); }
    regBtn.disabled = false;
  }

  async function doAuth() {
    const rp = rpInput.value.trim();
    if (!rp) { toast('Enter a relying party', 'error'); return; }
    authBtn.disabled = true;
    try { await api.fido2Authenticate(rp); toast('Authentication successful'); } catch (e) { toast(errorText(e, 'FIDO2'), 'error'); }
    authBtn.disabled = false;
  }

  async function loadCreds() {
    try {
      const c = await api.fido2Credentials();
      if (!alive) return;
      creds.replaceChildren((c.credentials || []).length
        ? c.credentials.map((cr) => h('div', { class: 'srow' },
            h('span', { class: 'mono' }, cr.rp), h('span', {}, cr.user || ''),
            h('button', { class: 'btn small', onclick: () => remove(cr.index) }, icon('delete'))))
        : h('p', { class: 'muted' }, 'No credentials stored.'));
    } catch (e) { if (alive) creds.replaceChildren(h('p', { class: 'error' }, errorText(e))); }
  }

  async function remove(idx) {
    try { await api.fido2Remove(idx); toast('Credential removed'); loadCreds(); } catch (e) { toast(errorText(e), 'error'); }
  }

  async function load() { try { state = await api.fido2(); if (alive) render(); } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); } }
  loadCreds();
  load();
  return { unmount() { alive = false; } };
}