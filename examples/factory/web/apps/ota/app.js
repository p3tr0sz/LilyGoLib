// Update: replace the firmware over the air, into the spare slot, with a roll-back to the running one.
import { h, icon, errorText, notice, toast, confirmDialog } from '../../js/ui.js';

export function mount(el, ctx) {
  const { api, events } = ctx;
  let alive = true, st = null, file = null;

  const slotInfo = h('div', { class: 'tool' });
  const fileInput = h('input', { type: 'file', accept: '.bin', style: { display: 'none' }, onchange: pick });
  const pickBtn = h('button', { class: 'btn', onclick: () => fileInput.click() }, icon('upload'), 'Choose .bin…');
  const fileName = h('span', { class: 'muted' }, 'No file chosen');
  const uploadBtn = h('button', { class: 'btn primary', onclick: upload }, 'Upload');
  const bar = h('i');
  const progress = h('div', { class: 'progress', hidden: true }, bar);
  const commitBtn = h('button', { class: 'btn danger', onclick: commit }, 'Reboot into new firmware');
  const rollbackBtn = h('button', { class: 'btn', onclick: rollback }, 'Roll back');
  const status = h('span', { class: 'muted' });

  el.append(h('div', { class: 'app ota' },
    h('div', { class: 'callout warn' }, 'Flashing the wrong file can brick the watch. Only upload a firmware build made for the T-Watch Ultra.'),
    h('div', { class: 'panel' }, h('h3', {}, 'Firmware'), slotInfo),
    h('div', { class: 'panel' }, h('h3', {}, 'Upload a build'),
      h('div', { class: 'toolbar' }, pickBtn, fileName), fileInput,
      h('div', { class: 'toolbar' }, uploadBtn, status), progress),
    h('div', { class: 'panel' }, h('h3', {}, 'Apply'),
      h('div', { class: 'toolbar' }, commitBtn, rollbackBtn))));

  function render() {
    if (!st) return;
    const row = (k, v) => h('div', { class: 'srow' }, h('span', { class: 'muted' }, k), h('span', { class: 'mono' }, v));
    slotInfo.replaceChildren(
      row('Running slot', st.slot),
      row('Spare slot', st.other_slot),
      row('State', st.state + (st.error ? ` (${st.error})` : '')));
    uploadBtn.disabled = !file || st.state === 'writing';
    commitBtn.disabled = st.state !== 'ready';
    rollbackBtn.disabled = !st.can_rollback;
    if (st.state === 'writing' && st.progress?.total) {
      progress.hidden = false;
      bar.style.width = `${Math.round(st.progress.done / st.progress.total * 100)}%`;
    } else if (st.state !== 'writing') {
      progress.hidden = st.state !== 'ready';
      if (st.state === 'ready') bar.style.width = '100%';
    }
    if (st.state === 'ready') status.textContent = 'Uploaded. Reboot to apply, or roll back.';
    else if (st.state === 'error') status.textContent = `Error: ${st.error || 'failed'}`;
  }

  async function refresh() { try { st = await api.ota(); if (alive) render(); } catch (e) { if (alive) status.textContent = errorText(e, 'Update'); } }

  function pick() {
    file = fileInput.files[0] || null;
    fileName.textContent = file ? `${file.name} · ${(file.size / 1024).toFixed(0)} KB` : 'No file chosen';
    render();
  }

  async function upload() {
    if (!file) return;
    uploadBtn.disabled = true;
    progress.hidden = false;
    bar.style.width = '0%';
    status.textContent = 'Uploading…';
    try {
      const r = await api.otaUpload(file, (f) => { bar.style.width = `${Math.round(f * 100)}%`; });
      status.textContent = `Uploaded ${(r.size / 1024).toFixed(0)} KB.`;
      await refresh();
    } catch (e) {
      status.textContent = e.code === 'bad_image' ? 'That file is not a firmware image.'
        : e.code === 'too_large' ? 'That file is too large for the slot.' : errorText(e, 'the upload');
      await refresh();
    }
  }

  async function commit() {
    if (!await confirmDialog('Reboot into the new firmware?', 'The watch will restart. If the new build does not start, it rolls back to the current one on the next power-up.', 'Continue')) return;
    if (!await confirmDialog('Are you sure?', 'This replaces the running firmware. Do not do this unless you trust the build you uploaded.', 'Reboot now', true)) return;
    try { await api.otaCommit(); toast('Rebooting into the new firmware…'); }
    catch (e) { toast(errorText(e, 'the update'), 'error'); await refresh(); }
  }

  async function rollback() {
    if (!await confirmDialog('Roll back to the other slot?', 'The watch reboots into the previously installed firmware.', 'Roll back', true)) return;
    try { await api.otaRollback(); toast('Rolling back…'); }
    catch (e) { toast(errorText(e, 'the rollback'), 'error'); }
  }

  const off = events.on('ota', (d) => { if (alive) { st = d; render(); } });
  refresh();
  return { unmount() { alive = false; off(); } };
}
