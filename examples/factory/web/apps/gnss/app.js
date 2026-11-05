// GNSS: the watch's position and satellites, the path walked this session, and tracks from the card.
import { h, icon, errorText, fmtTime, fmtBytes } from '../../js/ui.js';
import { fit, palette } from '../../js/chart.js';

const EVERY_MS = 1000;
const TRACKS = '/sd/tracks';
const SYS_COLOR = { GPS: '#5aa9ff', GLONASS: '#ff6b35', Galileo: '#b577ff', BeiDou: '#ffb800', QZSS: '#88cc00', SBAS: '#a0a0a0' };

// GPX in the browser: track, route and waypoints, in order.
export function parseGpx(text) {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.querySelector('parsererror')) throw new Error('Not a GPX file');
  const pts = [];
  for (const p of doc.querySelectorAll('trkpt, rtept, wpt')) {
    const lat = parseFloat(p.getAttribute('lat')), lon = parseFloat(p.getAttribute('lon'));
    if (Number.isFinite(lat) && Number.isFinite(lon)) pts.push({ lat, lon });
  }
  return pts;
}

export function mount(el, ctx) {
  const { api } = ctx;
  const box = (label) => { const b = h('b', {}, '–'); return [h('div', { class: 'stat-box' }, h('small', {}, label), b), b]; };
  const [sFix, vFix] = box('Fix');
  const [sLat, vLat] = box('Latitude');
  const [sLon, vLon] = box('Longitude');
  const [sAlt, vAlt] = box('Altitude');
  const [sSpd, vSpd] = box('Speed');
  const [sSat, vSat] = box('Satellites');
  const [sTtff, vTtff] = box('Time to fix');
  const [sAnt, vAnt] = box('Antenna');
  const osm = h('a', { class: 'btn', target: '_blank', rel: 'noopener', hidden: true }, icon('link'), 'OpenStreetMap');
  const model = h('span', { class: 'muted' });
  const sky = h('canvas', { class: 'dial', 'aria-label': 'Satellites in the sky' });
  const bars = h('canvas', { class: 'chart', 'aria-label': 'Signal strength' });
  const map = h('canvas', { class: 'map', 'aria-label': 'Map' });
  const trackSel = h('select', { class: 'field', style: { maxWidth: '260px' }, 'aria-label': 'Track from the card', onchange: () => loadTrack(trackSel.value) },
    h('option', { value: '' }, 'Tracks on the card…'));
  const mapNote = h('span', { class: 'muted', style: { fontSize: '13px' } });
  const err = h('div', { class: 'muted' });
  const root = h('div', { class: 'tool gnss' },
    h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' } }, model, h('span', { style: { flex: 1 } }), osm),
    h('div', { class: 'stats' }, sFix, sLat, sLon, sAlt, sSpd, sSat, sTtff, sAnt), err,
    h('div', { class: 'panel' }, h('div', { style: { display: 'flex', gap: '10px', alignItems: 'center', flexWrap: 'wrap', marginBottom: '8px' } },
      h('h3', { style: { margin: 0, flex: 1 } }, 'Map'), mapNote, trackSel), map,
      h('div', { class: 'legend' }, h('span', {}, h('i', { style: { background: 'var(--accent)' } }), 'This session'), h('span', {}, h('i', { style: { background: '#ff6b35' } }), 'Track from the card'))),
    h('div', { class: 'grid2' },
      h('div', { class: 'panel' }, h('h3', {}, 'Sky'), sky,
        h('div', { class: 'legend' }, Object.entries(SYS_COLOR).map(([n, c]) => h('span', {}, h('i', { style: { background: c } }), n)))),
      h('div', { class: 'panel' }, h('h3', {}, 'Signal (C/N0, dB-Hz)'), bars)));
  el.append(h('div', { class: 'app' }, root));

  let last = null, timer = null, alive = true, track = [], gpx = [];

  function drawSky() {
    const { g, w, d } = fit(sky);
    const r = w / 2 - 14 * d;
    g.clearRect(0, 0, w, w);
    g.strokeStyle = '#333'; g.lineWidth = d;
    for (const el of [0, 30, 60]) { g.beginPath(); g.arc(w / 2, w / 2, r * (90 - el) / 90, 0, Math.PI * 2); g.stroke(); }
    g.beginPath(); g.moveTo(w / 2, w / 2 - r); g.lineTo(w / 2, w / 2 + r); g.moveTo(w / 2 - r, w / 2); g.lineTo(w / 2 + r, w / 2); g.stroke();
    g.fillStyle = '#a0a0a0'; g.font = `${11 * d}px system-ui`; g.textAlign = 'center'; g.textBaseline = 'middle';
    g.fillText('N', w / 2, 6 * d); g.fillText('S', w / 2, w - 6 * d); g.fillText('E', w - 6 * d, w / 2); g.fillText('W', 6 * d, w / 2);
    const sats = last?.satellites || [];
    for (const s of sats) {
      if (s.el == null || s.az == null) continue;
      const rr = r * (90 - Math.max(0, s.el)) / 90, a = s.az * Math.PI / 180;
      const x = w / 2 + Math.sin(a) * rr, y = w / 2 - Math.cos(a) * rr;
      g.fillStyle = SYS_COLOR[s.sys] || '#ddd';
      g.globalAlpha = s.used ? 1 : 0.45;
      g.beginPath(); g.arc(x, y, 7 * d, 0, Math.PI * 2); g.fill();
      g.globalAlpha = 1;
      g.fillStyle = '#000'; g.font = `600 ${8 * d}px system-ui`;
      g.fillText(String(s.prn), x, y);
    }
    sky.dataset.sats = String(sats.length);
  }

  function drawBars() {
    const { g, w, h: hh, d } = fit(bars);
    g.clearRect(0, 0, w, hh);
    const sats = (last?.satellites || []).filter((s) => s.cn0 != null);
    if (!sats.length) return;
    const bw = w / sats.length, max = 50;
    sats.forEach((s, i) => {
      const bh = (hh - 16 * d) * Math.min(1, s.cn0 / max);
      g.fillStyle = SYS_COLOR[s.sys] || '#ddd';
      g.globalAlpha = s.used ? 1 : 0.45;
      g.fillRect(i * bw + 2 * d, hh - 14 * d - bh, Math.max(1, bw - 4 * d), bh);
      g.globalAlpha = 1;
      g.fillStyle = '#a0a0a0'; g.font = `${9 * d}px system-ui`; g.textAlign = 'center';
      g.fillText(String(s.prn), i * bw + bw / 2, hh - 3 * d);
    });
  }

  function drawMap() {
    const { g, w, h: hh, d } = fit(map);
    g.clearRect(0, 0, w, hh);
    const all = [...track, ...gpx];
    if (!all.length) {
      g.fillStyle = '#a0a0a0'; g.font = `${13 * d}px system-ui`; g.textAlign = 'center';
      g.fillText('Waiting for a position', w / 2, hh / 2);
      return;
    }
    // Local flat projection in metres around the middle of everything shown.
    const lat0 = all.reduce((a, p) => a + p.lat, 0) / all.length, lon0 = all.reduce((a, p) => a + p.lon, 0) / all.length;
    const kx = 111320 * Math.cos(lat0 * Math.PI / 180), ky = 110540;
    const xy = (p) => [(p.lon - lon0) * kx, (p.lat - lat0) * ky];
    let minx = Infinity, maxx = -Infinity, miny = Infinity, maxy = -Infinity;
    for (const p of all) { const [x, y] = xy(p); minx = Math.min(minx, x); maxx = Math.max(maxx, x); miny = Math.min(miny, y); maxy = Math.max(maxy, y); }
    const span = Math.max(maxx - minx, maxy - miny, 100);
    const pad = 24 * d, scale = Math.min(w - 2 * pad, hh - 2 * pad) / span;
    const cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    const px = (p) => { const [x, y] = xy(p); return [w / 2 + (x - cx) * scale, hh / 2 - (y - cy) * scale]; };
    const line = (pts, color) => {
      if (!pts.length) return;
      g.strokeStyle = color; g.lineWidth = 3 * d; g.lineJoin = 'round'; g.lineCap = 'round';
      g.beginPath();
      pts.forEach((p, i) => { const [x, y] = px(p); if (i) g.lineTo(x, y); else g.moveTo(x, y); });
      g.stroke();
    };
    line(gpx, '#ff6b35');
    line(track, palette()[0]);
    if (track.length) {
      const [x, y] = px(track[track.length - 1]);
      g.fillStyle = palette()[0];
      g.beginPath(); g.arc(x, y, 7 * d, 0, Math.PI * 2); g.fill();
      g.strokeStyle = '#fff'; g.lineWidth = 2 * d; g.stroke();
    }
    // Scale bar: a round number of metres about a quarter of the width.
    const target = (w / 4) / scale;
    const nice = [1, 2, 5].flatMap((m) => [1, 10, 100, 1000, 10000].map((k) => m * k)).filter((v) => v <= target).pop() || 1;
    const len = nice * scale;
    g.strokeStyle = '#ddd'; g.lineWidth = 2 * d;
    g.beginPath(); g.moveTo(pad, hh - pad); g.lineTo(pad + len, hh - pad); g.stroke();
    g.fillStyle = '#ddd'; g.font = `${11 * d}px system-ui`; g.textAlign = 'left';
    g.fillText(nice >= 1000 ? `${nice / 1000} km` : `${nice} m`, pad, hh - pad - 6 * d);
    map.dataset.points = String(track.length);
    map.dataset.gpxPoints = String(gpx.length);
  }

  const draw = () => { drawSky(); drawBars(); drawMap(); };

  async function tick() {
    clearTimeout(timer);
    if (!alive) return;
    try {
      const s = await api.gnss();
      last = s;
      model.textContent = s.model ? `Receiver: ${s.model}` : '';
      vFix.textContent = s.fix ? 'Yes' : 'Searching…';
      sFix.classList.toggle('ok', !!s.fix);
      vLat.textContent = s.lat == null ? '–' : s.lat.toFixed(6);
      vLon.textContent = s.lon == null ? '–' : s.lon.toFixed(6);
      vAlt.textContent = s.alt == null ? '–' : `${s.alt.toFixed(1)} m`;
      vSpd.textContent = s.speed_kmh == null ? '–' : `${s.speed_kmh.toFixed(1)} km/h`;
      const used = (s.satellites || []).filter((x) => x.used).length;
      vSat.textContent = `${s.sats ?? used} used / ${(s.satellites || []).length}`;
      vTtff.textContent = s.ttff_ms == null ? '–' : `${(s.ttff_ms / 1000).toFixed(0)} s`;
      vAnt.textContent = s.antenna || '–';
      if (s.fix && s.lat != null) {
        const p = { lat: s.lat, lon: s.lon };
        const prev = track[track.length - 1];
        if (!prev || prev.lat !== p.lat || prev.lon !== p.lon) track.push(p);
        if (track.length > 3600) track.shift();
        osm.hidden = false;
        osm.href = `https://www.openstreetmap.org/?mlat=${s.lat}&mlon=${s.lon}#map=17/${s.lat}/${s.lon}`;
      }
      root.dataset.fix = s.fix ? '1' : '0';
      err.textContent = s.time ? `GNSS time ${fmtTime(s.time)}` : '';
      draw();
    } catch (e) {
      err.textContent = errorText(e, 'the GNSS');
    } finally {
      if (alive) timer = setTimeout(tick, document.hidden ? EVERY_MS * 5 : EVERY_MS);
    }
  }

  async function listTracks() {
    try {
      const l = await api.list(TRACKS);
      const files = l.entries.filter((e) => !e.dir && /\.gpx$/i.test(e.name));
      trackSel.replaceChildren(h('option', { value: '' }, files.length ? 'Tracks on the card…' : 'No tracks on the card'),
        ...files.map((f) => h('option', { value: `${TRACKS}/${f.name}` }, `${f.name} (${fmtBytes(f.size)})`)));
    } catch (e) {
      trackSel.replaceChildren(h('option', { value: '' }, e.code === 'sd_missing' ? 'No SD card' : 'No tracks on the card'));
    }
  }
  async function loadTrack(path) {
    gpx = [];
    mapNote.textContent = '';
    if (path) {
      try {
        gpx = parseGpx(await api.readText(path, 4 << 20));
        mapNote.textContent = `${gpx.length} points`;
      } catch (e) { mapNote.textContent = e.message; }
    }
    drawMap();
  }

  const ro = new ResizeObserver(draw);
  ro.observe(root);
  listTracks();
  tick();
  return { unmount() { alive = false; clearTimeout(timer); ro.disconnect(); } };
}
