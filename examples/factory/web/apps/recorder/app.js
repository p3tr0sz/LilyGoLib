// Recorder: record on the watch's microphone to the card, listen here or on the watch.
import { h, icon, toast, errorText, fmtBytes, fmtDate } from '../../js/ui.js';

const DIR = '/sd/data/recordings';
const mmss = (ms) => { const s = Math.floor((ms || 0) / 1000); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`; };

export function mount(el, ctx) {
  const { api, events, caps } = ctx;
  const canPlay = caps.includes('audio.play');
  const recBtn = h('button', { class: 'rec-btn', 'aria-label': 'Record', onclick: toggle }, icon('record'));
  const time = h('span', { class: 'rec-time' }, '0:00');
  const meter = h('i', { style: { width: '0%' } });
  const maxSel = h('select', { class: 'field', style: { width: 'auto' }, 'aria-label': 'Longest recording' },
    [[30, '30 s'], [60, '1 min'], [300, '5 min'], [600, '10 min']].map(([v, l]) => h('option', { value: v, selected: v === 60 }, l)));
  const state = h('div', { class: 'muted' });
  const list = h('div', {});
  const player = h('div', { class: 'panel', hidden: !canPlay });
  el.append(h('div', { class: 'app' }, h('div', { class: 'tool recorder' },
    h('div', { class: 'panel' },
      h('div', { class: 'rec-head' }, recBtn, time, h('div', { class: 'bar-meter', title: 'Microphone level' }, meter), maxSel), state),
    player,
    h('div', { class: 'panel' }, h('h3', {}, 'Recordings on the card'), list))));

  let rec = { state: 'idle' }, audio = null, alive = true;

  function renderRec() {
    const on = rec.state === 'recording';
    recBtn.classList.toggle('on', on);
    recBtn.replaceChildren(icon(on ? 'stop' : 'record'));
    recBtn.setAttribute('aria-label', on ? 'Stop' : 'Record');
    maxSel.disabled = on;
    time.textContent = on ? `${mmss(rec.elapsed_ms)} / ${mmss((rec.max_s || 60) * 1000)}` : mmss(rec.elapsed_ms || 0);
    meter.style.width = `${on ? rec.level || 0 : 0}%`;
    state.textContent = on ? `Recording to ${rec.path}` : rec.path ? `Last: ${rec.path}` : 'Records 16 kHz mono WAV on the SD card.';
  }

  function renderPlayer() {
    if (!canPlay || !audio) return;
    const vol = h('input', { type: 'range', min: 0, max: 100, value: audio.volume, 'aria-label': 'Watch volume', style: { maxWidth: '220px' } });
    let t = null;
    vol.addEventListener('input', () => { clearTimeout(t); t = setTimeout(() => api.audioPatch({ volume: Number(vol.value) }).catch((e) => toast(errorText(e), 'error')), 150); });
    player.replaceChildren(h('h3', {}, 'Watch speaker'),
      h('div', { style: { display: 'flex', alignItems: 'center', gap: '12px', flexWrap: 'wrap' } },
        h('span', { class: 'audio-state', style: { flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis' } },
          audio.playing ? `Playing ${audio.path}` : 'Not playing'),
        audio.playing ? h('button', { class: 'btn', onclick: () => api.audioStop().catch((e) => toast(errorText(e), 'error')) }, icon('stop'), 'Stop') : null,
        h('span', { class: 'muted' }, 'Volume'), vol));
  }

  async function loadList() {
    try {
      const l = await api.list(DIR);
      const files = l.entries.filter((e) => !e.dir && /\.(wav|mp3)$/i.test(e.name)).sort((a, b) => b.mtime - a.mtime || b.name.localeCompare(a.name));
      if (!files.length) { list.replaceChildren(h('div', { class: 'muted' }, 'No recordings yet.')); return; }
      list.replaceChildren(h('table', { class: 'tbl' }, h('tbody', {}, files.map((f) => {
        const path = `${DIR}/${f.name}`;
        return h('tr', { class: 'rec-row', 'data-name': f.name },
          h('td', {}, f.name, h('span', { class: 'sub' }, `${fmtBytes(f.size)} · ${fmtDate(f.mtime)}`)),
          h('td', { style: { width: '45%' } }, h('audio', { controls: true, preload: 'none', src: api.contentUrl(path) })),
          canPlay ? h('td', { style: { width: '1%' } }, h('button', { class: 'btn', title: 'Play on the watch speaker', onclick: () => playOnWatch(path) },
            icon('play'), h('span', { class: 'lbl' }, 'On watch'))) : null);
      }))));
    } catch (e) {
      list.replaceChildren(h('div', { class: 'muted' }, e.status === 404 ? 'No recordings yet.' : errorText(e)));
    }
  }

  async function playOnWatch(path) {
    try { await api.audioPlay(path); } catch (e) { toast(errorText(e, 'the speaker'), 'error'); }
  }

  async function toggle() {
    recBtn.disabled = true;
    try {
      if (rec.state === 'recording') {
        const r = await api.recStop();
        rec = { ...rec, state: 'idle', elapsed_ms: r.duration_ms, path: r.path };
        toast(`Saved ${r.path}`);
        loadList();
      } else {
        const r = await api.recStart({ max_s: Number(maxSel.value) });
        rec = { state: 'recording', path: r.path, elapsed_ms: 0, max_s: Number(maxSel.value), level: 0 };
      }
      renderRec();
    } catch (e) { toast(errorText(e, 'the microphone'), 'error'); }
    finally { recBtn.disabled = false; }
  }

  const offs = [
    events.on('recorder', (r) => {
      if (!alive) return;
      const ended = rec.state === 'recording' && r.state !== 'recording';
      rec = r;
      renderRec();
      if (ended) loadList();
    }),
    events.on('audio', (a) => { if (alive) { audio = a; renderPlayer(); } }),
    events.on('files.changed', (c) => { if (alive && [c.path, c.to].some((p) => p && p.startsWith(DIR))) loadList(); }),
    events.on('sd.changed', () => { if (alive) loadList(); }),
  ];
  renderRec();
  api.recorder().then((r) => { if (alive) { rec = r; renderRec(); } }).catch(() => {});
  if (canPlay) api.audio().then((a) => { if (alive) { audio = a; renderPlayer(); } }).catch(() => {});
  loadList();
  return { unmount() { alive = false; offs.forEach((f) => f()); } };
}
