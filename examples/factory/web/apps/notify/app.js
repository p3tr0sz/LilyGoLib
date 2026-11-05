// Notify: send a notification to the watch (a pop-up and a buzz) and see the ones it showed.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

const LEVELS = [['info', 'Info'], ['warn', 'Warning'], ['alert', 'Alert']];

export function mount(el, ctx) {
  const { api, events } = ctx;
  const title = h('input', { class: 'field', maxlength: 64, placeholder: 'Title', 'aria-label': 'Title', name: 'title' });
  const body = h('textarea', { class: 'field', maxlength: 512, placeholder: 'Message (optional)', 'aria-label': 'Message', name: 'body' });
  let level = 'info';
  const seg = h('div', { class: 'seg', role: 'group', 'aria-label': 'Level' }, LEVELS.map(([v, label]) =>
    h('button', { class: v === level ? 'on' : '', 'data-level': v, onclick: (e) => { level = v; seg.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b === e.currentTarget)); } }, label)));
  const vibrate = h('input', { type: 'checkbox', checked: true });
  const send = h('button', { class: 'btn primary', onclick: submit }, icon('send'), 'Send to watch');
  const list = h('div', { class: 'notes' });
  let items = [];

  el.append(h('div', { class: 'app' }, h('div', { class: 'tool' },
    h('div', { class: 'panel' }, h('h3', {}, 'New notification'),
      h('div', { style: { display: 'grid', gap: '10px' } }, title, body,
        h('div', { style: { display: 'flex', gap: '12px', alignItems: 'center', flexWrap: 'wrap' } },
          seg, h('label', { class: 'check' }, vibrate, 'Vibrate'), h('span', { style: { flex: 1 } }), send))),
    h('div', { class: 'panel' }, h('h3', {}, 'Shown on the watch'), list))));

  title.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });

  function render() {
    if (!items.length) { list.replaceChildren(h('div', { class: 'muted' }, 'Nothing yet.')); return; }
    list.replaceChildren(...items.map((n) => h('div', { class: 'note' },
      h('span', { class: `tag ${n.level === 'info' ? 'ok' : n.level}` }, n.level),
      h('div', { style: { minWidth: 0 } }, h('b', {}, n.title), n.body ? h('p', {}, n.body) : null),
      h('span', { class: 'when' }, fmtTime(n.ts)))));
  }
  const add = (n) => { items = [n, ...items.filter((x) => x.id !== n.id)].slice(0, 20); render(); };

  async function submit() {
    const t = title.value.trim();
    if (!t) { title.focus(); return; }
    send.disabled = true;
    try {
      await api.notify({ title: t, body: body.value.trim(), level, vibrate: vibrate.checked });
      title.value = ''; body.value = '';
      toast('Sent to the watch');
      load();
    } catch (e) { toast(errorText(e, 'notifications'), 'error'); }
    finally { send.disabled = false; }
  }
  async function load() {
    try { items = (await api.notifications()).items || []; render(); }
    catch (e) { list.replaceChildren(h('div', { class: 'muted' }, errorText(e))); }
  }
  const off = events.on('notify', add);
  render();
  load();
  setTimeout(() => title.focus());
  return { unmount() { off(); } };
}
