// Scripts: edit and run .scr files on the watch through /api/script/*. The firmware runs the same
// engine the on-device Scripts app uses (web_tool_script.cpp), so a script behaves identically in
// both places. Editing lives here (a real keyboard); the watch is the runner.
import { h, icon, errorText } from '../../js/ui.js';

const PLACEHOLDER = [
  '# A small sh-like script. Try:',
  '#   ls /sd ; status',
  '#   for d in /sd /flash; do echo "== $d =="; ls "$d"; done',
  '#   n=3; while [ $n -gt 0 ]; do echo $n; n=$((n-1)); done',
].join('\n');

const nameValid = (s) => /^[A-Za-z0-9._-]{1,48}$/.test(s) && !s.includes('..') && s.endsWith('.scr');

export function mount(el, ctx) {
  const { api, setTitle } = ctx;
  let scripts = [];       // [{name,size}]
  let current = '';       // loaded script name, '' = unsaved
  let busy = false;

  // ---- toolbar
  const select = h('select', { 'aria-label': 'Script', style: sel() });
  const nameInput = h('input', {
    'aria-label': 'Script name', placeholder: 'name.scr', autocomplete: 'off', spellcheck: 'false',
    style: { ...fieldBase(), width: '140px' },
  });
  const btnNew = button('New');
  const btnSave = button('save', 'Save', true);
  const btnRun = button('play', 'Run', true);
  const btnDelete = button('delete', 'Delete');
  const status = h('span', { style: { fontSize: '12px', color: 'var(--muted)', alignSelf: 'center', minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' } });

  const toolbar = h('div', {
    style: { display: 'flex', flexWrap: 'wrap', gap: '8px', alignItems: 'center', padding: '10px 12px', borderBottom: '1px solid var(--line)' },
  }, select, btnNew, nameInput, btnSave, btnRun, btnDelete, status);

  // ---- editor
  const editor = h('textarea', {
    'aria-label': 'Script source', spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off',
    placeholder: PLACEHOLDER,
    style: {
      flex: 1, minHeight: '120px', resize: 'none', border: 0, outline: 'none', padding: '12px',
      background: 'var(--card)', color: 'var(--text)', fontFamily: 'var(--mono)', fontSize: '13px',
      lineHeight: '1.5', whiteSpace: 'pre', overflow: 'auto',
    },
  });

  // ---- output
  const out = h('div', {
    class: 'scr-out',
    style: {
      height: '34%', minHeight: '80px', overflow: 'auto', fontFamily: 'var(--mono)', fontSize: '13px',
      lineHeight: '1.45', whiteSpace: 'pre-wrap', wordBreak: 'break-word', padding: '12px',
      borderTop: '1px solid var(--line)', color: 'var(--text)', background: 'var(--bg)',
    },
  }, h('div', { style: { color: 'var(--muted)' } }, 'Output appears here. Run does not need a save — it runs the editor.'));

  el.append(h('div', { class: 'app' },
    h('div', {
      style: { display: 'flex', flexDirection: 'column', height: '100%', background: 'var(--card)', borderRadius: 'var(--radius)', overflow: 'hidden' },
    }, toolbar, editor, out)));

  function setStatus(text, danger) { status.textContent = text || ''; status.style.color = danger ? 'var(--danger)' : 'var(--muted)'; }
  function setBusy(b) { busy = b; [btnNew, btnSave, btnRun, btnDelete, select].forEach((x) => { x.disabled = b; }); }
  function setCurrent(name) { current = name; setTitle?.(name || 'untitled'); nameInput.value = name; }

  function fillSelect() {
    select.replaceChildren();
    select.append(h('option', { value: '' }, scripts.length ? '— choose a script —' : '— no scripts yet —'));
    for (const s of scripts) select.append(h('option', { value: s.name }, s.name));
    select.value = current;
  }

  async function refresh(keep) {
    try {
      const r = await api.scriptList();
      scripts = Array.isArray(r.scripts) ? r.scripts : [];
    } catch (e) {
      scripts = [];
      setStatus(errorText(e), true);
    }
    if (keep !== undefined) current = keep;
    fillSelect();
  }

  async function load(name) {
    if (!name) { setCurrent(''); editor.value = ''; return; }
    setBusy(true);
    try {
      const r = await api.scriptRead(name);
      editor.value = r.source || '';
      setCurrent(name);
      setStatus(`Loaded ${name}`);
    } catch (e) {
      setStatus(errorText(e), true);
    } finally {
      setBusy(false);
    }
  }

  async function save() {
    let name = nameInput.value.trim();
    if (name && !name.includes('.')) name += '.scr';
    if (!nameValid(name)) { setStatus('Name must be letters/digits/._- and end in .scr', true); nameInput.focus(); return; }
    setBusy(true);
    try {
      await api.scriptSave(name, editor.value);
      await refresh(name);
      setCurrent(name);
      setStatus(`Saved ${name}`);
    } catch (e) {
      setStatus(errorText(e), true);
    } finally {
      setBusy(false);
    }
  }

  async function run() {
    setBusy(true);
    setStatus('Running…');
    try {
      const r = await api.scriptRun(editor.value, '/sd');
      out.replaceChildren(h('div', {}, (r.output || '').replace(/\n$/, '') || '(no output)'));
      out.scrollTop = 0;
      setStatus(current ? `Ran ${current}` : 'Ran (unsaved)');
    } catch (e) {
      out.replaceChildren(h('div', { style: { color: 'var(--danger)' } }, errorText(e)));
      setStatus('Run failed', true);
    } finally {
      setBusy(false);
    }
  }

  async function remove() {
    if (!current) { setStatus('Nothing saved to delete', true); return; }
    if (!window.confirm(`Delete ${current}?`)) return;
    setBusy(true);
    try {
      await api.scriptDelete(current);
      const gone = current;
      setCurrent('');
      editor.value = '';
      await refresh('');
      setStatus(`Deleted ${gone}`);
    } catch (e) {
      setStatus(errorText(e), true);
    } finally {
      setBusy(false);
    }
  }

  select.addEventListener('change', () => { if (!busy) load(select.value); });
  btnNew.addEventListener('click', () => { if (busy) return; setCurrent(''); editor.value = ''; select.value = ''; setStatus('New script — type, name it, Save'); editor.focus(); });
  btnSave.addEventListener('click', () => { if (!busy) save(); });
  btnRun.addEventListener('click', () => { if (!busy) run(); });
  btnDelete.addEventListener('click', () => { if (!busy) remove(); });

  setTitle?.('Scripts');
  refresh().then(() => { if (scripts.length) load(scripts[0].name); });

  return { unmount() {} };

  // ---- small style helpers (match the rest of the desktop)
  function fieldBase() {
    return {
      border: '1px solid var(--line)', background: 'var(--bg)', color: 'var(--text)',
      borderRadius: '8px', padding: '6px 8px', fontSize: '13px', fontFamily: 'var(--mono)', outline: 'none',
    };
  }
  function sel() { return { ...fieldBase(), fontFamily: 'inherit', maxWidth: '45%' }; }
  function button(iconOrText, label, primary) {
    const hasIcon = label !== undefined;
    const b = h('button', {
      style: {
        display: 'inline-flex', alignItems: 'center', gap: '6px', cursor: 'pointer',
        border: '1px solid var(--line)', borderRadius: '8px', padding: '6px 10px', fontSize: '13px',
        background: primary ? 'var(--accent)' : 'var(--bg)', color: primary ? '#fff' : 'var(--text)',
      },
    });
    if (hasIcon) { b.append(icon(iconOrText), h('span', {}, label)); } else { b.textContent = iconOrText; }
    return b;
  }
}
