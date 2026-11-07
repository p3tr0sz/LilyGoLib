// FIDO2 credential management; shown only when firmware advertises a real capability.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';
import { toolApp } from '../../js/toolapp.js';

export function mount(el, { api }) {
  let state = { available: false, state: 'unavailable', credentials: 0, credentialItems: [], error: null };
  const rpInput = h('input', { class: 'field', autocomplete: 'off', placeholder: 'example.com', 'aria-label': 'Relying party hostname' });
  const userInput = h('input', { class: 'field', autocomplete: 'off', placeholder: 'alice@example.com', 'aria-label': 'User name' });
  const regBtn = h('button', { class: 'btn primary', type: 'button', onclick: register }, icon('key'), 'Register');
  const authBtn = h('button', { class: 'btn', type: 'button', onclick: authenticate }, icon('lock'), 'Authenticate');
  const info = h('div', {});
  const credList = h('div', { class: 'list' });

  el.append(h('div', { class: 'app fido2' },
    h('div', { class: 'panel' },
      h('h3', {}, 'FIDO2 authenticator'),
      h('p', { class: 'muted' }, 'Registration and authentication require a firmware FIDO2/CTAP transport. Keyboard HID alone is not a security key.')),
    h('div', { class: 'panel' },
      h('div', { class: 'form' },
        h('label', {}, 'Relying party', rpInput),
        h('label', {}, 'User', userInput)),
      h('div', { class: 'row gap' }, regBtn, authBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Status'), info),
    h('div', { class: 'panel' }, h('h3', {}, 'Registered credentials'), credList)));

  function render() {
    regBtn.disabled = app.busy || !state.available;
    authBtn.disabled = app.busy || !state.available;
    info.replaceChildren(
      h('div', { class: 'srow' }, h('span', {}, 'Transport'), h('span', { class: `tag ${state.available ? 'ok' : 'warn'}` }, state.available ? 'AVAILABLE' : 'UNAVAILABLE')),
      h('div', { class: 'srow' }, h('span', {}, 'State'), h('span', {}, state.state || 'unknown')),
      h('div', { class: 'srow' }, h('span', {}, 'Credentials'), h('span', {}, String(state.credentials || 0))),
      state.error ? h('p', { class: 'inline-error', role: 'alert' }, state.error) : null);
    credList.replaceChildren(...((state.credentialItems || []).length
      ? state.credentialItems.map(c => h('div', { class: 'note' },
          h('div', {}, h('b', {}, c.rp || '?'), h('p', {}, c.user || '?')),
          h('button', { class: 'btn sm danger', type: 'button', disabled: app.busy || !state.available, onclick: () => remove(c) }, 'Remove')))
      : [h('p', { class: 'muted' }, 'No credentials registered.')]));
  }

  const app = toolApp({
    name: 'FIDO2',
    read: async () => {
      const [status, creds] = await Promise.all([api.fido2(), api.fido2Credentials()]);
      return { status, creds };
    },
    apply: ({ status, creds }) => { state = { ...state, ...status, credentialItems: creds.credentials || [] }; },
    render,
    onError: (e) => info.replaceChildren(h('p', { class: 'inline-error', role: 'alert' }, errorText(e, 'FIDO2'))),
    interval: 2500,
  });

  function register() {
    if (!rpInput.value.trim() || !userInput.value.trim()) { toast('Enter both relying party and user.', 'error'); return; }
    return app.action(() => api.fido2Register(rpInput.value.trim(), userInput.value.trim()), 'Credential registered');
  }
  function authenticate() {
    if (!rpInput.value.trim()) { toast('Enter a relying party.', 'error'); return; }
    return app.action(() => api.fido2Authenticate(rpInput.value.trim()), 'Authentication accepted');
  }
  async function remove(c) {
    if (!await confirmDialog('Remove credential?', `Remove ${c.user} for ${c.rp}? This cannot be undone.`, 'Remove', true)) return;
    return app.action(() => api.fido2Remove(c.index), 'Credential removed');
  }

  app.start();
  return { unmount: () => app.unmount() };
}
