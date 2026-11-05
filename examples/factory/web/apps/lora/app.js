// LoRa: a messenger over the watch's radio, and the radio's settings.
import { h, icon, toast, errorText, notice, fmtTime } from '../../js/ui.js';

const BWS = [7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500];
const dec = new TextDecoder('utf-8', { fatal: true });

const bytesOf = (b64) => Uint8Array.from(atob(b64 || ''), (c) => c.charCodeAt(0));
// Text when the packet is readable text, else hex.
export function packetText(b64) {
  const b = bytesOf(b64);
  try {
    const t = dec.decode(b);
    if (!/[\u0000-\u0008\u000e-\u001f\u007f]/.test(t)) return { text: t, hex: false };
  } catch {}
  return { text: [...b].map((x) => x.toString(16).padStart(2, '0')).join(' '), hex: true };
}

export function mount(el, ctx) {
  const { api, events } = ctx;
  let view = 'chat', cfg = null, packets = [], alive = true;
  const tabs = h('div', { class: 'seg' },
    h('button', { class: 'on', 'data-tab': 'chat', onclick: () => show('chat') }, 'Messages'),
    h('button', { 'data-tab': 'radio', onclick: () => show('radio') }, 'Radio'));
  const listenBtn = h('button', { class: 'btn', onclick: () => apply({ listen: !cfg?.listen }) });
  const saveBtn = h('button', { class: 'btn', title: 'Save the log to the SD card', onclick: saveLog }, icon('save'), h('span', { class: 'lbl' }, 'Save log'));
  const toolbar = h('div', { class: 'toolbar' }, tabs, h('span', { style: { flex: 1 } }), listenBtn, saveBtn);
  const body = h('div', { style: { flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0 } });
  el.append(h('div', { class: 'app lora' }, toolbar, body));

  // ---- messages
  const chat = h('div', { class: 'chat', 'aria-live': 'polite' });
  const input = h('input', { class: 'field', maxlength: 255, placeholder: 'Message', 'aria-label': 'Message' });
  const sendBtn = h('button', { class: 'btn primary', onclick: send }, icon('send'), 'Send');
  const hint = h('div', { class: 'muted', style: { padding: '0 14px', fontSize: '13px' } });
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') send(); });
  const chatView = [chat, hint, h('div', { class: 'composer' }, input, sendBtn)];

  function bubble(p) {
    const { text, hex } = packetText(p.data_b64);
    const meta = p.dir === 'rx' ? `${fmtTime(p.ts)} · ${p.rssi} dBm · SNR ${p.snr} dB` : `${fmtTime(p.ts)} · sent`;
    return h('div', { class: `bubble ${p.dir}`, 'data-n': p.n }, h('span', { class: hex ? 'mono' : '' }, text), h('small', {}, `${meta} · ${p.len} B`));
  }
  function renderChat() {
    const atEnd = chat.scrollTop + chat.clientHeight >= chat.scrollHeight - 20;
    chat.replaceChildren(...(packets.length ? packets.map(bubble) : [h('div', { class: 'empty' }, 'No messages yet.')]));
    if (atEnd || !chat.dataset.seen) chat.scrollTop = chat.scrollHeight;
    chat.dataset.seen = '1';
  }
  const addPacket = (p) => { if (packets.some((x) => x.n === p.n)) return; packets.push(p); packets.sort((a, b) => a.n - b.n); if (packets.length > 200) packets.shift(); renderChat(); };

  async function send() {
    const text = input.value;
    if (!text) return;
    if (new TextEncoder().encode(text).length > 255) { toast('At most 255 bytes', 'error'); return; }
    sendBtn.disabled = true;
    try {
      await api.loraSend({ text });
      input.value = '';
      await loadLog();
    } catch (e) { handle(e); }
    finally { sendBtn.disabled = false; input.focus(); }
  }

  // ---- radio settings
  function radioView() {
    const num = (name, value, attrs) => h('input', { class: 'field', type: 'number', name, value, ...attrs });
    const sel = (name, opts, value) => h('select', { class: 'field', name }, opts.map(([v, l]) => h('option', { value: v, selected: v === value }, l)));
    const f = {
      freq: num('freq', cfg.freq, { min: 150, max: 960, step: 0.001 }),
      bw: sel('bw', BWS.map((b) => [b, `${b} kHz`]), cfg.bw),
      sf: sel('sf', [5, 6, 7, 8, 9, 10, 11, 12].map((v) => [v, `SF${v}`]), cfg.sf),
      cr: sel('cr', [5, 6, 7, 8].map((v) => [v, `4/${v}`]), cfg.cr),
      power: num('power', cfg.power, { min: -9, max: 22, step: 1 }),
      sync: h('input', { class: 'field mono', name: 'sync_word', value: `0x${cfg.sync_word.toString(16).toUpperCase().padStart(2, '0')}` }),
      listen: h('input', { type: 'checkbox', name: 'listen', checked: cfg.listen }),
    };
    const save = async () => {
      const sync = parseInt(f.sync.value, f.sync.value.trim().toLowerCase().startsWith('0x') ? 16 : 10);
      await apply({ freq: Number(f.freq.value), bw: Number(f.bw.value), sf: Number(f.sf.value), cr: Number(f.cr.value),
        power: Number(f.power.value), sync_word: sync, listen: f.listen.checked }, true);
    };
    return h('div', { class: 'tool' }, h('div', { class: 'panel' }, h('h3', {}, 'Radio'),
      h('div', { class: 'form' },
        h('label', {}, 'Frequency (MHz)', f.freq), h('label', {}, 'Bandwidth', f.bw), h('label', {}, 'Spreading factor', f.sf),
        h('label', {}, 'Coding rate', f.cr), h('label', {}, 'Power (dBm)', f.power), h('label', {}, 'Sync word', f.sync),
        h('label', { class: 'check' }, f.listen, 'Receive between sends')),
      h('div', { style: { display: 'flex', justifyContent: 'flex-end', marginTop: '14px' } },
        h('button', { class: 'btn primary', onclick: save }, 'Apply')),
      h('p', { class: 'muted', style: { fontSize: '13px', margin: '10px 0 0' } },
        'Both ends need the same frequency, bandwidth, spreading factor, coding rate and sync word. Use a frequency allowed where you are.')));
  }

  async function apply(patch, announce = false) {
    try {
      cfg = await api.loraPatch(patch);
      renderBar();
      if (announce) toast('Radio settings applied');
    } catch (e) { handle(e); }
  }

  function renderBar() {
    if (!cfg) return;
    listenBtn.replaceChildren(icon('lora'), h('span', { class: 'lbl' }, cfg.listen ? 'Listening' : 'Not listening'));
    listenBtn.classList.toggle('primary', cfg.listen);
    listenBtn.title = cfg.listen ? 'Receiving between sends: click to stop' : 'Not receiving: click to listen';
    hint.textContent = cfg.listen ? `${cfg.freq} MHz · ${cfg.bw} kHz · SF${cfg.sf}` : 'Receiving is off: replies will not arrive. Turn on Listening.';
  }

  function show(v) {
    view = v;
    tabs.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b.dataset.tab === v));
    if (!cfg) return;
    if (v === 'chat') { body.replaceChildren(...chatView); renderChat(); setTimeout(() => input.focus()); }
    else body.replaceChildren(radioView());
  }

  function handle(e) {
    if (e.code === 'busy') {
      cfg = null;
      body.replaceChildren(notice(errorText(e, 'LoRa'), 'Try again', start));
    } else toast(errorText(e, 'LoRa'), 'error');
  }

  async function loadLog() { packets = (await api.loraLog()).packets || []; if (view === 'chat') renderChat(); }

  async function saveLog() {
    const d = new Date(), pad = (n) => String(n).padStart(2, '0');
    const name = `log-${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.json`;
    const path = `/sd/data/lora/${name}`;
    const data = { saved: d.toISOString(), radio: cfg, packets: packets.map((p) => ({ ...p, text: packetText(p.data_b64).text })) };
    try { await api.writeText(path, JSON.stringify(data, null, 2)); toast(`Saved ${path}`); }
    catch (e) { toast(errorText(e), 'error'); }
  }

  async function start() {
    body.replaceChildren(h('div', { class: 'empty' }, 'Loading…'));
    try {
      cfg = await api.loraConfig();
      await loadLog();
      renderBar();
      show(view);
    } catch (e) { handle(e); }
  }

  const off = events.on('lora.rx', (p) => { if (alive) addPacket(p); });
  const offHello = events.on('hello', () => { if (alive && cfg) loadLog().catch(() => {}); });
  start();
  return { unmount() { alive = false; off(); offHello(); } };
}
