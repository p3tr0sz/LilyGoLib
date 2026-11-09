// Notepad: a plain-text editor over the file API (GET/PUT /api/files/content). Open a text file from
// the watch, edit it with a real keyboard, save it back. Opens on its own, or from the Files app's
// "Edit" button (openApp('notepad', { path })). No firmware routes of its own.
import { h, icon, toast, errorText, promptDialog, confirmDialog, dialog } from '../../js/ui.js';

const TEXT = /\.(txt|md|json|csv|log|ini|cfg|conf|yaml|yml|xml|html|css|js|mjs|gpx|nmea|ir|c|h|cpp|py|sh|scr)$/i;
const ROOTS = [{ id: 'sd', label: 'SD card', cap: 'files' }, { id: 'flash', label: 'Flash', cap: 'files.flash' }];
const MAX = 256 * 1024;                 // largest file this editor loads / saves
const baseName = (p) => p.slice(p.lastIndexOf('/') + 1);
const nameValid = (s) => !!s && !/[\\/:*?"<>|]/.test(s) && s !== '.' && s !== '..' && !s.includes('/');

export function mount(el, ctx) {
  const { api, caps, setTitle } = ctx;
  const roots = ROOTS.filter((r) => caps.includes(r.cap));
  let path = '';            // current file path, '' = unsaved new buffer
  let saved = '';           // last-saved contents, for the dirty check
  let busy = false;

  const pathLbl = h('span', { class: 'np-path muted' }, 'Untitled');
  const btnNew = h('button', { class: 'btn' }, 'New');
  const btnOpen = h('button', { class: 'btn' }, icon('folder'), h('span', {}, 'Open'));
  const btnSave = h('button', { class: 'btn primary' }, icon('save'), h('span', {}, 'Save'));
  const btnSaveAs = h('button', { class: 'btn' }, 'Save as…');
  const editor = h('textarea', {
    class: 'np-editor', 'aria-label': 'Text', spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', autocorrect: 'off',
    placeholder: 'A plain-text editor. Open a file, or start typing and Save.',
  });

  const toolbar = h('div', { class: 'np-bar' }, btnNew, btnOpen, btnSave, btnSaveAs, pathLbl);
  el.append(h('div', { class: 'app np' }, toolbar, editor));

  const dirty = () => editor.value !== saved;
  function refreshTitle() {
    const name = path ? baseName(path) : 'Untitled';
    pathLbl.textContent = (dirty() ? '• ' : '') + (path || 'Untitled');
    pathLbl.title = path;
    setTitle?.((dirty() ? '• ' : '') + name);
  }
  editor.addEventListener('input', refreshTitle);

  async function confirmDiscard() {
    if (!dirty()) return true;
    return confirmDialog('Discard unsaved changes?', `"${path ? baseName(path) : 'Untitled'}" has edits that are not saved.`, 'Discard', true);
  }

  // A modal file/folder browser across both roots. In save mode it offers a filename box and resolves
  // a folder + name; in open mode a click on a text file resolves its path.
  function browse({ title, okLabel, save, suggest = '' }) {
    return dialog((box, close) => {
      let dir = `/${roots[0]?.id || 'sd'}`;
      const rootBar = h('div', { class: 'seg', role: 'tablist' });
      const crumb = h('nav', { class: 'crumbs', 'aria-label': 'Folder' });
      const flist = h('div', { class: 'files-list', style: { maxHeight: '40vh', overflow: 'auto' } });
      const nameInput = save ? h('input', { class: 'field', value: suggest, placeholder: 'file.txt', 'aria-label': 'File name', spellcheck: 'false', autocomplete: 'off' }) : null;
      const okBtn = h('button', { class: 'btn primary', onclick: () => {
        if (!save) return;
        const n = nameInput.value.trim();
        if (!nameValid(n)) { toast('Enter a valid file name', 'error'); return; }
        close(`${dir}/${n}`);
      } }, okLabel);

      async function show(d) {
        dir = d;
        rootBar.replaceChildren(...roots.map((r) => h('button', { class: dir.split('/')[1] === r.id ? 'on' : '', role: 'tab', onclick: () => show(`/${r.id}`) }, r.label)));
        const parts = dir.split('/').filter(Boolean);
        crumb.replaceChildren(...parts.map((p, i) => {
          const dpath = `/${parts.slice(0, i + 1).join('/')}`;
          return h('button', { onclick: () => show(dpath) }, i === 0 ? (roots.find((r) => r.id === p)?.label || p) : p);
        }));
        let entries = [];
        try { entries = (await api.list(dir)).entries || []; } catch (err) { toast(errorText(err), 'error'); }
        const rows = entries
          .filter((e) => e.dir || save || TEXT.test(e.name))
          .map((e) => {
            const p = `${dir}/${e.name}`;
            const act = e.dir ? () => show(p) : save ? () => { nameInput.value = e.name; } : () => close(p);
            return h('div', { class: 'frow', role: 'button', tabindex: '0', onclick: act, onkeydown: (ev) => { if (ev.key === 'Enter') act(); } },
              h('span', { class: `ficon${e.dir ? ' dir' : ''}` }, icon(e.dir ? 'folder' : 'text')), h('span', { class: 'fname' }, e.name));
          });
        flist.replaceChildren(...(rows.length ? rows : [h('div', { class: 'empty' }, save ? 'No files or subfolders' : 'No text files here')]));
      }

      box.append(h('h2', {}, title), rootBar, crumb, flist,
        nameInput ? h('div', { class: 'np-name-row' }, nameInput) : null,
        h('div', { class: 'actions' }, h('button', { class: 'btn', onclick: () => close(null) }, 'Cancel'), save ? okBtn : null));
      show(dir);
    }, { label: title, wide: true });
  }

  async function loadPath(p) {
    busy = true;
    try {
      const text = await api.readText(p, MAX);
      editor.value = text;
      saved = text;
      path = p;
      refreshTitle();
    } catch (e) { toast(errorText(e), 'error'); }
    finally { busy = false; }
  }

  async function doOpen() {
    if (!await confirmDiscard()) return;
    const p = await browse({ title: 'Open a file', okLabel: 'Open', save: false });
    if (p) await loadPath(p);
  }

  async function writeTo(p) {
    if (editor.value.length > MAX) { toast('File is too large to save here', 'error'); return; }
    busy = true;
    try {
      await api.writeText(p, editor.value);
      saved = editor.value;
      path = p;
      refreshTitle();
      toast('Saved');
    } catch (e) { toast(errorText(e), 'error'); }
    finally { busy = false; }
  }

  async function doSave() {
    if (busy) return;
    if (!path) return doSaveAs();
    await writeTo(path);
  }

  async function doSaveAs() {
    const p = await browse({ title: 'Save as', okLabel: 'Save here', save: true, suggest: path ? baseName(path) : 'untitled.txt' });
    if (p) await writeTo(p);
  }

  async function doNew() {
    if (!await confirmDiscard()) return;
    editor.value = '';
    saved = '';
    path = '';
    refreshTitle();
    editor.focus();
  }

  btnNew.onclick = doNew;
  btnOpen.onclick = doOpen;
  btnSave.onclick = doSave;
  btnSaveAs.onclick = doSaveAs;
  editor.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); doSave(); }
  });

  refreshTitle();
  return {
    unmount() {},
    // Opened from the Files app ("Edit") or a deep link: load the given path.
    async open(options) { if (options?.path) await loadPath(options.path); },
  };
}
