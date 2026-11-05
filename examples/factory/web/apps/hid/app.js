// BadUSB: the watch types into the computer it is plugged into. DuckyScript, over USB HID.
import { h, icon, errorText, notice, toast } from '../../js/ui.js';

const DIR = '/sd/data/badusb';
const CHEAT = 'STRING types text · DELAY ms · DEFAULT_DELAY ms · ENTER/TAB/ESC/GUI/arrows · combos: GUI r, CTRL ALT DEL';
const TEMPLATE = 'REM A harmless demo: opens a text editor and types a line.\nDELAY 500\nSTRING Hello from the T-Watch\nENTER\n';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, st = null, poll = null, armTimer = null, curPath = '';

  const banner = h('div', {});
  const typeField = h('input', { class: 'field', placeholder: 'Type this on the computer…', 'aria-label': 'Text to type' });
  const typeBtn = h('button', { class: 'btn', onclick: typeNow }, icon('usb'), 'Type');
  const picker = h('select', { class: 'field', 'aria-label': 'Script' });
  const editor = h('textarea', { class: 'field mono', rows: '10', spellcheck: 'false', 'aria-label': 'DuckyScript' });
  const runBtn = h('button', { class: 'btn primary', onclick: run }, icon('play'), 'Run');
  const stopBtn = h('button', { class: 'btn danger', onclick: () => api.hidStop().catch(() => {}) }, icon('stop'), 'Stop');
  const saveBtn = h('button', { class: 'btn', onclick: save }, icon('save'), 'Save');
  const newBtn = h('button', { class: 'btn ghost', onclick: () => { curPath = ''; editor.value = TEMPLATE; picker.value = ''; } }, 'New');
  const runStatus = h('span', { class: 'muted' });

  el.append(h('div', { class: 'app hid' },
    banner,
    h('div', { class: 'callout warn' }, 'This types into the computer the watch is plugged into. Use it only on machines you are authorised to use.'),
    h('div', { class: 'panel' }, h('h3', {}, 'Type text'),
      h('div', { class: 'toolbar' }, typeField, typeBtn)),
    h('div', { class: 'panel' }, h('h3', {}, 'Script'),
      h('div', { class: 'toolbar' }, picker, newBtn, saveBtn),
      editor,
      h('div', { class: 'toolbar' }, runBtn, stopBtn, runStatus),
      h('div', { class: 'callout' }, CHEAT))));

  function render() {
    const avail = !!(st && st.available);
    banner.replaceChildren(avail
      ? h('div', { class: 'callout ok' }, 'USB keyboard ready — the computer has the watch as a keyboard.')
      : h('div', { class: 'callout' }, 'Plug the watch into a computer by USB. The keyboard appears once the computer sees it.'));
    const running = st && (st.state === 'armed' || st.state === 'running');
    typeBtn.disabled = !avail || running;
    runBtn.disabled = !avail || running;
    stopBtn.disabled = !running;
    if (!st) runStatus.textContent = '';
    else if (st.state === 'armed') runStatus.textContent = 'Arming… switch to the target window';
    else if (st.state === 'running') runStatus.textContent = `Running${st.lines ? ` · line ${st.line}/${st.lines}` : ''}${st.script ? ` · ${st.script}` : ''}`;
    else runStatus.textContent = 'Idle';
  }

  async function refresh() { try { st = await api.hid(); if (alive) render(); } catch (e) { if (alive) runStatus.textContent = errorText(e, 'USB HID'); } }

  async function loadScripts() {
    try {
      let entries = [];
      try { entries = (await api.list(DIR)).entries || []; }
      catch (e) { if (e.code === 'not_found') { await api.mkdir(DIR); } }
      const names = entries.filter((x) => !x.dir && /\.(txt|duck|ducky)$/i.test(x.name)).map((x) => x.name);
      picker.replaceChildren(h('option', { value: '' }, names.length ? 'Open a script…' : '(no scripts yet)'),
        ...names.map((n) => h('option', { value: `${DIR}/${n}` }, n)));
    } catch (e) { toast(errorText(e, 'scripts'), 'error'); }
  }

  picker.addEventListener('change', async () => {
    const p = picker.value;
    if (!p) return;
    try { editor.value = await api.readText(p); curPath = p; }
    catch (e) { toast(errorText(e, 'the script'), 'error'); }
  });

  async function save() {
    let p = curPath;
    if (!p) {
      const name = prompt('Save script as (in /sd/data/badusb):', 'script.txt');
      if (!name) return;
      p = `${DIR}/${name.replace(/[^\w.() -]/g, '_')}`;
      if (!/\.(txt|duck|ducky)$/i.test(p)) p += '.txt';
    }
    try { await api.mkdir(DIR); await api.writeText(p, editor.value); curPath = p; await loadScripts(); picker.value = p; toast('Saved'); }
    catch (e) { toast(errorText(e, 'the script'), 'error'); }
  }

  async function typeNow() {
    const text = typeField.value;
    if (!text) return;
    try { await api.hidType(text); toast('Typed'); }
    catch (e) { toast(hidError(e), 'error'); }
  }

  async function run() {
    const text = editor.value.trim();
    if (!text) { toast('Nothing to run — write or open a script.', 'error'); return; }
    try {
      await api.hidRun(curPath ? { path: curPath } : { text });
      let n = 2;
      runStatus.textContent = `Arming… ${n}`;
      clearInterval(armTimer);
      armTimer = setInterval(() => { if (--n > 0) runStatus.textContent = `Arming… ${n}`; else clearInterval(armTimer); }, 1000);
    } catch (e) { toast(hidError(e), 'error'); }
  }

  const hidError = (e) => e.code === 'hid_unavailable' ? 'No computer sees the watch as a keyboard yet.' : errorText(e, 'USB HID');

  const off = events.on('hid', (d) => { if (alive) { st = d; render(); } });
  refresh();
  loadScripts();
  editor.value = TEMPLATE;
  poll = setInterval(refresh, 4000);
  return { unmount() { alive = false; clearInterval(poll); clearInterval(armTimer); off(); } };
}
