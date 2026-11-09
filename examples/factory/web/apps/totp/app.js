// Authenticator (TOTP): RFC 6238 codes computed on the watch from its own clock. This app lists the
// accounts with a live countdown, adds new ones (fields or a pasted otpauth:// URI, with a preview),
// copies a code and removes an account. Secrets stay on the watch — the browser never stores them.
import { h, icon, toast, errorText, confirmDialog } from '../../js/ui.js';

const groupCode = (c) =>
  c && c.length === 8 ? `${c.slice(0, 4)} ${c.slice(4)}` :
  c && c.length === 6 ? `${c.slice(0, 3)} ${c.slice(3)}` : (c || '––––––');

// Parse an otpauth://totp/Label?secret=…&issuer=…&digits=…&period=… URI into our fields.
function parseOtpauth(uri) {
  try {
    const u = new URL(uri);
    if (u.protocol !== 'otpauth:' || u.host.toLowerCase() !== 'totp') return null;
    const label = decodeURIComponent(u.pathname.replace(/^\//, ''));
    let issuer = u.searchParams.get('issuer') || '';
    let account = label;
    if (label.includes(':')) {
      const i = label.indexOf(':');
      if (!issuer) issuer = label.slice(0, i).trim();
      account = label.slice(i + 1).trim();
    }
    const secret = (u.searchParams.get('secret') || '').trim();
    if (!secret) return null;
    const digits = parseInt(u.searchParams.get('digits') || '6', 10) === 8 ? 8 : 6;
    const period = parseInt(u.searchParams.get('period') || '30', 10) || 30;
    const algorithm = (u.searchParams.get('algorithm') || 'SHA1').toUpperCase();
    return { issuer, account, secret, digits, period, algorithm };
  } catch { return null; }
}

export function mount(el, ctx) {
  const { api, setTitle } = ctx;
  setTitle?.('');
  let alive = true, timer = null;
  let accounts = [];

  const list = h('div', { class: 'totp-list' });

  const fIssuer = h('input', { class: 'field', placeholder: 'Issuer (e.g. GitHub)', 'aria-label': 'Issuer', spellcheck: 'false', autocomplete: 'off' });
  const fAccount = h('input', { class: 'field', placeholder: 'Account (e.g. you@example.com)', 'aria-label': 'Account', spellcheck: 'false', autocomplete: 'off' });
  const fSecret = h('input', { class: 'field', placeholder: 'Base32 secret, or paste an otpauth:// URI', 'aria-label': 'Secret', spellcheck: 'false', autocomplete: 'off' });
  const fDigits = h('select', { class: 'field', 'aria-label': 'Digits', style: { maxWidth: '130px' } }, h('option', { value: '6' }, '6 digits'), h('option', { value: '8' }, '8 digits'));
  const fPeriod = h('input', { class: 'field', type: 'number', min: '10', max: '120', step: '1', value: '30', 'aria-label': 'Period (seconds)', style: { maxWidth: '110px' } });
  const previewEl = h('span', { class: 'muted totp-preview' });
  const btnPreview = h('button', { class: 'btn' }, 'Preview');
  const btnAdd = h('button', { class: 'btn primary' }, icon('save'), h('span', {}, 'Add'));

  fSecret.addEventListener('input', () => {
    const v = fSecret.value.trim();
    if (!v.startsWith('otpauth://')) return;
    const p = parseOtpauth(v);
    if (!p) return;
    if (p.issuer) fIssuer.value = p.issuer;
    if (p.account) fAccount.value = p.account;
    fSecret.value = p.secret;
    fDigits.value = String(p.digits);
    fPeriod.value = String(p.period);
    toast(p.algorithm !== 'SHA1' ? 'Filled from URI — only SHA-1 is supported, using SHA-1' : 'Filled from otpauth URI');
  });

  async function preview() {
    previewEl.textContent = '…';
    try {
      const r = await api.post('/api/totp/preview', { secret: fSecret.value.trim(), digits: +fDigits.value, period: +fPeriod.value });
      previewEl.textContent = `Now: ${groupCode(r.code)} · ${r.remaining_s}s`;
    } catch (e) { previewEl.textContent = errorText(e); }
  }
  btnPreview.onclick = preview;

  async function add() {
    const body = { issuer: fIssuer.value.trim(), account: fAccount.value.trim(), secret: fSecret.value.trim(), digits: +fDigits.value, period: +fPeriod.value };
    if (!body.secret) { toast('Enter a secret', 'error'); return; }
    if (!body.issuer && !body.account) { toast('Enter an issuer or account', 'error'); return; }
    btnAdd.disabled = true;
    try {
      await api.post('/api/totp/add', body);
      fIssuer.value = ''; fAccount.value = ''; fSecret.value = ''; previewEl.textContent = '';
      await load();
      toast('Account added');
    } catch (e) { toast(errorText(e), 'error'); }
    finally { btnAdd.disabled = false; }
  }
  btnAdd.onclick = add;

  async function copyCode(code) {
    try { await navigator.clipboard.writeText(code); toast('Code copied'); }
    catch { toast('Could not copy', 'error'); }
  }

  async function del(a) {
    const name = a.issuer || a.account || 'this account';
    if (!await confirmDialog(`Remove ${name}?`, 'Its secret is deleted from the watch. Make sure you can still sign in another way first.', 'Remove', true)) return;
    try { await api.post('/api/totp/delete', { id: a.id }); await load(); } catch (e) { toast(errorText(e), 'error'); }
  }

  function renderList() {
    if (!accounts.length) { list.replaceChildren(h('div', { class: 'empty' }, 'No accounts yet. Add one below.')); return; }
    list.replaceChildren(...accounts.map((a) => {
      const frac = a.period ? Math.max(0, Math.min(1, a.remaining_s / a.period)) : 0;
      const ring = h('span', { class: 'totp-ring', style: { '--frac': String(frac) }, title: `${a.remaining_s}s left`, role: 'timer' }, h('span', { class: 'totp-ring-n' }, String(a.remaining_s)));
      const codeEl = h('button', { class: `totp-code${a.remaining_s <= 5 ? ' soon' : ''}`, title: 'Copy code', onclick: () => copyCode(a.code) }, groupCode(a.code));
      return h('div', { class: 'totp-card' },
        h('div', { class: 'totp-meta' },
          h('div', { class: 'totp-issuer' }, a.issuer || a.account || '—'),
          a.issuer && a.account ? h('div', { class: 'totp-account muted' }, a.account) : null),
        codeEl, ring,
        h('button', { class: 'btn icon ghost', title: 'Remove', 'aria-label': `Remove ${a.issuer || a.account || 'account'}`, onclick: () => del(a) }, icon('trash')));
    }));
  }

  async function load() {
    try { const r = await api.get('/api/totp'); accounts = r.accounts || []; renderList(); }
    catch (e) { list.replaceChildren(h('div', { class: 'empty' }, errorText(e))); }
  }

  // Codes are computed from the watch's clock, so refetch each second for a live countdown.
  async function tick() {
    clearTimeout(timer);
    if (!alive) return;
    await load();
    if (alive) timer = setTimeout(tick, document.hidden ? 5000 : 1000);
  }

  el.append(h('div', { class: 'app totp' },
    h('div', { class: 'panel' }, h('h3', {}, 'Codes'), list),
    h('div', { class: 'panel' }, h('h3', {}, 'Add account'),
      h('div', { class: 'totp-form' },
        fIssuer, fAccount, fSecret,
        h('div', { class: 'totp-form-row' }, fDigits, fPeriod, btnPreview, previewEl),
        h('div', { class: 'totp-form-row' }, btnAdd)),
      h('p', { class: 'muted totp-note' }, 'Secrets are stored on the watch and codes are computed there — they are never kept in the browser. Removing an account deletes its secret from the watch.'))));

  tick();
  return { unmount() { alive = false; clearTimeout(timer); } };
}
