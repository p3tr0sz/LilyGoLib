// Small canvas drawings shared by the tool apps: line charts that keep their size in step with the layout.

export function fit(canvas) {
  const r = canvas.getBoundingClientRect();
  const d = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * d)), hgt = Math.max(1, Math.round(r.height * d));
  if (canvas.width !== w || canvas.height !== hgt) { canvas.width = w; canvas.height = hgt; }
  return { g: canvas.getContext('2d'), w, h: hgt, d };
}

export const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
export const palette = () => [cssVar('--accent') || '#00d4aa', '#ff6b35', '#5aa9ff', '#b577ff'];

const fmtv = (v) => String(Math.abs(v) >= 100 || Number.isInteger(v) ? Math.round(v) : Math.abs(v) >= 10 ? +v.toFixed(1) : +v.toFixed(2));

// lines: [{values, color}], newest last; cap: how many points fill the width.
export function drawLines(canvas, lines, { cap = 0, min, max, unit = '' } = {}) {
  const { g, w, h, d } = fit(canvas);
  g.clearRect(0, 0, w, h);
  const all = lines.flatMap((l) => l.values).filter(Number.isFinite);
  let lo = min ?? (all.length ? Math.min(...all) : 0), hi = max ?? (all.length ? Math.max(...all) : 1);
  if (hi - lo < 1e-9) { hi += 1; lo -= 1; }
  const span = hi - lo;
  if (min === undefined) lo -= span * 0.08;
  if (max === undefined) hi += span * 0.08;
  const pad = 8 * d;
  g.strokeStyle = '#262626';
  g.lineWidth = d;
  for (let i = 0; i <= 3; i++) {
    const y = Math.round(pad + (h - 2 * pad) * i / 3) + 0.5;
    g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
  }
  for (const l of lines) {
    const n = Math.max(cap || l.values.length, 2);
    g.strokeStyle = l.color;
    g.lineWidth = 2 * d;
    g.lineJoin = 'round';
    g.beginPath();
    let on = false;
    l.values.forEach((v, i) => {
      if (!Number.isFinite(v)) { on = false; return; }
      const x = w - (l.values.length - 1 - i) * (w / (n - 1));
      const y = pad + (h - 2 * pad) * (1 - (v - lo) / (hi - lo));
      if (on) g.lineTo(x, y); else { g.moveTo(x, y); on = true; }
    });
    g.stroke();
  }
  g.font = `${11 * d}px system-ui, sans-serif`;
  g.fillStyle = '#a0a0a0';
  g.fillText(fmtv(hi) + unit, 4 * d, pad + 10 * d);
  g.fillText(fmtv(lo) + unit, 4 * d, h - pad - 2 * d);
}

// Keep a fixed-length history: push v, drop the oldest beyond cap.
export function push(arr, v, cap) { arr.push(v); if (arr.length > cap) arr.splice(0, arr.length - cap); return arr; }
