// Monitor: battery and memory over the last minutes, what the watch is doing, and its log.
import { h, fmtBytes, fmtDuration, errorText } from '../../js/ui.js';
import { drawLines, push, palette } from '../../js/chart.js';

const EVERY_MS = 2000;
const CAP = 150;            // 5 minutes of samples

export function mount(el, ctx) {
  const { api } = ctx;
  const box = (label) => { const b = h('b', {}, '–'); return [h('div', { class: 'stat-box' }, h('small', {}, label), b), b]; };
  const [sBat, vBat] = box('Battery');
  const [sVolt, vVolt] = box('Voltage');
  const [sCur, vCur] = box('Current');
  const [sTmp, vTmp] = box('Temperature');
  const [sUp, vUp] = box('Uptime');
  const [sApp, vApp] = box('Open on the watch');
  const [sCpu, vCpu] = box('CPU');
  const [sHeap, vHeap] = box('Free heap');
  const [sPs, vPs] = box('Free PSRAM');
  const charts = {};
  const chartPanel = (key, title, unit) => {
    const c = h('canvas', { class: 'chart', 'aria-label': title });
    charts[key] = { c, unit };
    return h('div', { class: 'panel' }, h('h3', {}, title), c);
  };
  const logEl = h('pre', { class: 'logbox', 'aria-label': 'Watch log' });
  const err = h('div', { class: 'muted' });
  const root = h('div', { class: 'tool monitor' },
    h('div', { class: 'stats' }, sBat, sVolt, sCur, sTmp, sUp, sApp, sCpu, sHeap, sPs), err,
    h('div', { class: 'grid2' },
      chartPanel('pct', 'Battery %', '%'), chartPanel('mv', 'Battery voltage', ' mV'), chartPanel('ma', 'Battery current', ' mA'),
      chartPanel('heap', 'Free heap', ' KB'), chartPanel('psram', 'Free PSRAM', ' MB')),
    h('div', { class: 'panel' }, h('h3', {}, 'Log'), logEl));
  el.append(h('div', { class: 'app' }, root));

  const hist = { pct: [], mv: [], ma: [], heap: [], psram: [] };
  let timer = null, alive = true, after = 0, samples = 0;
  const lines = [];

  function draw() {
    const [a] = palette();
    for (const [k, { c, unit }] of Object.entries(charts)) {
      drawLines(c, [{ values: hist[k], color: a }], { cap: CAP, unit, ...(k === 'pct' ? { min: 0, max: 100 } : {}) });
    }
  }

  async function tick() {
    clearTimeout(timer);
    if (!alive) return;
    try {
      const m = await api.monitor();
      const b = m.battery || {}, mem = m.mem || {};
      vBat.textContent = `${b.percent ?? '–'}%${b.charging ? ' charging' : ''}`;
      vVolt.textContent = b.mv ? `${(b.mv / 1000).toFixed(2)} V` : '–';
      vCur.textContent = b.ma == null ? '–' : `${b.ma} mA`;
      vTmp.textContent = b.temp_c == null ? '–' : `${b.temp_c.toFixed(1)} °C`;
      vUp.textContent = fmtDuration(m.uptime_s || 0);
      vApp.textContent = m.app || 'Watch face';
      vCpu.textContent = m.cpu_mhz ? `${m.cpu_mhz} MHz` : '–';
      vHeap.textContent = mem.heap_free == null ? '–' : fmtBytes(mem.heap_free);
      sHeap.title = mem.heap_min ? `Lowest since start: ${fmtBytes(mem.heap_min)}` : '';
      vPs.textContent = mem.psram_free == null ? '–' : fmtBytes(mem.psram_free);
      push(hist.pct, b.percent ?? NaN, CAP);
      push(hist.mv, b.mv ?? NaN, CAP);
      push(hist.ma, b.ma ?? NaN, CAP);
      push(hist.heap, mem.heap_free == null ? NaN : mem.heap_free / 1024, CAP);
      push(hist.psram, mem.psram_free == null ? NaN : mem.psram_free / 1048576, CAP);
      root.dataset.samples = String(++samples);
      err.textContent = '';
      draw();
      const l = await api.logs(after);
      if (l.lines?.length) {
        const atEnd = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 8;
        for (const x of l.lines) lines.push(`${(x.ms / 1000).toFixed(1).padStart(8)}  ${x.text}`);
        if (lines.length > 500) lines.splice(0, lines.length - 500);
        logEl.textContent = lines.join('\n');
        if (atEnd) logEl.scrollTop = logEl.scrollHeight;
      }
      if (typeof l.next === 'number') after = l.next;
    } catch (e) {
      err.textContent = errorText(e);
    } finally {
      if (alive) timer = setTimeout(tick, document.hidden ? EVERY_MS * 3 : EVERY_MS);
    }
  }
  const ro = new ResizeObserver(draw);
  ro.observe(root);
  tick();
  return { unmount() { alive = false; clearTimeout(timer); ro.disconnect(); } };
}
