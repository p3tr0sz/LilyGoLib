// NFC: read tags held to the watch, see their records, and keep them on the card.
import { h, icon, toast, errorText, fmtTime } from '../../js/ui.js';

const KIND = { text: 'Text', uri: 'Link', wifi: 'Wi-Fi', other: 'Other' };

export function mount(el, ctx) {
  const { api, events } = ctx;
  const secs = h('select', { class: 'field', style: { width: 'auto' }, 'aria-label': 'Scan for' },
    [10, 20, 30, 60].map((s) => h('option', { value: s, selected: s === 20 }, `${s} s`)));
  const scanBtn = h('button', { class: 'btn primary', onclick: toggle });
  const status = h('span', { class: 'muted' });
  const card = h('div', { class: 'panel nfc-tag' });
  const hist = h('div', {});
  el.append(h('div', { class: 'app' },
    h('div', { class: 'toolbar' }, scanBtn, secs, status),
    h('div', { class: 'tool' }, card, h('div', { class: 'panel' }, h('h3', {}, 'Read this session'), hist))));

  let scanning = false, until = 0, tick = null, last = null, alive = true;
  const seen = [];

  function renderScan() {
    scanBtn.replaceChildren(icon(scanning ? 'stop' : 'nfc'), scanning ? 'Stop' : 'Scan');
    scanBtn.classList.toggle('primary', !scanning);
    secs.disabled = scanning;
    if (scanning) {
      const left = Math.max(0, Math.ceil((until - Date.now()) / 1000));
      status.textContent = until ? `Hold a tag to the watch… ${left} s` : 'Hold a tag to the watch…';
    } else status.textContent = '';
  }
  function setScanning(on, seconds) {
    scanning = on;
    until = on && seconds ? Date.now() + seconds * 1000 : 0;
    clearInterval(tick);
    if (on) tick = setInterval(renderScan, 500);
    renderScan();
  }

  const fileName = (uid) => `${(uid || 'tag').replace(/[^0-9a-f]/gi, '').toUpperCase() || 'tag'}.json`;
  function renderTag() {
    if (!last) {
      card.replaceChildren(h('h3', {}, 'Tag'), h('div', { class: 'muted' }, 'No tag read yet. Press Scan, then hold a tag to the back of the watch.'));
      return;
    }
    const recs = last.ndef || [];
    card.replaceChildren(
      h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' } },
        h('h3', { style: { margin: 0, flex: 1 } }, 'Tag'),
        h('button', { class: 'btn', onclick: () => save(last) }, icon('save'), 'Save')),
      h('div', { class: 'uid', title: 'UID' }, last.uid),
      h('div', { class: 'muted' }, `${last.type || 'Unknown type'} · ${fmtTime(last.ts)}`),
      recs.length ? h('table', { class: 'tbl', style: { marginTop: '10px' } },
        h('thead', {}, h('tr', {}, h('th', {}, 'Record'), h('th', {}, 'Value'))),
        h('tbody', {}, recs.map((r) => h('tr', {}, h('td', {}, KIND[r.kind] || r.kind),
          h('td', { style: { wordBreak: 'break-all' } }, r.kind === 'uri' && /^https?:\/\//i.test(r.value)
            ? h('a', { href: r.value, target: '_blank', rel: 'noopener' }, r.value) : r.value)))))
        : h('div', { class: 'muted', style: { marginTop: '10px' } }, 'No NDEF records.'));
  }
  function renderHist() {
    hist.replaceChildren(...(seen.length ? seen.map((t) => h('div', { class: 'note' },
      h('span', { class: 'mono' }, t.uid), h('span', { class: 'muted' }, t.type || ''), h('span', { class: 'when' }, fmtTime(t.ts))))
      : [h('div', { class: 'muted' }, 'Nothing yet.')]));
  }

  async function save(tag) {
    const path = `/sd/data/nfc/${fileName(tag.uid)}`;
    try { await api.writeText(path, JSON.stringify(tag, null, 2)); toast(`Saved ${path}`); }
    catch (e) { toast(errorText(e), 'error'); }
  }

  async function toggle() {
    scanBtn.disabled = true;
    try {
      if (scanning) { await api.nfcStop(); setScanning(false); }
      else { const s = Number(secs.value); await api.nfcScan(s); setScanning(true, s); }
    } catch (e) { toast(errorText(e, 'NFC'), 'error'); }
    finally { scanBtn.disabled = false; }
  }

  const offs = [
    events.on('nfc.tag', (t) => { if (!alive) return; last = t; seen.unshift(t); if (seen.length > 50) seen.pop(); renderTag(); renderHist(); }),
    events.on('nfc.state', (d) => { if (alive && !d.scanning) setScanning(false); else if (alive && d.scanning && !scanning) setScanning(true); }),
  ];
  setScanning(false);
  renderTag();
  renderHist();
  api.nfc().then((s) => { if (!alive) return; last = s.last || null; renderTag(); if (s.scanning) setScanning(true); }).catch(() => {});
  return { unmount() { alive = false; clearInterval(tick); offs.forEach((f) => f()); } };
}
