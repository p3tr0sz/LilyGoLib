// Automation: manage the watch's headless rule engine over /api/automation/*. A rule listens on an
// event topic, matches field predicates over the event's `d`, and runs an ordered list of actions.
// The firmware and the JS mock share the same matcher, so the Test panel here agrees with the device.
import { h, icon, toast, errorText } from '../../js/ui.js';

const idValid = (s) => /^[A-Za-z0-9._-]{1,48}$/.test(s) && !s.includes('..');

// Topics the engine commonly emits (free text is allowed; a trailing * is a prefix match).
const TOPICS = ['battery', 'net', 'notify', 'settings.changed', 'sd.changed', 'wifi.scan', 'ble.scan',
  'nfc.tag', 'lora.rx', 'gnss.fix', 'presence.alarm', 'sentinel', 'sniffer'];

const OPS = [
  ['eq', '='], ['ne', '≠'], ['lt', '<'], ['le', '≤'], ['gt', '>'], ['ge', '≥'],
  ['substr', 'contains'], ['exists', 'exists'],
];

// Parse a value the way the engine compares it: booleans, null, numbers stay typed; everything else is a string.
function parseScalar(s) {
  const t = String(s).trim();
  if (t === 'true') return true;
  if (t === 'false') return false;
  if (t === 'null') return null;
  if (t !== '' && !Number.isNaN(Number(t))) return Number(t);
  return s;
}

export function mount(el, ctx) {
  const { api, setTitle } = ctx;
  let rules = [];       // [{id,enabled,on}]
  let current = '';     // loaded rule id, '' = unsaved/new
  let busy = false;
  let rawMode = false;  // true while the raw-JSON textarea is the source of truth

  // ---------------------------------------------------------------- rules bar
  const select = h('select', { class: 'field', 'aria-label': 'Rule', style: { maxWidth: '55%' } });
  const btnNew = h('button', { class: 'btn' }, 'New');
  const btnSave = h('button', { class: 'btn primary' }, icon('save'), h('span', {}, 'Save'));
  const btnDelete = h('button', { class: 'btn danger' }, icon('trash'), h('span', {}, 'Delete'));
  const status = h('span', { class: 'muted', style: { fontSize: '12px', flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' } });

  const rulesBar = h('div', { class: 'panel' },
    h('h3', {}, 'Rules'),
    h('div', { style: { display: 'flex', flexWrap: 'wrap', gap: '8px', alignItems: 'center' } },
      select, btnNew, btnSave, btnDelete),
    h('div', { style: { marginTop: '8px', display: 'flex' } }, status));

  // ---------------------------------------------------------------- identity / trigger
  const idInput = h('input', { class: 'field', placeholder: 'rule-id', 'aria-label': 'Rule id', spellcheck: 'false', autocomplete: 'off', style: { maxWidth: '220px' } });
  const enabled = h('input', { type: 'checkbox', checked: true });
  const topicList = h('datalist', { id: 'auto-topics' }, TOPICS.map((t) => h('option', { value: t })));
  const onInput = h('input', { class: 'field', list: 'auto-topics', placeholder: 'battery', 'aria-label': 'Trigger topic', spellcheck: 'false', autocomplete: 'off', style: { maxWidth: '220px' } });
  const interval = h('input', { class: 'field', type: 'number', min: '0', step: '1000', placeholder: 'none', 'aria-label': 'Minimum interval (ms)', style: { maxWidth: '140px' } });

  const triggerPanel = h('div', { class: 'panel' },
    h('h3', {}, 'Trigger'),
    topicList,
    h('div', { class: 'auto-grid' },
      labelled('Id', idInput),
      labelled('On topic', onInput),
      labelled('Min interval (ms)', interval),
      h('label', { class: 'check', style: { alignSelf: 'end' } }, enabled, 'Enabled')),
    h('div', { class: 'field-hint' }, 'A trailing * matches a prefix, e.g. ble.* — min interval throttles how often the rule may fire.'));

  // ---------------------------------------------------------------- if predicates
  const ifRows = h('div', { style: { display: 'grid', gap: '8px' } });
  const btnAddIf = h('button', { class: 'btn' }, icon('newFolder'), h('span', {}, 'Add condition'));
  const ifPanel = h('div', { class: 'panel' },
    h('h3', {}, 'Conditions (if)'),
    ifRows,
    h('div', { style: { marginTop: '8px' } }, btnAddIf),
    h('div', { class: 'field-hint' }, 'All conditions must match. No conditions = fire on every event. Fields are read from the event data.'));

  function ifRow(field = '', op = 'eq', value = '') {
    const f = h('input', { class: 'field', placeholder: 'field', 'aria-label': 'Field', spellcheck: 'false', autocomplete: 'off', value: field });
    const o = h('select', { class: 'field', 'aria-label': 'Operator' }, OPS.map(([v, lbl]) => h('option', { value: v, selected: v === op || undefined }, lbl)));
    const v = h('input', { class: 'field', placeholder: 'value', 'aria-label': 'Value', spellcheck: 'false', autocomplete: 'off', value: String(value) });
    const del = h('button', { class: 'btn icon ghost', 'aria-label': 'Remove condition', onclick: () => { row.remove(); syncFromForm(); } }, icon('close'));
    const syncVisibility = () => { v.placeholder = o.value === 'exists' ? 'true / false' : 'value'; };
    o.addEventListener('change', () => { syncVisibility(); syncFromForm(); });
    [f, v].forEach((x) => x.addEventListener('input', syncFromForm));
    syncVisibility();
    const row = h('div', { class: 'auto-cond', 'data-row': 'if' }, f, o, v, del);
    row._read = () => ({ field: f.value.trim(), op: o.value, value: v.value });
    return row;
  }

  // ---------------------------------------------------------------- do actions
  const doRows = h('div', { style: { display: 'grid', gap: '10px' } });
  const btnAddDo = h('button', { class: 'btn' }, icon('newFolder'), h('span', {}, 'Add action'));
  const doPanel = h('div', { class: 'panel' },
    h('h3', {}, 'Actions (do)'),
    doRows,
    h('div', { style: { marginTop: '8px' } }, btnAddDo),
    h('div', { class: 'field-hint' }, 'Actions run in order; the chain stops at the first failure.'));

  function doRow(action = { notify: { title: '' } }) {
    const kind = action.term !== undefined ? 'term' : action.script !== undefined ? 'script' : 'notify';
    const kindSel = h('select', { class: 'field', 'aria-label': 'Action kind', style: { maxWidth: '140px' } },
      ['notify', 'term', 'script'].map((k) => h('option', { value: k, selected: k === kind || undefined }, k)));
    const fields = h('div', { style: { display: 'grid', gap: '8px', flex: 1, minWidth: 0 } });
    const del = h('button', { class: 'btn icon ghost', 'aria-label': 'Remove action', onclick: () => { row.remove(); syncFromForm(); } }, icon('close'));

    let readFields = () => ({});
    function build(k, seed) {
      fields.replaceChildren();
      if (k === 'notify') {
        const n = seed.notify || {};
        const t = h('input', { class: 'field', placeholder: 'Title', 'aria-label': 'Notify title', value: n.title || '' });
        const b = h('input', { class: 'field', placeholder: 'Body (optional)', 'aria-label': 'Notify body', value: n.body || '' });
        const lv = h('select', { class: 'field', 'aria-label': 'Notify level', style: { maxWidth: '140px' } },
          ['', 'info', 'warn', 'alert'].map((x) => h('option', { value: x, selected: x === (n.level || '') || undefined }, x || 'default level')));
        [t, b].forEach((x) => x.addEventListener('input', syncFromForm));
        lv.addEventListener('change', syncFromForm);
        fields.append(t, b, lv);
        readFields = () => { const o = { title: t.value }; if (b.value) o.body = b.value; if (lv.value) o.level = lv.value; return { notify: o }; };
      } else if (k === 'term') {
        const line = h('input', { class: 'field', placeholder: 'logs add hello', 'aria-label': 'Terminal line', spellcheck: 'false', autocomplete: 'off', value: seed.term || '', style: { fontFamily: 'var(--mono)' } });
        line.addEventListener('input', syncFromForm);
        fields.append(line);
        readFields = () => ({ term: line.value });
      } else {
        const name = h('input', { class: 'field', placeholder: 'name.scr', 'aria-label': 'Script name', spellcheck: 'false', autocomplete: 'off', value: seed.script || '' });
        name.addEventListener('input', syncFromForm);
        fields.append(name);
        readFields = () => ({ script: name.value });
      }
    }
    build(kind, action);
    kindSel.addEventListener('change', () => { build(kindSel.value, {}); syncFromForm(); });
    const row = h('div', { class: 'auto-action' }, h('div', { style: { display: 'flex', gap: '8px' } }, kindSel, del), fields);
    row._read = () => readFields();
    return row;
  }

  // ---------------------------------------------------------------- raw JSON
  const raw = h('textarea', { class: 'field', spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', 'aria-label': 'Raw rule JSON',
    style: { minHeight: '140px', fontFamily: 'var(--mono)', fontSize: '12px', whiteSpace: 'pre', resize: 'vertical' } });
  const btnApplyRaw = h('button', { class: 'btn' }, 'Apply JSON to form');
  const rawErr = h('div', { class: 'pair-error' });
  const rawPanel = h('details', { class: 'panel' },
    h('summary', { style: { cursor: 'pointer', fontWeight: '600' } }, 'Raw JSON'),
    h('div', { style: { marginTop: '10px', display: 'grid', gap: '8px' } },
      h('div', { class: 'field-hint', style: { marginTop: 0 } }, 'The form and this JSON stay in sync. Edit here for anything the form does not cover, then apply.'),
      raw, rawErr, h('div', {}, btnApplyRaw)));
  rawPanel.addEventListener('toggle', () => { if (rawPanel.open) { rawMode = false; raw.value = JSON.stringify(buildRule(), null, 2); } });

  // ---------------------------------------------------------------- test panel
  const testTopic = h('input', { class: 'field', list: 'auto-topics', placeholder: 'battery', 'aria-label': 'Test event topic', spellcheck: 'false', autocomplete: 'off', style: { maxWidth: '220px' } });
  const testData = h('textarea', { class: 'field', spellcheck: 'false', autocomplete: 'off', placeholder: '{ "level": 12, "charging": false }', 'aria-label': 'Test event data (JSON)',
    style: { minHeight: '72px', fontFamily: 'var(--mono)', fontSize: '12px', resize: 'vertical' } });
  const btnTest = h('button', { class: 'btn primary' }, icon('play'), h('span', {}, 'Run test'));
  const btnTestFill = h('button', { class: 'btn' }, 'Use rule topic');
  const testOut = h('div', { style: { marginTop: '10px' } });
  const testPanel = h('div', { class: 'panel' },
    h('h3', {}, 'Test'),
    h('div', { class: 'field-hint', style: { marginTop: 0 } }, 'Dry-run the current rule against a synthetic event. No actions are executed on the watch.'),
    h('div', { class: 'auto-grid' }, labelled('Event topic', testTopic)),
    labelled('Event data (d)', testData),
    h('div', { style: { display: 'flex', gap: '8px', marginTop: '10px', flexWrap: 'wrap' } }, btnTest, btnTestFill),
    testOut);

  // ---------------------------------------------------------------- styles + assembly
  el.append(h('style', { html: `
    .auto-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: 10px; align-items: end; }
    .auto-grid label.lbl { display: grid; gap: 4px; font-size: 12px; color: var(--text-2); }
    .auto-cond { display: grid; grid-template-columns: 1fr 100px 1fr auto; gap: 6px; align-items: center; }
    .auto-action { display: grid; gap: 8px; border: 1px solid var(--line); border-radius: var(--radius-s); padding: 8px; }
    .auto-badges { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 8px; }
    .auto-plan { font-family: var(--mono); font-size: 12px; white-space: pre-wrap; word-break: break-word; }
    @media (max-width: 520px) { .auto-cond { grid-template-columns: 1fr 1fr; } }
  ` }));
  el.append(h('div', { class: 'app' }, h('div', { class: 'tool' },
    rulesBar, triggerPanel, ifPanel, doPanel, rawPanel, testPanel)));

  // ---------------------------------------------------------------- helpers
  function labelled(text, control) { return h('label', { class: 'lbl' }, text, control); }
  function setStatus(text, danger) { status.textContent = text || ''; status.style.color = danger ? 'var(--danger)' : ''; status.classList.toggle('muted', !danger); }
  function setBusy(b) { busy = b; [btnNew, btnSave, btnDelete, select].forEach((x) => { x.disabled = b; }); }

  function fillSelect() {
    select.replaceChildren(h('option', { value: '' }, rules.length ? '— choose a rule —' : '— no rules yet —'));
    for (const r of rules) select.append(h('option', { value: r.id }, `${r.id}${r.enabled ? '' : ' (off)'}`));
    select.value = current;
  }

  // Read the whole form into a rule object.
  function buildRule() {
    const rule = { id: idInput.value.trim(), enabled: enabled.checked, on: onInput.value.trim() };
    const ifObj = {};
    for (const row of ifRows.querySelectorAll('[data-row="if"]')) {
      const { field, op, value } = row._read();
      if (!field) continue;
      ifObj[field] = op === 'exists' ? { exists: parseScalar(value) !== false && value.trim() !== 'false' }
        : { [op]: parseScalar(value) };
    }
    if (Object.keys(ifObj).length) rule.if = ifObj;
    const doArr = [];
    for (const row of doRows.children) doArr.push(row._read());
    if (doArr.length) rule.do = doArr;
    const iv = Number(interval.value);
    if (interval.value.trim() !== '' && iv > 0) rule.min_interval_ms = iv;
    return rule;
  }

  // Keep the raw textarea current whenever the form changes (unless the user is editing raw).
  function syncFromForm() { if (rawPanel.open && !rawMode) raw.value = JSON.stringify(buildRule(), null, 2); }

  // Populate the form from a rule object.
  function loadRule(rule) {
    rule = rule && typeof rule === 'object' ? rule : {};
    idInput.value = typeof rule.id === 'string' ? rule.id : '';
    enabled.checked = rule.enabled !== false;
    onInput.value = typeof rule.on === 'string' ? rule.on : '';
    interval.value = typeof rule.min_interval_ms === 'number' ? rule.min_interval_ms : '';
    ifRows.replaceChildren();
    const ifObj = (rule.if && typeof rule.if === 'object' && !Array.isArray(rule.if)) ? rule.if : {};
    for (const field of Object.keys(ifObj)) {
      const pred = ifObj[field];
      if (pred && typeof pred === 'object' && !Array.isArray(pred)) {
        const op = Object.keys(pred)[0] || 'eq';
        ifRows.append(ifRow(field, op, pred[op]));
      } else {
        ifRows.append(ifRow(field, 'eq', pred));
      }
    }
    doRows.replaceChildren();
    const doArr = Array.isArray(rule.do) ? rule.do : [];
    for (const a of doArr) doRows.append(doRow(a && typeof a === 'object' ? a : { notify: {} }));
    if (rawPanel.open) raw.value = JSON.stringify(buildRule(), null, 2);
    testTopic.value = onInput.value;
  }

  // ---------------------------------------------------------------- actions
  async function refresh(keep) {
    try {
      const r = await api.automationList();
      rules = Array.isArray(r.rules) ? r.rules : [];
    } catch (e) { rules = []; setStatus(errorText(e), true); }
    if (keep !== undefined) current = keep;
    fillSelect();
  }

  async function open(id) {
    if (!id) { newRule(); return; }
    setBusy(true);
    try {
      const r = await api.automationRead(id);
      loadRule(r.rule || {});
      current = id;
      setTitle?.(id);
      setStatus(`Loaded ${id}`);
    } catch (e) { setStatus(errorText(e), true); }
    finally { setBusy(false); }
  }

  function newRule() {
    current = '';
    loadRule({ enabled: true, on: 'battery', do: [{ notify: { title: '' } }] });
    select.value = '';
    setTitle?.('new rule');
    setStatus('New rule — set an id, topic and actions, then Save');
    idInput.focus();
  }

  async function save() {
    let rule;
    if (rawPanel.open && rawMode) {
      try { rule = JSON.parse(raw.value); } catch (e) { setStatus('Raw JSON is invalid — fix it or apply to the form', true); return; }
    } else rule = buildRule();
    const id = typeof rule.id === 'string' ? rule.id.trim() : '';
    if (!idValid(id)) { setStatus('Id must be 1–48 of letters/digits/._- and no ..', true); idInput.focus(); return; }
    if (!rule.on) { setStatus('Pick a trigger topic', true); onInput.focus(); return; }
    rule.id = id;
    setBusy(true);
    try {
      const r = await api.automationSave(rule);
      await refresh(id);
      setTitle?.(id);
      toast(`Saved ${id} (${r.size} B)`);
      setStatus(`Saved ${id}`);
    } catch (e) { setStatus(errorText(e), true); toast(errorText(e, 'automation'), 'error'); }
    finally { setBusy(false); }
  }

  async function remove() {
    if (!current) { setStatus('Nothing saved to delete', true); return; }
    if (!window.confirm(`Delete ${current}?`)) return;
    setBusy(true);
    try {
      const gone = current;
      await api.automationDelete(gone);
      await refresh('');
      newRule();
      setStatus(`Deleted ${gone}`);
    } catch (e) { setStatus(errorText(e), true); }
    finally { setBusy(false); }
  }

  async function runTest() {
    let d = {};
    const dtext = testData.value.trim();
    if (dtext) { try { d = JSON.parse(dtext); } catch { testOut.replaceChildren(h('div', { class: 'pair-error' }, 'Event data is not valid JSON.')); return; } }
    const rule = buildRule();
    const topic = testTopic.value.trim();
    btnTest.disabled = true;
    try {
      const r = await api.automationTest(rule, { topic, d });
      renderTest(r);
    } catch (e) { testOut.replaceChildren(h('div', { class: 'pair-error' }, errorText(e))); }
    finally { btnTest.disabled = false; }
  }

  function badge(label, ok) { return h('span', { class: `tag ${ok ? 'ok' : 'alert'}` }, `${label}: ${ok ? 'yes' : 'no'}`); }
  function renderTest(r) {
    const plan = (r.actions || []).map((a, i) => {
      if (a.notify) return `${i + 1}. notify "${a.notify.title || ''}"${a.notify.body ? ` — ${a.notify.body}` : ''}`;
      if (a.term !== undefined) return `${i + 1}. term: ${a.term}`;
      if (a.script !== undefined) return `${i + 1}. script: ${a.script}`;
      return `${i + 1}. ${JSON.stringify(a)}`;
    });
    testOut.replaceChildren(
      h('div', { class: 'auto-badges' },
        badge('match', r.match), badge('topic', r.topic_match), badge('if', r.if_match),
        h('span', { class: `tag ${r.enabled ? 'ok' : 'warn'}` }, `enabled: ${r.enabled ? 'yes' : 'no'}`)),
      r.match
        ? h('div', {}, h('div', { class: 'muted', style: { fontSize: '12px', marginBottom: '4px' } }, plan.length ? 'Would run:' : 'Matches, but no actions defined.'),
          h('div', { class: 'auto-plan' }, plan.join('\n')))
        : h('div', { class: 'muted', style: { fontSize: '12px' } },
          !r.topic_match ? 'Topic does not match the trigger.' : 'Topic matches but a condition failed.'));
  }

  // ---------------------------------------------------------------- wiring
  select.addEventListener('change', () => { if (!busy) open(select.value); });
  btnNew.addEventListener('click', () => { if (!busy) newRule(); });
  btnSave.addEventListener('click', () => { if (!busy) save(); });
  btnDelete.addEventListener('click', () => { if (!busy) remove(); });
  btnAddIf.addEventListener('click', () => { ifRows.append(ifRow()); syncFromForm(); });
  btnAddDo.addEventListener('click', () => { doRows.append(doRow()); syncFromForm(); });
  [idInput, onInput, interval].forEach((x) => x.addEventListener('input', syncFromForm));
  enabled.addEventListener('change', syncFromForm);
  raw.addEventListener('input', () => { rawMode = true; });
  btnApplyRaw.addEventListener('click', () => {
    rawErr.textContent = '';
    let rule;
    try { rule = JSON.parse(raw.value); } catch (e) { rawErr.textContent = 'Invalid JSON.'; return; }
    if (!rule || typeof rule !== 'object' || Array.isArray(rule)) { rawErr.textContent = 'Rule must be a JSON object.'; return; }
    loadRule(rule); rawMode = false; setStatus('Applied JSON to form');
  });
  btnTest.addEventListener('click', () => runTest());
  btnTestFill.addEventListener('click', () => { testTopic.value = onInput.value.trim().replace(/\*$/, ''); });

  setTitle?.('Automation');
  refresh().then(() => { if (rules.length) open(rules[0].id); else newRule(); });

  return { unmount() {} };
}
