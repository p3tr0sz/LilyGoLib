// Settings: display, time, storage, the web server and this browser's session.
import { h, icon, fmtBytes, fmtDuration, toast, confirmDialog } from '../../js/ui.js';

const TIMEOUTS = [[0, 'Never'], [5, '5 s'], [10, '10 s'], [15, '15 s'], [30, '30 s'], [60, '1 min'], [120, '2 min'], [255, '4 min']];
const IDLE = [[0, 'Never'], [300, '5 min'], [600, '10 min'], [1800, '30 min'], [3600, '1 hour']];

function row(label, hint, ...ctrl) {
  return h('div', { class: 'srow' },
    h('div', { class: 's-label' }, label, hint ? h('small', {}, hint) : null), ...ctrl);
}

function select(options, value, onchange) {
  const has = options.some(([v]) => v === value);
  const all = has ? options : [...options, [value, `${value}`]];
  return h('select', { class: 'field s-ctrl', onchange: (e) => onchange(Number(e.target.value)) },
    all.map(([v, label]) => h('option', { value: v, selected: v === value }, label)));
}

export function mount(el, ctx) {
  const { api, events } = ctx;
  const page = h('div', { class: 'settings' });
  el.append(h('div', { class: 'app' }, h('div', { style: { overflow: 'auto', flex: 1 } }, page)));

  const display = h('section', {}, h('h3', {}, 'Display'));
  const time = h('section', {}, h('h3', {}, 'Time'));
  const storage = h('section', {}, h('h3', {}, 'Storage'));
  const web = h('section', {}, h('h3', {}, 'Web desktop'));
  const device = h('section', {}, h('h3', {}, 'Watch'));
  page.append(display, time, storage, web, device);

  let settings = null;
  const patch = async (p) => {
    try { settings = await api.patchSettings(p); }
    catch (e) { toast(e.message, 'error'); }
  };

  // ---- display
  function renderDisplay() {
    const value = h('span', { class: 's-value' }, `${settings.brightness}%`);
    let t = null;
    const slider = h('input', {
      type: 'range', min: 0, max: 100, step: 1, value: settings.brightness, class: 's-ctrl', 'aria-label': 'Brightness',
      oninput: (e) => {
        value.textContent = `${e.target.value}%`;
        clearTimeout(t);
        t = setTimeout(() => patch({ brightness: Number(e.target.value) }), 150);
      },
    });
    display.replaceChildren(h('h3', {}, 'Display'),
      row('Brightness', null, value, slider),
      row('Screen off after', 'When the watch is not touched', select(TIMEOUTS, settings.screen_timeout_s, (v) => patch({ screen_timeout_s: v }))));
  }

  // ---- web
  let deskVersion = '';
  fetch('/manifest.json', { cache: 'no-store' }).then((r) => r.json()).then((m) => { deskVersion = m.version || ''; if (settings) renderWeb(); }).catch(() => {});

  function renderWeb() {
    web.replaceChildren(h('h3', {}, 'Web desktop'),
      row('Desktop version', 'Install a newer build from the rescue page',
        h('span', { class: 's-value mono' }, deskVersion),
        h('a', { class: 'btn', href: '/rescue' }, 'Update…')),
      row('Turn the web server off after', 'With no browser connected, to save battery', select(IDLE, settings.web_idle_off_s, (v) => patch({ web_idle_off_s: v }))),
      row('This browser', 'Forget this browser; it will need the PIN again',
        h('button', { class: 'btn', onclick: async () => { await api.logout(false).catch(() => {}); location.reload(); } }, 'Log out')),
      row('All browsers', 'Every paired browser will need the PIN again',
        h('button', {
          class: 'btn danger',
          onclick: async () => {
            if (!await confirmDialog('Log out all browsers?', 'Every browser paired with this watch, including this one, will need the PIN again.', 'Log out all', true)) return;
            await api.logout(true).catch(() => {});
            location.reload();
          },
        }, 'Log out all')));
  }

  // ---- time
  let clockTimer = null;
  function renderTime() {
    const d = ctx.device();
    const t = d?.time;
    const watchTime = h('span', { class: 's-value mono' });
    const browserTime = h('span', { class: 's-value mono' });
    const tick = () => {
      const now = new Date();
      browserTime.textContent = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
      const cur = ctx.device()?.time;
      if (cur) {
        const local = new Date((cur.epoch + (cur.offset_min || 0) * 60) * 1000 + (Date.now() - fetchedAt));
        watchTime.textContent = local.toISOString().slice(11, 19);
      }
    };
    const fetchedAt = Date.now();
    clearInterval(clockTimer);
    clockTimer = setInterval(tick, 1000);
    tick();
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;
    time.replaceChildren(h('h3', {}, 'Time'),
      row('On the watch', t ? `${t.tz || 'UTC'}${t.synced ? '' : ' · not set'}` : '', watchTime),
      row('In this browser', tz, browserTime),
      row('Set the watch clock', 'Copies this browser\'s time and time zone',
        h('button', {
          class: 'btn primary',
          onclick: async () => {
            const now = Date.now();
            try {
              await api.setTime(Math.round(now / 1000), -new Date(now).getTimezoneOffset(), tz);
              await ctx.refreshDevice();
              renderTime();
              toast('Watch clock set');
            } catch (e) { toast(e.message, 'error'); }
          },
        }, 'Set from this browser')));
  }

  // ---- storage
  async function renderStorage() {
    let sd = null;
    try { sd = await api.sd(); } catch (e) { sd = null; }
    const d = ctx.device();
    const meter = (total, free) => {
      const used = total - free;
      return h('div', { class: 's-ctrl' },
        h('div', { class: 'bar-meter' }, h('i', { style: { width: `${total ? Math.round((used / total) * 100) : 0}%` } })),
        h('small', { class: 'muted' }, `${fmtBytes(used)} used of ${fmtBytes(total)} · ${fmtBytes(free)} free`));
    };
    const rows = [];
    if (!sd || !sd.present) {
      rows.push(row(h('span', {}, icon('sd'), ' SD card'), 'No card. The watch takes FAT32 cards up to 32 GB.'));
    } else if (!sd.mounted) {
      rows.push(row('SD card', sd.error ? 'The watch could not read it. Use a FAT32 card up to 32 GB.' : 'Inserted, not mounted',
        h('button', { class: 'btn primary', onclick: async () => { try { await api.sdMount(); renderStorage(); } catch (e) { toast(e.message, 'error'); } } }, 'Mount')));
    } else {
      rows.push(row('SD card', `${sd.card || ''} ${sd.fs || ''}`.trim(), meter(sd.total, sd.free)));
      rows.push(row('Remove the card', 'Unmount first so no file is left half-written',
        h('button', {
          class: 'btn',
          onclick: async () => {
            try { await api.sdUnmount(); toast('Safe to remove the SD card'); renderStorage(); } catch (e) { toast(e.message, 'error'); }
          },
        }, icon('eject'), 'Safe to remove')));
    }
    if (d?.flash) rows.push(row('Internal flash', 'Boot sound, music, settings', meter(d.flash.total, d.flash.free)));
    storage.replaceChildren(h('h3', {}, 'Storage'), ...rows);
  }

  // ---- device
  function renderDevice() {
    const d = ctx.device();
    if (!d) return;
    const kv = (k, v) => row(k, null, h('span', { class: 's-value mono' }, v));
    device.replaceChildren(h('h3', {}, 'Watch'),
      kv('Board', d.board || ''),
      kv('Firmware', d.fw || ''),
      kv('Up for', d.uptime_s !== undefined ? fmtDuration(d.uptime_s) : ''),
      kv('Battery', d.battery ? `${d.battery.percent}%${d.battery.charging ? ', charging' : ''} · ${(d.battery.mv / 1000).toFixed(2)} V` : ''),
      kv('Network', d.net ? `${d.net.mode} · ${d.net.ssid || ''} · ${d.net.ip || ''}` : ''),
      kv('Memory free', d.mem ? `${fmtBytes(d.mem.heap_free)} RAM · ${fmtBytes(d.mem.psram_free)} PSRAM` : ''),
      row('Restart the watch', 'The desktop reconnects by itself',
        h('button', {
          class: 'btn danger',
          onclick: async () => {
            if (!await confirmDialog('Restart the watch?', 'Apps open on the watch will close.', 'Restart', true)) return;
            try { await api.reboot(); toast('Restarting…'); } catch (e) { toast(e.message, 'error'); }
          },
        }, 'Restart')));
  }

  async function load() {
    try {
      settings = await api.settings();
      await ctx.refreshDevice();
      renderDisplay();
      renderTime();
      renderWeb();
      renderDevice();
      await renderStorage();
    } catch (e) {
      page.replaceChildren(h('div', { class: 'empty' }, e.message));
    }
  }

  const offs = [
    events.on('settings.changed', (s) => {
      settings = s;
      // Don't redraw a control the user is moving.
      if (!display.contains(document.activeElement)) renderDisplay();
      if (!web.contains(document.activeElement)) renderWeb();
    }),
    events.on('sd.changed', () => renderStorage()),
    events.on('battery', () => renderDevice()),
  ];
  load();
  return { unmount() { offs.forEach((f) => f()); clearInterval(clockTimer); } };
}
