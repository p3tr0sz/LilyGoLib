// Files: browse, upload, download, rename, move, duplicate (copy), delete and preview files on the SD card and the flash.
import { h, icon, fmtBytes, fmtDate, toast, progressToast, confirmDialog, promptDialog, dialog } from '../../js/ui.js';

const TEXT = /\.(txt|md|json|csv|log|ini|cfg|conf|yaml|yml|xml|html|css|js|mjs|gpx|nmea|ir|c|h|cpp|py|sh)$/i;
const IMAGE = /\.(png|jpe?g|gif|webp|svg|bmp|ico)$/i;
const AUDIO = /\.(wav|mp3|ogg|opus|m4a|aac|flac)$/i;
const VIDEO = /\.(mp4|webm|mov)$/i;
const ROOTS = [
  { id: 'sd', label: 'SD card', cap: 'files' },
  { id: 'flash', label: 'Flash', cap: 'files.flash' },
];

const kindIcon = (e) => e.dir ? 'folder' : IMAGE.test(e.name) ? 'image' : AUDIO.test(e.name) ? 'audio' : TEXT.test(e.name) ? 'text' : 'file';
const join = (dir, name) => (dir.endsWith('/') ? dir : `${dir}/`) + name;
const parent = (p) => p.slice(0, p.lastIndexOf('/')) || p;
const badName = (n) => !n ? 'Enter a name' : /[\\/:*?"<>|]/.test(n) ? 'A name cannot contain \\ / : * ? " < > |' : n === '.' || n === '..' ? 'Not a valid name' : null;

export function mount(el, ctx) {
  const { api, events, caps } = ctx;
  const roots = ROOTS.filter((r) => caps.includes(r.cap));
  let cwd = `/${roots[0]?.id || 'sd'}`;
  let entries = [];
  let loading = 0;

  const seg = h('div', { class: 'seg', role: 'tablist' });
  const crumbs = h('nav', { class: 'crumbs', 'aria-label': 'Folder' });
  const fileInput = h('input', { type: 'file', multiple: true, class: 'hidden' });
  const list = h('div', { class: 'files-list' });
  const toolbar = h('div', { class: 'toolbar' },
    seg,
    h('button', { class: 'btn icon', title: 'Up one folder', 'aria-label': 'Up one folder', onclick: () => { if (cwd.split('/').length > 2) go(parent(cwd)); } }, icon('up')),
    crumbs,
    h('button', { class: 'btn icon', title: 'Refresh', 'aria-label': 'Refresh', onclick: () => load() }, icon('refresh')),
    h('button', { class: 'btn icon', title: 'New folder', 'aria-label': 'New folder', onclick: newFolder }, icon('newFolder')),
    h('button', { class: 'btn primary upload-btn', 'aria-label': 'Upload', onclick: () => fileInput.click() }, icon('upload'), h('span', { class: 'lbl' }, 'Upload')),
    fileInput);
  el.append(h('div', { class: 'app' }, toolbar, list));

  function renderRoots() {
    seg.replaceChildren(...roots.map((r) => h('button', {
      class: cwd.split('/')[1] === r.id ? 'on' : '', role: 'tab', onclick: () => go(`/${r.id}`),
    }, r.label)));
  }

  function renderCrumbs() {
    const parts = cwd.split('/').filter(Boolean);
    crumbs.replaceChildren(...parts.map((p, i) => {
      const path = `/${parts.slice(0, i + 1).join('/')}`;
      const label = i === 0 ? (roots.find((r) => r.id === p)?.label || p) : p;
      return h('button', { onclick: () => go(path) }, label);
    }));
    ctx.setTitle(parts.length > 1 ? parts[parts.length - 1] : '');
  }

  function go(path) { cwd = path; return load(); }

  async function openTarget(target) {
    const path = target?.path || '';
    if (!roots.some((r) => path === `/${r.id}` || path.startsWith(`/${r.id}/`))) return;
    const dir = target.dir ? path : parent(path);
    cwd = dir;
    await load();
    if (!target.preview) return;
    const name = path.split('/').pop();
    const entry = entries.find((e) => !e.dir && e.name === name);
    if (entry) await preview(entry, path);
    else toast(`File not found: ${name}`, 'error');
  }

  async function load() {
    renderRoots();
    renderCrumbs();
    const mine = ++loading;
    try {
      const r = await api.list(cwd);
      if (mine !== loading) return;
      entries = r.entries || [];
      renderList();
    } catch (e) {
      if (mine !== loading) return;
      entries = [];
      if (e.code === 'sd_missing') renderSdMissing();
      else if (e.status === 404 && cwd.split('/').length > 2) go(parent(cwd));
      else list.replaceChildren(h('div', { class: 'empty' }, e.message));
    }
  }

  async function renderSdMissing() {
    let sd = null;
    try { sd = await api.sd(); } catch {}
    const msg = sd?.present ? 'The SD card is inserted but not mounted.' : 'No SD card in the watch.';
    const hint = sd?.error ? 'The watch could not read it. Use a FAT32 card up to 32 GB.' : sd?.present ? '' : 'Insert a FAT32 card up to 32 GB.';
    list.replaceChildren(h('div', { class: 'empty' }, icon('sd'), h('div', {}, msg), hint ? h('div', { class: 'muted' }, hint) : null,
      sd?.present ? h('div', { style: { marginTop: '14px' } }, h('button', {
        class: 'btn primary',
        onclick: async () => { try { await api.sdMount(); load(); } catch (e) { toast(e.message, 'error'); } },
      }, 'Mount')) : null));
  }

  function renderList() {
    if (!entries.length) {
      list.replaceChildren(h('div', { class: 'empty' }, icon('folder'), 'This folder is empty. Drop files here to upload.'));
      return;
    }
    const head = h('div', { class: 'frow head' }, h('span'), h('span', {}, 'Name'), h('span', { class: 'fsize' }, 'Size'), h('span', { class: 'fdate' }, 'Modified'), h('span'));
    list.replaceChildren(head, ...entries.map(row));
  }

  function row(e) {
    const path = join(cwd, e.name);
    const act = (name, label, fn) => h('button', {
      class: `btn icon ghost${name === 'more' ? ' more' : ''}`, title: label, 'aria-label': `${label} ${e.name}`,
      onclick: (ev) => { ev.stopPropagation(); fn(); },
    }, icon(name));
    return h('div', {
      class: 'frow', tabindex: '0', role: 'button',
      onclick: () => (e.dir ? go(path) : preview(e, path)),
      onkeydown: (ev) => { if (ev.key === 'Enter') (e.dir ? go(path) : preview(e, path)); if (ev.key === 'Delete') del(e, path); },
    },
    h('span', { class: `ficon${e.dir ? ' dir' : ''}` }, icon(kindIcon(e))),
    h('span', { class: 'fname', title: e.name }, e.name),
    h('span', { class: 'fsize' }, e.dir ? '' : fmtBytes(e.size)),
    h('span', { class: 'fdate' }, fmtDate(e.mtime)),
    h('span', { class: 'factions' },
      e.dir ? null : act('download', 'Download', () => download(path, e.name)),
      act('rename', 'Rename', () => rename(e, path)),
      act('trash', 'Delete', () => del(e, path)),
      act('more', 'More', () => more(e, path))));
  }

  function download(path, name) {
    const a = h('a', { href: api.contentUrl(path), download: name });
    document.body.append(a);
    a.click();
    a.remove();
  }

  async function rename(e, path) {
    const name = await promptDialog(`Rename ${e.dir ? 'folder' : 'file'}`, e.name, 'Rename',
      (v) => badName(v) || (v !== e.name && entries.some((x) => x.name === v) ? 'That name is taken here' : null));
    if (!name || name === e.name) return;
    try { await api.move(path, join(cwd, name)); load(); } catch (err) { toast(err.message, 'error'); }
  }

  async function del(e, path) {
    const ok = await confirmDialog(`Delete "${e.name}"?`, e.dir ? 'The folder and everything in it will be deleted from the watch.' : 'The file will be deleted from the watch.', 'Delete', true);
    if (!ok) return;
    try { await api.remove(path, e.dir); load(); } catch (err) { toast(err.message, 'error'); }
  }

  function suggestCopyName(name) {
    const dot = name.lastIndexOf('.');
    const stem = dot > 0 ? name.slice(0, dot) : name;
    const ext = dot > 0 ? name.slice(dot) : '';
    let base = `${stem} copy${ext}`;
    for (let n = 2; entries.some((x) => x.name === base); n++) base = `${stem} copy ${n}${ext}`;
    return base;
  }

  async function duplicate(e, path) {
    const name = await promptDialog(`Duplicate ${e.dir ? 'folder' : 'file'}`, suggestCopyName(e.name), 'Duplicate',
      (v) => badName(v) || (entries.some((x) => x.name === v) ? 'That name is taken here' : null));
    if (!name) return;
    try { await api.copy(path, join(cwd, name)); load(); } catch (err) { toast(err.message, 'error'); }
  }

  // A modal folder browser (roots + subfolders). Resolves the chosen folder path, or null.
  // `disable` marks a folder (and its subtree) that cannot be chosen — the source of a move/copy.
  function pickFolder({ title, okLabel, start, disable = null }) {
    const under = (p, base) => p === base || p.startsWith(`${base}/`);
    return dialog((box, close) => {
      let dir = start;
      const rootBar = h('div', { class: 'seg', role: 'tablist' });
      const crumb = h('nav', { class: 'crumbs', 'aria-label': 'Destination folder' });
      const flist = h('div', { class: 'files-list', style: { maxHeight: '42vh', overflow: 'auto' } });
      const okBtn = h('button', { class: 'btn primary', onclick: () => close(dir) }, okLabel);

      async function show(d) {
        dir = d;
        rootBar.replaceChildren(...roots.map((r) => h('button', {
          class: dir.split('/')[1] === r.id ? 'on' : '', role: 'tab', onclick: () => show(`/${r.id}`),
        }, r.label)));
        const parts = dir.split('/').filter(Boolean);
        crumb.replaceChildren(...parts.map((p, i) => {
          const dpath = `/${parts.slice(0, i + 1).join('/')}`;
          return h('button', { onclick: () => show(dpath) }, i === 0 ? (roots.find((r) => r.id === p)?.label || p) : p);
        }));
        const blocked = disable && under(dir, disable);
        okBtn.disabled = !!blocked;
        okBtn.title = blocked ? 'Cannot use this folder' : '';
        let subs = [];
        try { subs = ((await api.list(dir)).entries || []).filter((x) => x.dir); } catch (err) { toast(err.message, 'error'); }
        const rows = subs.map((x) => {
          const p = join(dir, x.name);
          const dis = disable && under(p, disable);
          return h('div', {
            class: `frow${dis ? ' muted' : ''}`, role: 'button', tabindex: '0',
            onclick: () => show(p), onkeydown: (ev) => { if (ev.key === 'Enter') show(p); },
          }, h('span', { class: 'ficon dir' }, icon('folder')), h('span', { class: 'fname' }, x.name));
        });
        flist.replaceChildren(...(rows.length ? rows : [h('div', { class: 'empty' }, 'No subfolders here')]));
      }

      box.append(h('h2', {}, title), rootBar, crumb, flist,
        h('div', { class: 'actions' },
          h('button', { class: 'btn', onclick: () => close(null) }, 'Cancel'),
          okBtn));
      show(start);
    }, { label: title, wide: true });
  }

  async function transfer(e, path, mode) {       // mode: 'move' | 'copy'
    const here = parent(path);
    const dest = await pickFolder({
      title: `${mode === 'move' ? 'Move' : 'Copy'} "${e.name}" to…`,
      okLabel: mode === 'move' ? 'Move here' : 'Copy here',
      start: here,
      disable: e.dir ? path : null,             // a folder cannot go into itself or its subtree
    });
    if (!dest) return;
    if (dest === here) { toast(mode === 'move' ? 'Already in that folder' : 'Use Duplicate to copy within a folder'); return; }
    const to = join(dest, e.name);
    const run = (overwrite) => (mode === 'move' ? api.move(path, to, overwrite) : api.copy(path, to, overwrite));
    try {
      await run(false);
      load();
    } catch (err) {
      if (err.status !== 409) { toast(err.message, 'error'); return; }
      const ok = await confirmDialog(`"${e.name}" already exists there`, 'Replace it?', 'Replace', true);
      if (!ok) return;
      try { await run(true); load(); } catch (err2) { toast(err2.message, 'error'); }
    }
  }

  async function more(e, path) {
    await dialog((box, close) => {
      const b = (label, fn, cls = '') => h('button', { class: `btn ${cls}`, style: { width: '100%' }, onclick: () => { close(); fn(); } }, label);
      box.append(h('h2', {}, e.name),
        h('div', { style: { display: 'grid', gap: '8px' } },
          e.dir ? b('Open', () => go(path)) : b('Preview', () => preview(e, path)),
          e.dir ? null : b('Download', () => download(path, e.name)),
          b('Rename', () => rename(e, path)),
          b('Duplicate', () => duplicate(e, path)),
          b('Move to…', () => transfer(e, path, 'move')),
          b('Copy to…', () => transfer(e, path, 'copy')),
          b('Delete', () => del(e, path), 'danger'),
          b('Cancel', () => {}, 'ghost')));
    });
  }

  async function newFolder() {
    const name = await promptDialog('New folder', 'New folder', 'Create',
      (v) => badName(v) || (entries.some((x) => x.name === v) ? 'That name is taken here' : null));
    if (!name) return;
    try { await api.mkdir(join(cwd, name)); load(); } catch (e) { toast(e.message, 'error'); }
  }

  async function upload(files) {
    const taken = files.filter((f) => entries.some((x) => x.name === f.name && !x.dir));
    if (taken.length && !await confirmDialog(
      taken.length === 1 ? `Replace "${taken[0].name}"?` : `Replace ${taken.length} files?`,
      'A file with the same name is already in this folder.', 'Replace', true)) {
      files = files.filter((f) => !taken.includes(f));
    }
    for (const f of files) {
      const t = progressToast(`Uploading ${f.name}`);
      try {
        await api.write(join(cwd, f.name), f, (p) => t.set(p));
        t.done(`Uploaded ${f.name}`);
      } catch (e) {
        t.done(`${f.name}: ${e.code === 'sd_missing' ? 'no SD card' : e.message}`, 'error');
      }
    }
    load();
  }

  async function preview(e, path) {
    const url = api.contentUrl(path);
    let content;
    if (IMAGE.test(e.name)) content = h('img', { src: url, alt: e.name });
    else if (AUDIO.test(e.name)) content = h('audio', { src: url, controls: true, autoplay: true });
    else if (VIDEO.test(e.name)) content = h('video', { src: url, controls: true });
    else if (TEXT.test(e.name) || e.size <= 64 * 1024) {
      try {
        let text = await api.readText(path);
        if (/\.json$/i.test(e.name)) { try { text = JSON.stringify(JSON.parse(text), null, 2); } catch {} }
        const cut = e.size > 1 << 20 ? h('p', { class: 'muted' }, 'Showing the first 1 MB.') : null;
        content = h('div', {}, cut, h('pre', {}, text));
      } catch (err) { content = h('p', {}, err.message); }
    } else {
      content = h('p', {}, 'No preview for this kind of file.');
    }
    await dialog((box, close) => {
      box.append(h('h2', {}, e.name), h('p', {}, `${fmtBytes(e.size)} · ${fmtDate(e.mtime)}`),
        h('div', { class: 'preview-body' }, content),
        h('div', { class: 'actions' },
          h('button', { class: 'btn', onclick: () => download(path, e.name) }, icon('download'), 'Download'),
          h('button', { class: 'btn primary', onclick: () => close() }, 'Close')));
    }, { wide: true });
  }

  // Drag and drop upload.
  list.addEventListener('dragover', (e) => { if (e.dataTransfer?.types?.includes('Files')) { e.preventDefault(); list.classList.add('drop'); } });
  list.addEventListener('dragleave', (e) => { if (e.target === list) list.classList.remove('drop'); });
  list.addEventListener('drop', (e) => { e.preventDefault(); list.classList.remove('drop'); if (e.dataTransfer.files.length) upload([...e.dataTransfer.files]); });
  fileInput.addEventListener('change', () => { if (fileInput.files.length) upload([...fileInput.files]); fileInput.value = ''; });

  // Live updates from the watch.
  let refreshTimer = null;
  const soon = () => { clearTimeout(refreshTimer); refreshTimer = setTimeout(load, 250); };
  const offFiles = events.on('files.changed', (d) => {
    const touches = (p) => p && (parent(p) === cwd || p === cwd || cwd.startsWith(`${p}/`));
    if (touches(d.path) || touches(d.to)) soon();
  });
  const offSd = events.on('sd.changed', () => { if (cwd.startsWith('/sd')) soon(); });

  load();
  return {
    open: openTarget,
    unmount() { offFiles(); offSd(); clearTimeout(refreshTimer); },
  };
}
