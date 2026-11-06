// Link: send a file to another T-Watch over ESP-NOW (no Wi-Fi network between them).
import { h, icon, errorText, notice, toast, fmtBytes, fmtTime } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, state = null, poll = null, refreshSeq = 0;

  const enableBtn = h('button', { class: 'btn', onclick: toggle });
  const peersBox = h('div', { class: 'tool' });
  const xfer = h('div', { class: 'tool' });
  const sendPath = h('input', { class: 'field', placeholder: '/sd/data/notes.txt', 'aria-label': 'File to send' });
  const sendTo = h('select', { class: 'field', 'aria-label': 'Send to' });
  const sendBtn = h('button', { class: 'btn primary', onclick: send }, icon('send'), 'Send');
  const status = h('span', { class: 'muted' });

  el.append(h('div', { class: 'app link' },
    h('div', { class: 'toolbar' }, enableBtn, status),
    h('div', { class: 'callout' }, 'Both watches must have Link on and be within a few metres.'),
    h('div', { class: 'panel' }, h('h3', {}, 'In range'), peersBox),
    h('div', { class: 'panel' }, h('h3', {}, 'Send a file'),
      h('div', { class: 'form' },
        h('label', {}, 'File', sendPath),
        h('label', {}, 'To', sendTo),
        h('div', { class: 'actions' }, sendBtn))),
    h('div', { class: 'panel' }, h('h3', {}, 'Transfer'), xfer)));

  function render() {
    if (!state) { status.textContent = ''; return; }
    enableBtn.replaceChildren(icon(state.enabled ? 'wifi' : 'wifiOff'), document.createTextNode(state.enabled ? 'Link on' : 'Link off'));
    enableBtn.classList.toggle('primary', !state.enabled);
    status.textContent = state.enabled ? `This watch: ${state.name} · ${state.mac}` : 'Turn Link on to find watches.';

    const peers = state.peers || [];
    if (!state.enabled) peersBox.replaceChildren(h('div', { class: 'empty' }, 'Link is off.'));
    else if (!peers.length) peersBox.replaceChildren(h('div', { class: 'empty' }, 'Looking for watches…'));
    else peersBox.replaceChildren(h('table', { class: 'tbl' },
      h('tbody', {}, peers.map((p) => h('tr', {},
        h('td', {}, p.name || '(unnamed)', h('span', { class: 'sub' }, p.mac)),
        h('td', { class: 'num' }, `${p.rssi} dBm`),
        h('td', { class: 'num muted' }, fmtTime(p.last_seen)))))));

    const opts = [h('option', { value: 'ff:ff:ff:ff:ff:ff' }, 'Everyone in range')];
    for (const p of peers) opts.push(h('option', { value: p.mac }, p.name || p.mac));
    const keep = sendTo.value;
    sendTo.replaceChildren(...opts);
    if ([...sendTo.options].some((o) => o.value === keep)) sendTo.value = keep;
    sendBtn.disabled = !state.enabled;

    const t = state.transfer;
    if (!t) { xfer.replaceChildren(h('div', { class: 'empty' }, 'No transfer yet.')); return; }
    const frac = t.size ? t.done / t.size : 0;
    const label = t.state === 'done' ? 'Done' : t.state === 'error' ? `Failed: ${t.error || 'error'}`
      : `${t.dir === 'tx' ? 'Sending' : 'Receiving'} ${Math.round(frac * 100)}%`;
    xfer.replaceChildren(h('div', { class: 'srow' },
      h('span', {}, `${t.dir === 'tx' ? '↑' : '↓'} ${t.name}`), h('span', { class: 'muted' }, fmtBytes(t.size))),
      h('div', { class: 'progress' }, h('i', { style: { width: `${Math.round(frac * 100)}%` } })),
      h('div', { class: 'srow' }, h('span', { class: t.state === 'error' ? 'error' : 'muted' }, label),
        t.state !== 'done' && t.state !== 'error' ? h('button', { class: 'btn ghost', onclick: () => api.linkCancel().catch(() => {}) }, 'Cancel') : null));
  }

  async function refresh() {
    const seq = ++refreshSeq;
    try {
      const next = await api.link();
      if (alive && seq === refreshSeq) { state = next; render(); }
    } catch (e) { if (alive && seq === refreshSeq) status.textContent = errorText(e, 'Link'); }
  }

  async function toggle() {
    enableBtn.disabled = true;
    try { const r = await api.linkEnable(!(state && state.enabled)); state = { ...(state || {}), enabled: r.enabled }; await refresh(); }
    catch (e) { toast(errorText(e, 'Link'), 'error'); } finally { enableBtn.disabled = false; }
  }

  async function send() {
    const path = sendPath.value.trim();
    if (!path) { toast('Enter a file path, e.g. /sd/data/notes.txt', 'error'); return; }
    sendBtn.disabled = true;
    try { await api.linkSend(path, sendTo.value); toast('Sending…'); }
    catch (e) { toast(errorText(e, 'Link'), 'error'); } finally { sendBtn.disabled = false; }
  }

  const off = events.on('link', (d) => { if (alive) { refreshSeq++; state = d; render(); } });
  const offFiles = events.on('files.changed', (d) => { if (d.op === 'write' && d.path && d.path.startsWith('/sd/data/link/')) toast(`Received ${d.path.split('/').pop()}`); });
  refresh();
  poll = setInterval(refresh, 5000);
  return { unmount() { alive = false; clearInterval(poll); off(); offFiles(); } };
}
