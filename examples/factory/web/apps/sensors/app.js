// Sensors: the watch's motion sensor live: tilt, heading on a compass, acceleration and steps.
import { h, errorText } from '../../js/ui.js';
import { drawLines, push, palette, fit } from '../../js/chart.js';

const EVERY_MS = 200;
const CAP = 150;            // 30 s of acceleration

export function mount(el, ctx) {
  const { api } = ctx;
  const box = (label) => { const b = h('b', {}, '–'); return [h('div', { class: 'stat-box' }, h('small', {}, label), b), b]; };
  const [sR, vR] = box('Roll');
  const [sP, vP] = box('Pitch');
  const [sH, vH] = box('Heading');
  const [sS, vS] = box('Steps today');
  const [sG, vG] = box('|a|');
  const dial = h('canvas', { class: 'dial', 'aria-label': 'Compass' });
  const accel = h('canvas', { class: 'chart', 'aria-label': 'Acceleration' });
  const [c1, c2, c3] = palette();
  const err = h('div', { class: 'muted' });
  const root = h('div', { class: 'tool sensors' },
    h('div', { class: 'stats' }, sR, sP, sH, sG, sS), err,
    h('div', { class: 'grid2' },
      h('div', { class: 'panel' }, h('h3', {}, 'Compass'), dial),
      h('div', { class: 'panel' }, h('h3', {}, 'Acceleration (m/s²)'), accel,
        h('div', { class: 'legend' }, ...[['x', c1], ['y', c2], ['z', c3]].map(([n, c]) => h('span', {}, h('i', { style: { background: c } }), n))))));
  el.append(h('div', { class: 'app' }, root));

  const ax = [], ay = [], az = [];
  let last = null, timer = null, alive = true, seen = 0;

  function drawDial(heading, roll, pitch) {
    const { g, w, d } = fit(dial);
    const r = w / 2, acc = palette()[0];
    g.clearRect(0, 0, w, w);
    g.save();
    g.translate(r, r);
    g.strokeStyle = '#333'; g.lineWidth = 2 * d;
    g.beginPath(); g.arc(0, 0, r - 4 * d, 0, Math.PI * 2); g.stroke();
    // Ticks and letters turn so that the watch's heading is up.
    g.rotate(-heading * Math.PI / 180);
    for (let a = 0; a < 360; a += 15) {
      const long = a % 90 === 0;
      g.strokeStyle = long ? '#bbb' : '#444';
      g.beginPath();
      g.moveTo(0, -(r - 6 * d)); g.lineTo(0, -(r - (long ? 18 : 12) * d)); g.stroke();
      g.rotate(15 * Math.PI / 180);
    }
    g.fillStyle = '#ddd'; g.font = `600 ${14 * d}px system-ui`; g.textAlign = 'center'; g.textBaseline = 'middle';
    [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([t, a]) => {
      const rad = a * Math.PI / 180, rr = r - 32 * d;
      g.fillStyle = t === 'N' ? '#ff4d4f' : '#ddd';
      g.fillText(t, Math.sin(rad) * rr, -Math.cos(rad) * rr);
    });
    g.restore();
    // The watch's heading mark and a bubble for the tilt.
    g.fillStyle = acc;
    g.beginPath(); g.moveTo(r, 2 * d); g.lineTo(r - 8 * d, 16 * d); g.lineTo(r + 8 * d, 16 * d); g.fill();
    const bx = r + Math.max(-1, Math.min(1, roll / 45)) * r * 0.45, by = r + Math.max(-1, Math.min(1, pitch / 45)) * r * 0.45;
    g.strokeStyle = '#333'; g.beginPath(); g.arc(r, r, r * 0.45, 0, Math.PI * 2); g.stroke();
    g.beginPath(); g.moveTo(r - 8 * d, r); g.lineTo(r + 8 * d, r); g.moveTo(r, r - 8 * d); g.lineTo(r, r + 8 * d); g.stroke();
    g.fillStyle = acc; g.beginPath(); g.arc(bx, by, 9 * d, 0, Math.PI * 2); g.fill();
  }
  function draw() {
    if (!last) return;
    drawDial(last.imu.heading || 0, last.imu.roll || 0, last.imu.pitch || 0);
    const [a, b, c] = palette();
    drawLines(accel, [{ values: ax, color: a }, { values: ay, color: b }, { values: az, color: c }], { cap: CAP });
  }

  async function tick() {
    clearTimeout(timer);
    if (!alive) return;
    if (document.hidden) { timer = setTimeout(tick, 1000); return; }
    try {
      const s = await api.sensors();
      const imu = s.imu || {};
      const fmt = (v) => (v == null ? '–' : `${v.toFixed(1)}°`);
      vR.textContent = fmt(imu.roll); vP.textContent = fmt(imu.pitch); vH.textContent = fmt(imu.heading);
      vS.textContent = s.steps == null ? '–' : s.steps.toLocaleString();
      if (imu.accel) {
        vG.textContent = `${Math.hypot(imu.accel.x, imu.accel.y, imu.accel.z).toFixed(2)} m/s²`;
        if (s.seq !== last?.seq) { push(ax, imu.accel.x, CAP); push(ay, imu.accel.y, CAP); push(az, imu.accel.z, CAP); }
      }
      last = s;
      root.dataset.seq = String(s.seq);
      root.dataset.reads = String(++seen);
      err.textContent = '';
      draw();
    } catch (e) {
      err.textContent = errorText(e, 'the sensors');
    } finally {
      if (alive) timer = setTimeout(tick, EVERY_MS);
    }
  }
  const ro = new ResizeObserver(draw);
  ro.observe(root);
  tick();
  return { unmount() { alive = false; clearTimeout(timer); ro.disconnect(); } };
}
