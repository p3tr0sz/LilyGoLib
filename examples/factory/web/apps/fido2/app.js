// FIDO2 Key: USB/BLE security key (passkeys).
import { h, icon, toast, errorText } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api } = ctx;
  let alive = true, state = { active: false, credentials: [] };

  const rpInput = h('input', { class: 'field', placeholder: 'Relying Party (e.g., example.com)', 'aria-label': 'Relying Party' });
  const userInput = h('input', { class: 'field', placeholder: 'User ID (e.g., alice@example.com)', 'aria-label': 'User ID' });
  const regBtn = h('button', { class: 'btn primary', onclick: register }, icon('key'), 'Register');
  const authBtn = h('button', { class: 'btn', onclick: authenticate }, icon('lock'), 'Authenticate');
  const info = h('div', { class: 'tool' });
  const credList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app fido2' },
    h('div', { class: 'panel' },
      h('h4', {}, 'FIDO2 Security Key'),
      h('p', { class: 'muted' }, 'USB/BLE passkey authenticator. Register credentials, then authenticate.'),
      h('div', { class: 'row gap' },
        h('div', { class: 'grow' }, rpInput),
        h('div', { class: 'grow' }, userInput)),
      h('div', { class: 'row gap' }, regBtn, authBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Registered Credentials'), credList),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info)));

  function render() {
    info.replaceChildren(state.active
      ? h('div', {},
          h('div', { class: 'srow' }, h('span', {}, 'Status'), h('span', { class: 'tag ok' }, 'READY')))
      : h('div', { class: 'empty' }, 'FIDO2 security key. Register passkeys for passwordless authentication.'));
    credList.replaceChildren(
      state.credentials && state.credentials.length
        ? state.credentials.map((c, i) => h('div', { class: 'note' },
            h('div', {}, h('b', {}, c.rp || '?'), ' · ', c.user || '?'),
            h('button', { class: 'btn sm', onclick: () => remove(i) }, 'Remove')))
        : h('p', { class: 'muted' }, 'No credentials registered.'));
  }

  async function register() {
    try {
      await api.fido2Register(rpInput.value.trim(), userInput.value.trim());
      toast('Credential registered');
    } catch (e) { toast(errorText(e, 'FIDO2'), 'error'); }
    load();
  }

  async function authenticate() {
    try { await api.fido2Authenticate(rpInput.value.trim()); toast('Authenticated'); } catch (e) { toast(errorText(e, 'FIDO2'), 'error'); }
  }

  async function remove(index) {
    try { await api.fido2Remove(index); toast('Credential removed'); load(); } catch (e) { toast(errorText(e), 'error'); }
  }

  async function load() {
    try {
      state = await api.fido2();
      const c = await api.fido2Credentials();
      state.credentials = c.credentials || [];
      if (!alive) return; render();
    } catch (e) { if (alive) info.replaceChildren(h('div', { class: 'empty error' }, errorText(e))); }
  }

  load();
  return { unmount() { alive = false; } };
}