'use strict';
// lite-deck configuration page, no framework: the config document is edited in memory and saved
// (PUT) shortly after each change. All page scripts share one global scope (tests/test_web_assets.cpp).

const MIN_ZONE = 60;
const MAX_ZONES = 8;
const ZONE_STEP = 10;      // px: a dragged zone border moves by this much
const TEXT_DELAY = 600;    // ms of typing pause before a text or a number is saved
const SLIDER_GAP = 250;    // ms between two saves while a slider moves: 4 per second
const HISTORY_LIMIT = 50;
const KIND_LABEL = { key: 'Key', knob: 'Knob', zone: 'Strip zone' };
const DEVICE_STATE = {
  connected: ['ok', 'Deck connected'],
  simulator: ['ok', 'Simulator'],
  busy: ['warn', 'Deck in use by '],  // + the vendor app's name (GET /api/device)
  disconnected: ['warn', 'Deck disconnected'],
  offline: ['bad', 'lite-deck not responding'],
  full: ['warn', 'Open in one tab only: previews paused'],  // the daemon refused a third page's stream
};
const EXPIRED = 'Session expired: reopen this page from the lite-deck tray icon '
  + '(left click, or right click then "Configure…").';
const NOT_RESPONDING = 'lite-deck is not responding. Is it running in the tray?';
const CONFLICT = 'The configuration was changed outside this page.';

const state = {
  modules: [],
  byId: new Map(),
  device: null,       // the active device (GET /api/device): grid, key size, strip, knobs
  doc: null,
  revision: 0,
  view: { tab: 'slots', kind: '', index: 0, sub: 'content', look: '' },
  subs: { key: 'content', knob: 'content', zone: 'content' },  // the sub-tab each kind shows
  picking: false,     // the module catalogue is open on a slot that has a module
  focusPath: '',      // a field to focus once the panel is drawn (an error link)
  errors: [],         // refused by the last save (422)
  fileErrors: [],     // config.json edited by hand and refused by the daemon
  dirty: false,       // the document holds changes the daemon does not have
  rejected: false,    // the last save was refused (422): wait for the next change
  conflict: 0,        // the revision that replaced ours (412): the user chooses
  structural: false,  // the pending change alters the layout or a module
  saving: false,
  saveTimer: 0,
  retryTimer: 0,
  loads: 0,           // documents loaded from the daemon: an Undo never crosses a reload
  dead: false,        // session expired: never reconnect
};
const undoLog = makeHistory(HISTORY_LIMIT);  // not "history": the browser's own global
const saveGate = makeSaveGate(4, 1000);
let events = null;
let retryTimer = 0;
let retryDelay = 1000;

// ---------- DOM helpers ----------

const $ = (id) => document.getElementById(id);

// el('button', { class: 'x', onclick: fn, disabled: true }, ...children). Children go in first, so
// that `value` and `selected` apply to a complete <select>. Text is always text, never markup.
function el(tag, attrs, ...children) {
  const e = document.createElement(tag);
  for (const c of children.flat()) if (c != null && c !== false) e.append(c);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'value') e.value = v;
    else if (k === 'checked') e.checked = true;
    else e.setAttribute(k, v === true ? '' : String(v));
  }
  return e;
}

// A labelled control. Stacked: label above the control; inline (switch, color): on one line. The
// hint, a passing note and the error of the field go under it, tied to the control for screen readers.
function row(id, label, control, opts = {}) {
  control.id = id;
  const help = opts.hint ? el('small', { id: `${id}-hint` }, opts.hint) : null;
  const note = el('small', { class: 'note', id: `${id}-note`, 'aria-live': 'polite' });
  const err = el('p', { class: 'field-error', id: `${id}-err` });
  control.setAttribute('aria-describedby', [help && help.id, err.id].filter(Boolean).join(' '));
  const shown = opts.show || control;
  return el('div', { class: `${opts.inline ? 'row inline' : 'row'}${opts.changed ? ' changed' : ''}` },
    el('div', { class: 'line' }, el('label', { for: id }, label), opts.inline ? shown : null, opts.reset || null),
    opts.inline ? null : shown, opts.extra || null, help, note, err);
}

// "↺": shown only while the value differs from its default (the space stays, so columns hold still).
function resetButton(what, onclick, on) {
  return el('button', { type: 'button', class: on ? 'ghost reset' : 'ghost reset off', title: 'Reset',
    'aria-label': `Reset ${what}`, 'data-f': `reset-${what}`, onclick }, '↺');
}

function showReset(button, on) {
  button.classList.toggle('off', !on);
  const r = button.closest('.row');
  if (r) r.classList.toggle('changed', on);
}

function fieldNote(control, text) {
  const n = $(`${control.id}-note`);
  if (!n) return;
  n.textContent = text;
  clearTimeout(n.timer);
  n.timer = setTimeout(() => { n.textContent = ''; }, 2500);
}

// actions: [[label, fn], ...]. A toast with actions stays 10 s, or until one is used; `sticky`: until then.
// While a conflict waits for the user's choice, its toast stays: no other toast replaces it.
// A live region (role=status): emptied, never hidden, so that screen readers read it as it fills;
// a sticky toast shown again with the same text is not read again. `said`: the alert (announce)
// already says the text, so only the buttons are read here.
function toast(text, actions, sticky, said) {
  const t = $('toast');
  if (state.conflict && !sticky) return;
  if (sticky && toast.sticky === text && t.children.length) return;
  clearTimeout(toast.timer);
  const hide = () => { toast.sticky = ''; t.replaceChildren(); };
  t.replaceChildren(el('span', { 'aria-hidden': said ? 'true' : null }, text), ...(actions || []).map(([label, fn]) => el('button', {
    type: 'button',
    onclick: () => { hide(); fn(); },
  }, label)));
  toast.sticky = sticky ? text : '';
  if (!sticky) toast.timer = setTimeout(hide, actions ? 10000 : 6000);
}

// A refused save, a conflict, the end of the session: said at once by the one alert of the page
// (#alert, role=alert, in the page from the start and never removed). Emptied, then written on the
// next frame, so that the same message twice is said twice.
function announce(text) {
  const a = $('alert');
  if (!a) return;
  a.textContent = '';
  const later = typeof requestAnimationFrame === 'function' ? requestAnimationFrame : (f) => setTimeout(f, 16);
  later(() => { a.textContent = text; });
}

// The session is over: the page stops. Changes not saved yet are shown, to be copied.
function fatal(text) {
  state.dead = true;
  stopEvents();
  if (typedEdit()) typedEdit().blur();  // its text (a font, a secret: committed when left) goes in `kept`
  const kept = state.dirty || state.saving ? JSON.stringify(state.doc, null, 2) : '';
  document.body.replaceChildren(el('main', { class: 'fatal' }, el('h1', {}, 'lite-deck'), el('p', {}, text),
    kept ? el('p', {}, 'These changes were not saved. Copy them before you reload the page:') : null,
    kept ? el('textarea', { readonly: true, rows: 12, 'aria-label': 'Unsaved configuration', value: kept }) : null), $('alert'));
  announce(text);
}

// The save state (role=status, polite): "Saving…", "Saved". A refusal is shown there but not read
// there (aria-hidden): the alert or a toast says it, once.
function setSaveState(kind, text) {
  const s = $('save');
  if (!s) return;
  clearTimeout(setSaveState.timer);
  s.className = `chip ${kind}`;
  if (s.textContent !== text) s.replaceChildren(kind === 'bad' ? el('span', { 'aria-hidden': 'true' }, text) : text);
  if (kind === 'ok') setSaveState.timer = setTimeout(() => { s.textContent = ''; }, 2000);
}

function setDevice(name) {
  const [kind, label] = DEVICE_STATE[name] || DEVICE_STATE.offline;
  const pill = $('device');
  if (!pill) return;
  pill.className = `chip ${kind}`;
  const text = name === 'busy' ? label + ((state.device && state.device.vendor_app) || 'another app') : label;
  if (pill.textContent !== text) pill.textContent = text;  // read once (role=status), not at every check
}

// Rebuilding part of the page keeps the keyboard focus on the control with the same data-f key; if
// that control is gone or disabled, on the panel's current tab or module button. Text typed in the
// focused field and not committed yet (no change event so far) is carried over to the new field,
// only if it edits the same value (same data-path, or the same field with no path: the search box).
function keepFocus(render) {
  const a = document.activeElement;
  const key = a && a.dataset ? a.dataset.f : '';
  const typed = key && a.typed ? { value: a.value, start: a.selectionStart, end: a.selectionEnd, path: a.dataset.path } : null;
  render();
  if (!key || (document.contains(a) && document.activeElement === a)) return;
  let n = document.querySelector(`[data-f="${CSS.escape(key)}"]`);
  if (n && typed && n.tagName === a.tagName && n.type === a.type && n.dataset.path === typed.path) {
    n.value = typed.value;
    n.typed = true;
    try { n.setSelectionRange(typed.start, typed.end); } catch { /* number fields have no selection */ }
    n.dispatchEvent(new Event('input', { bubbles: true }));  // a search box filters again
  }
  if (!n || n.disabled || n.classList.contains('off')) n = document.querySelector('#panel [role=tab][aria-selected=true], #panel [data-f=module]');
  if (n) n.focus({ preventScroll: true });
}

// Which field holds text typed but not committed yet (keepFocus): set on input, cleared on change.
function trackTyping() {
  document.addEventListener('input', (e) => { if (e.target.matches('input:not([type=range]):not([type=color]), textarea')) e.target.typed = true; }, true);
  document.addEventListener('change', (e) => { e.target.typed = false; }, true);
}
// The focused field of the document (data-path) holding text not committed yet: a font, a secret.
const typedEdit = () => { const a = document.activeElement; return a && a.typed && a.dataset && a.dataset.path ? a : null; };

// ---------- the document ----------

const keyCount = () => state.device.rows * state.device.cols;
const stripWidth = () => (state.device.strip ? state.device.strip.width : 800);
const moduleOf = (slot) => (slot && slot.module ? state.byId.get(slot.module) || null : null);
const slotName = (kind, index) => `${KIND_LABEL[kind]} ${index + 1}`;
// How many keys, knobs or zones the page shows (the document may hold more, kept from a bigger deck).
const slotCount = (kind) => (kind === 'key' ? keyCount() : kind === 'knob' ? state.device.knobs
  : state.device.strip ? state.doc.strip.length : 0);

function listOf(d, kind) {
  return kind === 'key' ? d.keys : kind === 'knob' ? d.knobs : d.strip;
}

function slotOf(sel) {
  return sel.kind ? listOf(state.doc, sel.kind)[sel.index] : null;
}

function slotPath(sel) {
  return `${{ key: 'keys', knob: 'knobs', zone: 'strip' }[sel.kind]}[${sel.index}]`;
}

function moduleName(slot) {
  const m = moduleOf(slot);
  return m ? m.name : slot && slot.module ? slot.module : 'Empty';
}

// Every change of the document goes through here: an undo step, the change, a save, the deck.
// opts: structural (layout or module: saved at once, panel redrawn), delay and throttle (ms before
// the save; throttle: a save already on its way is not pushed back), merge (changes with the same
// key less than 1.5 s apart make one undo step). A change that changes nothing makes no step and no save of its own.
function edit(change, opts = {}) {
  if (!state.doc) return;
  const before = JSON.stringify(state.doc);
  change(state.doc);
  const changed = JSON.stringify(state.doc) !== before;
  if (changed) undoLog.record(before, opts.merge || '', Date.now());
  // Nothing new to send; a save still waiting is moved to this change's time (a field left: now).
  if (changed || state.saveTimer) scheduleSave(!!opts.structural, opts.delay, opts.throttle);
  keepFocus(() => {
    patchDeck();
    if (opts.structural) renderPanel();
  });
  renderUndo();
}

// Up to `limit` earlier documents (JSON text), kept in the page only (tests/web/history.test.js).
function makeHistory(limit) {
  let past = [];
  let future = [];
  let lastKey = '';
  let lastAt = 0;
  return {
    record(snapshot, key, now) {
      const merge = key !== '' && key === lastKey && now - lastAt < 1500;
      lastKey = key;
      lastAt = now;
      if (merge) return;
      future = [];
      if (past[past.length - 1] === snapshot) return;
      past.push(snapshot);
      if (past.length > limit) past.shift();
    },
    undo(current) {
      if (!past.length) return null;
      future.push(current);
      lastKey = '';
      return past.pop();
    },
    redo(current) {
      if (!future.length) return null;
      past.push(current);
      lastKey = '';
      return future.pop();
    },
    clear() {
      past = [];
      future = [];
      lastKey = '';
    },
    canUndo: () => past.length > 0,
    canRedo: () => future.length > 0,
  };
}

function undo() { if (state.doc && !state.dead) applyStep(undoLog.undo(JSON.stringify(state.doc))); }
function redo() { if (state.doc && !state.dead) applyStep(undoLog.redo(JSON.stringify(state.doc))); }

// An undo or a redo is saved like any other change (during a conflict: kept until the user chooses).
// The value in the focused field is the one replaced: it is not carried over the redraw.
function applyStep(text) {
  if (text == null) return;
  const a = document.activeElement;
  if (a) a.typed = false;
  state.errors = stepErrors(text);  // marks moved with their slots go back with them (deck.js)
  state.doc = JSON.parse(text);
  state.picking = false;
  scheduleSave(true);
  renderAll();
  renderUndo();
}

function renderUndo() {
  const u = $('undo');
  const r = $('redo');
  if (u) u.disabled = !undoLog.canUndo();
  if (r) r.disabled = !undoLog.canRedo();
}

// The Undo of a toast (module change, zone merge, part reset) puts back what it replaced: a key's or knob's slot,
// and for a zone the whole strip (zones move, split and merge meanwhile: an index alone could hit
// another zone). Pure (tests/web/save.test.js): a snapshot as JSON text, and its application.
function undoPoint(doc, kind, index) {
  const part = kind === 'zone' ? doc.strip : listOf(doc, kind)[index];
  return { kind, index, text: JSON.stringify(part === undefined ? null : part), loads: state.loads };
}

function applyUndo(doc, p) {
  const value = JSON.parse(p.text);
  if (p.kind === 'zone') doc.strip = value;
  else listOf(doc, p.kind)[p.index] = value;
}

// An Undo never crosses a reload of the document from lite-deck. It is a change like any other:
// Ctrl+Z takes it back. One point, or several (a swap) put back in one step.
function restoreSlot(point) {
  const points = [].concat(point);
  if (points[0].loads !== state.loads) { toast('The configuration was reloaded since: nothing to undo.'); return; }
  edit((d) => points.forEach((p) => applyUndo(d, p)), { structural: true });
  patchSelection();
}

// ---------- server ----------

async function api(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.headers['X-Lite-Deck'] = '1';
    if (state.revision) opts.headers['If-Match'] = `"${state.revision}"`;
    opts.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, opts);
  } catch {
    return { status: 0, data: null, revision: 0 };
  }
  let data = null;
  try { data = await res.json(); } catch { /* empty or not JSON */ }
  const etag = res.headers.get('ETag');
  return { status: res.status, data, revision: etag ? Number(etag.replace(/\D/g, '')) : 0 };
}

// onlyIfClean: a reload the user did not ask for, dropped if a change was made during the GET.
async function loadConfig(onlyIfClean) {
  const r = await api('GET', '/api/config');
  if (r.status === 401) { fatal(EXPIRED); return false; }
  if (r.status !== 200) { toast(NOT_RESPONDING); return false; }
  if (onlyIfClean && !mayReload(state)) return false;
  state.doc = r.data;
  state.revision = r.revision;
  state.loads += 1;
  undoLog.clear();  // the steps were made on another document
  renderUndo();
  return true;
}

async function refreshStatus() {
  const r = await api('GET', '/api/status');
  if (r.status === 401) { fatal(EXPIRED); return false; }
  if (r.status !== 200) { setDevice('offline'); return false; }
  setDevice(r.data.device);
  state.fileErrors = r.data.config_errors || [];
  if (r.data.revision !== state.revision && mayReload(state) && (await loadConfig(true))) renderAll();
  renderErrors();
  return true;
}

// What the answer to a PUT means for the changes it carried (tests/web/save.test.js).
function saveOutcome(status) {
  if (status === 200) return 'saved';
  if (status === 422) return 'invalid';
  if (status === 412) return 'conflict';
  if (status === 401) return 'expired';
  return 'failed';  // 0 (no answer), 5xx, 400, 413
}

// A reload from the daemon replaces the document: never while it holds changes not saved yet.
function mayReload(s) {
  return !s.dirty && !s.saving && !s.conflict;
}

// Token bucket: `burst` saves at once, then one every `every` ms (tests/web/save.test.js).
function makeSaveGate(burst, every) {
  let tokens = burst;
  let at = 0;
  const fill = (now) => {
    tokens = Math.min(burst, tokens + (now - at) / every);
    at = now;
  };
  return {
    wait(now) { fill(now); return tokens >= 1 ? 0 : Math.ceil((1 - tokens) * every); },
    take(now) { fill(now); tokens -= 1; },
  };
}

function scheduleSave(structural, delay, throttle) {
  state.dirty = true;
  state.rejected = false;
  if (structural) state.structural = true;
  if (state.conflict) { showConflict(); return; }  // nothing is sent before the user chooses
  setSaveState('pending', 'Saving…');
  if (throttle && state.saveTimer && !structural) return;  // a save is already on its way
  clearTimeout(state.saveTimer);
  state.saveTimer = setTimeout(save, structural ? 0 : delay == null ? 350 : delay);
}

async function save() {
  clearTimeout(state.saveTimer);
  state.saveTimer = 0;
  if (state.dead) return;
  if (state.conflict) { showConflict(); return; }
  if (state.saving) { state.saveTimer = setTimeout(save, 150); return; }
  if (!state.dirty) return;
  const wait = saveGate.wait(Date.now());
  if (wait > 0) { state.saveTimer = setTimeout(save, wait); return; }
  saveGate.take(Date.now());
  state.saving = true;
  state.dirty = false;
  const structural = state.structural;
  state.structural = false;
  const r = await api('PUT', '/api/config', state.doc);
  state.saving = false;
  const outcome = saveOutcome(r.status);
  if (outcome !== 'saved') state.dirty = true;  // the changes still live only in this page
  if (outcome === 'saved') {
    state.revision = r.revision || r.data.revision;
    state.errors = [];
    retrySave.delay = 1000;
    if (!state.dirty) {
      const before = state.doc;
      state.doc = r.data.config;  // canonical form: defaults of a new module filled in
      keepFocus(() => {
        patchDeck();
        if (structural && panelChanged(before, state.doc)) renderPanel();
      });
    }
    if (state.dirty) setSaveState('pending', 'Saving…');  // a newer change follows
    else setSaveState('ok', 'Saved');
  } else if (outcome === 'invalid') {
    state.rejected = true;
    state.errors = (r.data && r.data.errors) || [];
    setSaveState('bad', 'Not saved: see the errors');
    const n = state.errors.length;
    announce(`Not saved: ${n === 1 ? '1 error' : `${n} errors`}${n ? `. ${humanError(state.errors[0])}` : ''}`);
  } else if (outcome === 'conflict') {
    state.conflict = r.revision || 1;
    announce(`Not saved: ${CONFLICT}`);  // the toast's buttons are read with it
    showConflict();
  } else if (outcome === 'expired') {
    fatal(EXPIRED);
    return;
  } else if (r.status === 400 || r.status === 413) {  // sending it again cannot help
    state.rejected = true;
    setSaveState('bad', 'Not saved');
    const why = `Could not save: lite-deck refused the configuration (error ${r.status}).`;
    announce(why);
    toast(why, null, false, true);
  } else {
    setSaveState('bad', 'Not saved: retrying…');  // the toast says it
    if ((retrySave.delay || 1000) <= 1000) toast(r.status ? `Could not save (error ${r.status}). Retrying…` : `${NOT_RESPONDING} Retrying…`);
    retrySave();
  }
  renderErrors();
  patchDeck();
}

// A save that got no answer is sent again later, and on every reconnection.
function retrySave() {
  retrySave.delay = Math.min((retrySave.delay || 1000) * 2, 15000);
  clearTimeout(state.retryTimer);
  state.retryTimer = setTimeout(() => {
    state.retryTimer = 0;
    if (state.dirty && !state.rejected && !state.saving) save();
  }, retrySave.delay);
}

// The conflict (412) choice, shown again at every change until the user makes it.
function showConflict() {
  setSaveState('bad', 'Not saved: choose a version');
  toast(CONFLICT, [['Keep my changes', keepMine],
    ['Load the other version', loadTheirs]], true, true);
}

function keepMine() {
  state.revision = state.conflict;  // write over the other version
  state.conflict = 0;
  scheduleSave(true);
}

async function loadTheirs() {
  if (!(await loadConfig())) {  // the conflict stays: the user chooses again
    if (!state.dead) showConflict();
    return;
  }
  state.conflict = 0;
  state.dirty = false;
  state.errors = [];
  renderAll();
  setSaveState('ok', 'Reloaded');
}
// ---------- errors ----------

// Where an error of PUT /api/config points (tests/web/errors.test.js): keys[3].settings with the
// setting named first in the message, keys[3].elements.value.size, strip[1].width, the strip total,
// appearance.* (Look) and device.* (Hardware). Indexes stay 0-based here.
function errorTarget(e) {
  const p = String(e.path || '');
  const m = /^(keys|knobs|strip)\[(\d+)\](?:\.(.*))?$/.exec(p);
  if (m) {
    const kind = { keys: 'key', knobs: 'knob', strip: 'zone' }[m[1]];
    let field = m[3] || '';
    if (field === 'settings') {
      const k = /^([\w.]+):/.exec(String(e.message || ''));
      if (k) field = `settings.${k[1]}`;
    }
    return { tab: 'slots', kind, index: Number(m[2]), field, path: field ? `${m[1]}[${m[2]}].${field}` : p };
  }
  if (p === 'strip') return { tab: 'slots', kind: 'zone', index: -1, field: 'width', path: p };
  if (p.startsWith('appearance')) return { tab: 'look', kind: '', index: -1, field: p, path: p };
  if (p.startsWith('device')) return { tab: 'hardware', kind: '', index: -1, field: p, path: p };
  return { tab: '', kind: '', index: -1, field: '', path: p };
}

const PLAIN_ERRORS = [
  [/^value too large \(max (.+)\)$/, 'too large (max $1)'],
  [/^value too small \(min (.+)\)$/, 'too small (min $1)'],
  [/^integer expected$/, 'a whole number is needed'],
  [/^number expected$/, 'a number is needed'],
  [/^value not allowed.*$/, 'choose one of the listed options'],
  [/^color #rrggbb expected$/, 'a color is needed'],
  [/^module (\S+) cannot be placed here$/, 'this module does not fit here'],
  [/^the widths must add up to (\d+) \(currently (\d+)\)$/, 'zone widths add up to $2 px; the strip needs $1 px'],
];

// One error in plain words, numbered from 1 like the rest of the page:
// "Key 4 › Warning at (%): too large (max 100)". A setting the module does not label keeps its key.
function humanError(e) {
  const t = errorTarget(e);
  let msg = String(e.message || '');
  const where = [];
  if (t.kind && t.index >= 0) {
    where.push(slotName(t.kind, t.index));
    if (t.field.startsWith('settings.')) {
      const info = moduleOf(slotOf(t));
      const f = info && info.schema ? info.schema.fields.find((x) => x.key === t.field.slice(9)) : null;
      where.push(f ? f.label || f.key : t.field.slice(9));
      msg = msg.replace(/^[\w.]+:\s*/, '');
    }
  } else if (t.kind === 'zone') {
    where.push('Strip');
  } else if (t.tab === 'look') {
    where.push('Look');
  } else if (t.tab === 'hardware') {
    where.push('Hardware');
  }
  for (const [re, words] of PLAIN_ERRORS) if (re.test(msg)) { msg = msg.replace(re, words); break; }
  return where.length ? `${where.join(' › ')}: ${msg}` : msg;
}

// ---------- navigation ----------

// '#slots/key-4/look' <-> { tab, kind, index (0-based), sub }; '#look/styles/value' -> { tab, look }.
// Numbers in the address start at 1, like the rest of the page (tests/web/route.test.js).
function parseHash(hash) {
  const [tab, a = '', b = '', c = ''] = String(hash || '').replace(/^#/, '').split('/');
  if (tab === 'look') return { tab, look: [a, b, c].filter(Boolean).join('/') };
  if (tab === 'hardware') return { tab };
  const view = { tab: 'slots' };
  const m = /^(key|knob|zone)-(\d{1,2})$/.exec(a);
  if (m && Number(m[2]) >= 1) {
    view.kind = m[1];
    view.index = Number(m[2]) - 1;
  }
  if (['content', 'look', 'tap', 'ring'].includes(b)) view.sub = b;
  return view;
}

function formatHash(v) {
  if (v.tab === 'look') return v.look ? `#look/${v.look}` : '#look';
  if (v.tab === 'hardware') return '#hardware';
  if (!v.kind) return '#slots';
  return `#slots/${v.kind}-${v.index + 1}${v.sub ? `/${v.sub}` : ''}`;
}

function route() {
  // Text typed and not committed yet goes to the slot it was typed in, before the view changes.
  const a = document.activeElement;
  if (a && a.typed) a.blur();
  const p = parseHash(location.hash);
  const v = state.view;
  const moved = p.kind !== v.kind || p.index !== v.index;
  v.tab = p.tab;
  v.look = p.look || '';
  if (p.tab === 'slots') {
    v.kind = p.kind || '';
    v.index = p.kind ? p.index : 0;
    v.sub = p.sub || state.subs[v.kind] || 'content';
  }
  if (moved) state.picking = false;
  clampView();
  if (v.kind && v.tab === 'slots') state.subs[v.kind] = v.sub;
  render();
}

// Changes the view through the address bar, so that back and forward work. `replace`: no new
// history entry (arrow keys on the deck).
function go(changes, replace) {
  const v = Object.assign({}, state.view, changes);
  if (changes.kind && !changes.sub) v.sub = state.subs[changes.kind] || 'content';
  const hash = formatHash(v);
  if (hash === location.hash) return;
  if (replace) {
    window.history.replaceState(null, '', hash);
    route();
  } else {
    location.hash = hash;
  }
}

// The document may have shrunk (an undo, an external edit): keep the view on something that exists.
function clampView() {
  const v = state.view;
  if (!v.kind) return;
  if (!(v.index >= 0 && v.index < slotCount(v.kind))) {
    v.kind = '';
    v.index = 0;
  }
  if (v.kind && !subTabsFor(v.kind, slotOf(v)).some(([id]) => id === v.sub)) v.sub = 'content';
}

function renderAll() {
  clampView();
  keepFocus(() => {
    patchDeck();
    render();
  });
}

function render() {
  if (!state.doc) return;
  for (const t of ['slots', 'look', 'hardware']) {
    const a = $(`tab-${t}`);
    if (state.view.tab === t) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  }
  patchSelection();
  renderPanel();
}

function renderPanel() {
  clampView();  // a zone merged away, a document replaced: never draw a slot that is gone
  const v = state.view;
  const info = v.tab === 'slots' ? moduleOf(slotOf(v)) : null;
  if (info && info.hue) document.documentElement.style.setProperty('--hue', info.hue);
  else document.documentElement.style.removeProperty('--hue');
  keepFocus(() => {
    $('panel').replaceChildren(...(v.tab === 'look' ? lookPanel() : v.tab === 'hardware' ? hardwarePanel() : slotPanel()));
  });
  renderErrors();
  if (state.focusPath) {
    const c = $('panel').querySelector(`[data-path="${CSS.escape(state.focusPath)}"]`);
    state.focusPath = '';
    if (c) c.focus();
  } else if (v.tab === 'look' && v.look) {
    const sec = $('panel').querySelector(`[data-look="${CSS.escape(v.look)}"]`);
    if (sec) sec.scrollIntoView({ block: 'nearest' });
  }
}

// ---------- keyboard ----------

// Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) undo and redo the changes of the page, Ctrl+C / Ctrl+V copy and
// paste the selected slot; not in a field you type in, which keeps its own. Lists, switches, sliders
// and colors have none: there, they work.
function onKey(e) {
  const t = e.target;
  const typing = t && t.matches && t.matches('textarea, input:not([type=checkbox]):not([type=radio]):not([type=range]):not([type=color]):not([type=button])');
  if (!(e.ctrlKey || e.metaKey) || e.altKey || typing || state.dead) return;
  const k = String(e.key || '').toLowerCase();
  if (k === 'z' && !e.shiftKey) undo();
  else if (k === 'y' || (k === 'z' && e.shiftKey)) redo();
  else if (k === 'c' && !String(window.getSelection())) copySelected();
  else if (k === 'v') pasteSelected();
  else return;
  e.preventDefault();
}

// ---------- live events ----------

function stopEvents() {
  clearTimeout(retryTimer);
  retryTimer = 0;
  if (events) { events.close(); events = null; }
}

// A browser does not reconnect after a non-200 answer (a reload can get a 503 while the old stream
// lingers): retry by hand with a growing delay, never while the tab is hidden.
function retryLater() {
  if (document.hidden || retryTimer) return;
  retryTimer = setTimeout(() => { retryTimer = 0; connect(); }, retryDelay);
  retryDelay = Math.min(retryDelay * 2, 15000);
}

function connect() {
  if (events || document.hidden || state.dead) return;
  const source = new EventSource('/api/events');
  events = source;
  source.addEventListener('open', () => {
    retryDelay = 1000;
    refreshStatus();  // reloads the document if its revision changed meanwhile, and only then
    if (connect.opened) refreshAll();
    else connect.opened = true;  // the first open: init() has just loaded every preview
    if (state.dirty && !state.rejected && !state.saving) save();  // a save lost on the way: again
  });
  source.addEventListener('error', () => {
    setDevice('offline');
    if (source.readyState !== EventSource.CLOSED) return;  // the browser retries by itself
    if (events === source) events = null;
    refreshStatus().then((up) => { if (up) setDevice('full'); });  // refused while the daemon answers
    retryLater();
  });
  source.addEventListener('frame', (e) => {
    const d = JSON.parse(e.data);
    d.keys.forEach((i) => refresh(`key/${i}`));
    if (d.strip) refresh('strip/0');
  });
  source.addEventListener('device', (e) => setDevice(JSON.parse(e.data).state));
  source.addEventListener('config', async (e) => {
    const d = JSON.parse(e.data);
    state.fileErrors = d.errors || [];
    if (d.revision !== state.revision && mayReload(state) && (await loadConfig(true))) renderAll();
    renderErrors();
  });
}

// A hidden tab closes its stream (nothing is encoded for a page nobody looks at) and sends what
// is waiting at once.
function onVisibility() {
  if (!document.hidden) { connect(); return; }
  stopEvents();
  if (state.saveTimer && !state.saving) {
    clearTimeout(state.saveTimer);
    save();
  }
}

// Closing the tab with changes not saved yet (or text typed and not committed) asks first.
function onLeave(e) {
  if (!state.dirty && !state.saving && !typedEdit()) return;
  e.preventDefault();
  e.returnValue = '';
}

async function init() {
  const m = await api('GET', '/api/modules');
  if (m.status === 401) { fatal(EXPIRED); return; }
  if (m.status !== 200) { fatal(NOT_RESPONDING); return; }
  state.modules = m.data;
  state.byId = new Map(m.data.map((x) => [x.id, x]));
  const dv = await api('GET', '/api/device');
  if (dv.status === 401) { fatal(EXPIRED); return; }
  if (dv.status !== 200) { fatal(NOT_RESPONDING); return; }
  state.device = dv.data;
  if (!(await loadConfig())) return;
  buildDeck();
  const top = document.querySelector('.top');
  new ResizeObserver(() => document.documentElement.style.setProperty('--header-h', `${top.offsetHeight}px`)).observe(top);
  window.addEventListener('hashchange', route);
  window.addEventListener('beforeunload', onLeave);
  document.addEventListener('visibilitychange', onVisibility);
  document.addEventListener('keydown', onKey);
  $('undo').addEventListener('click', undo);
  $('redo').addEventListener('click', redo);
  $('errors-btn').addEventListener('click', toggleErrors);
  $('error-list').addEventListener('click', onErrorLink);
  $('panel').addEventListener('keydown', onPanelKey);
  trackTyping();
  route();
  loadFonts();
  refreshStatus();
  connect();
}

// Node loads this file for its tests (tests/web): no page to start there.
if (typeof document !== 'undefined') init();
