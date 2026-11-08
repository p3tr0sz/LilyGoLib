// Terminal: a command console over POST /api/term (the same shell the on-device Terminal runs).
// cwd is kept here, in the browser; the firmware dispatcher is stateless.
import { h, icon, errorText } from '../../js/ui.js';

const BANNER = 'T-Watch Ultra terminal. Type "help".';

export function mount(el, ctx) {
  const { api, setTitle } = ctx;
  let cwd = '/sd';
  const history = [];
  let hpos = 0;          // index into history while arrowing; history.length = "new line"
  let busy = false;

  const out = h('div', {
    style: {
      flex: 1, overflow: 'auto', fontFamily: 'var(--mono)', fontSize: '13px', lineHeight: '1.45',
      whiteSpace: 'pre-wrap', wordBreak: 'break-word', padding: '12px', color: 'var(--text)',
    },
  });
  const prompt = h('span', { style: { fontFamily: 'var(--mono)', color: 'var(--accent)', whiteSpace: 'nowrap' } });
  const input = h('input', {
    'aria-label': 'Command', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false',
    style: {
      flex: 1, minWidth: 0, border: 0, outline: 'none', background: 'transparent',
      color: 'var(--text)', fontFamily: 'var(--mono)', fontSize: '13px',
    },
  });
  const row = h('div', {
    style: { display: 'flex', gap: '8px', alignItems: 'center', padding: '10px 12px', borderTop: '1px solid var(--line)' },
  }, prompt, input);

  el.append(h('div', { class: 'app' },
    h('div', {
      class: 'term',
      style: { display: 'flex', flexDirection: 'column', height: '100%', background: 'var(--card)', borderRadius: 'var(--radius)', overflow: 'hidden' },
    }, out, row)));

  const atBottom = () => out.scrollHeight - out.scrollTop - out.clientHeight < 40;
  function write(text, cls) {
    const stick = atBottom();
    const span = h('div', cls ? { style: cls } : {}, text);
    out.append(span);
    if (stick) out.scrollTop = out.scrollHeight;
  }
  function setPrompt() { prompt.textContent = `${cwd} $`; }

  write(BANNER, { color: 'var(--accent)' });
  setPrompt();
  setTitle?.(cwd);

  async function run(line) {
    write(`${cwd} $ ${line}`, { opacity: 0.85 });
    const cmd = line.trim().split(/\s+/)[0];
    if (cmd === 'clear') { out.replaceChildren(); return; }
    busy = true;
    input.disabled = true;
    try {
      const r = await api.term(line, cwd);
      if (r.output) write(r.output.replace(/\n$/, ''));
      if (r.cwd && r.cwd !== cwd) { cwd = r.cwd; setPrompt(); setTitle?.(cwd); }
    } catch (e) {
      write(errorText(e), { color: 'var(--danger)' });
    } finally {
      busy = false;
      input.disabled = false;
      input.focus();
    }
  }

  input.addEventListener('keydown', (e) => {
    if (busy) { e.preventDefault(); return; }
    if (e.key === 'Enter') {
      const line = input.value;
      input.value = '';
      if (line.trim()) { history.push(line); if (history.length > 100) history.shift(); }
      hpos = history.length;
      if (line.trim()) run(line); else write(`${cwd} $`, { opacity: 0.85 });
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (hpos > 0) { hpos--; input.value = history[hpos] || ''; input.setSelectionRange(input.value.length, input.value.length); }
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      if (hpos < history.length) { hpos++; input.value = history[hpos] || ''; }
    } else if (e.key === 'l' && e.ctrlKey) {
      e.preventDefault();
      out.replaceChildren();
    }
  });

  // Focus the input when the user taps anywhere in the console.
  const focusInput = (e) => { if (!window.getSelection()?.toString()) input.focus(); };
  el.querySelector('.term').addEventListener('click', focusInput);
  queueMicrotask(() => input.focus());

  return { unmount() { el.querySelector('.term')?.removeEventListener('click', focusInput); } };
}
