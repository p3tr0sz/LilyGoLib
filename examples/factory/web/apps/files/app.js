// Files: browse, upload, download, rename, move, delete and preview files on the SD card and the flash.
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

  function go(path) { cwd = path; load(); }

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

  async function more(e, path) {
    await dialog((box, close) => {
      const b = (label, fn, cls = '') => h('button', { class: `btn ${cls}`, style: { width: '100%' }, onclick: () => { close(); fn(); } }, label);
      box.append(h('h2', {}, e.name),
        h('div', { style: { display: 'grid', gap: '8px' } },
          e.dir ? b('Open', () => go(path)) : b('Preview', () => preview(e, path)),
          e.dir ? null : b('Download', () => download(path, e.name)),
          b('Rename', () => rename(e, path)),
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
  return { unmount() { offFiles(); offSd(); clearTimeout(refreshTimer); } };
}
