// Small DOM helpers, icons, dialogs and toasts shared by the shell and the apps.

export function h(tag, props = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat(Infinity)) {
    if (c === undefined || c === null || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

const P = {
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  file: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="M21 17l-5-5-9 8"/>',
  audio: '<path d="M9 18V6l11-2v12"/><circle cx="6" cy="18" r="3"/><circle cx="17" cy="16" r="3"/>',
  text: '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5M9 13h6M9 17h6"/>',
  settings: '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>',
  battery: '<rect x="2" y="7" width="18" height="10" rx="2"/><path d="M22 11v2"/>',
  charging: '<rect x="2" y="7" width="18" height="10" rx="2"/><path d="M22 11v2M11 8l-3 4h4l-3 4"/>',
  wifi: '<path d="M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/>',
  wifiOff: '<path d="M3 3l18 18M8.5 16a5 5 0 0 1 7 0M2 9a15 15 0 0 1 4.2-2.8M10.7 5.1A15 15 0 0 1 22 9"/><circle cx="12" cy="19" r="1"/>',
  sd: '<path d="M7 3h8l4 4v12a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M9 7v3M12 7v3M15 7v3"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
  close: '<path d="M6 6l12 12M18 6L6 18"/>',
  back: '<path d="M15 18l-6-6 6-6"/>',
  up: '<path d="M12 19V5M5 12l7-7 7 7"/>',
  upload: '<path d="M12 16V4M6 10l6-6 6 6M4 20h16"/>',
  download: '<path d="M12 4v12M6 10l6 6 6-6M4 20h16"/>',
  newFolder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M12 10v6M9 13h6"/>',
  refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"/>',
  rename: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M14 6l4 4"/>',
  trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
  more: '<circle cx="5" cy="12" r="1.5"/><circle cx="12" cy="12" r="1.5"/><circle cx="19" cy="12" r="1.5"/>',
  eject: '<path d="M5 15h14L12 6zM5 19h14"/>',
};

export function icon(name) {
  const span = document.createElement('span');
  span.style.display = 'contents';
  span.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${P[name] || P.file}</svg>`;
  return span.firstChild;
}

export function fmtBytes(n) {
  if (n === null || n === undefined || Number.isNaN(n)) return '';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${i ? n.toFixed(n < 10 ? 1 : 0) : n} ${u[i]}`;
}

export function fmtDate(epochS) {
  if (!epochS) return '';
  const d = new Date(epochS * 1000);
  const same = d.toDateString() === new Date().toDateString();
  return same ? d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : d.toLocaleDateString([], { year: 'numeric', month: 'short', day: 'numeric' });
}

export function fmtDuration(s) {
  s = Math.floor(s);
  const d = Math.floor(s / 86400), hr = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${hr} h` : hr ? `${hr} h ${m} min` : `${m} min`;
}

// ---------------------------------------------------------------- toasts

export function toast(message, kind = 'info', ms = 3500) {
  const el = h('div', { class: `toast ${kind}` }, message);
  document.getElementById('toasts').append(el);
  if (ms) setTimeout(() => el.remove(), ms);
  return el;
}

export function progressToast(label) {
  const bar = h('i');
  const text = h('div', {}, label);
  const el = h('div', { class: 'toast' }, text, h('div', { class: 'progress' }, bar));
  document.getElementById('toasts').append(el);
  return {
    set(p) { bar.style.width = `${Math.round(p * 100)}%`; },
    done(msg, kind = 'info') { text.textContent = msg; el.classList.toggle('error', kind === 'error'); bar.style.width = '100%'; setTimeout(() => el.remove(), 2500); },
  };
}

// ---------------------------------------------------------------- dialogs

export function dialog(build, { wide = false } = {}) {
  return new Promise((resolve) => {
    const prev = document.activeElement;
    const box = h('div', { class: `dialog${wide ? ' wide' : ''}`, role: 'dialog', 'aria-modal': 'true' });
    const scrim = h('div', { class: 'scrim' }, box);
    const close = (v) => { scrim.remove(); document.removeEventListener('keydown', onKey); if (prev && prev.focus) prev.focus(); resolve(v); };
    const onKey = (e) => { if (e.key === 'Escape') close(null); };
    scrim.addEventListener('mousedown', (e) => { if (e.target === scrim) close(null); });
    document.addEventListener('keydown', onKey);
    build(box, close);
    document.body.append(scrim);
    const f = box.querySelector('[autofocus], input, select, textarea, .btn.primary, .btn');
    if (f) f.focus();
  });
}

export function confirmDialog(title, text, okLabel = 'OK', danger = false) {
  return dialog((box, close) => {
    box.append(
      h('h2', {}, title),
      text ? h('p', {}, text) : null,
      h('div', { class: 'actions' },
        h('button', { class: 'btn', onclick: () => close(false) }, 'Cancel'),
        h('button', { class: `btn ${danger ? 'danger' : 'primary'}`, onclick: () => close(true) }, okLabel)),
    );
  }).then((v) => v === true);
}

export function promptDialog(title, value = '', okLabel = 'OK', validate = () => null) {
  return dialog((box, close) => {
    const input = h('input', { class: 'field', value, autofocus: true, spellcheck: 'false' });
    const err = h('div', { class: 'pair-error' });
    const submit = () => {
      const v = input.value.trim();
      const e = validate(v);
      if (e) { err.textContent = e; return; }
      close(v);
    };
    input.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });
    box.append(h('h2', {}, title), input, err,
      h('div', { class: 'actions' },
        h('button', { class: 'btn', onclick: () => close(null) }, 'Cancel'),
        h('button', { class: 'btn primary', onclick: submit }, okLabel)));
    setTimeout(() => { input.focus(); const dot = value.lastIndexOf('.'); input.setSelectionRange(0, dot > 0 ? dot : value.length); });
  });
}
