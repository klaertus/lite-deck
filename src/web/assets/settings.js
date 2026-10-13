'use strict';
// The panel of a key, knob or strip zone: breadcrumb, sub-tabs, the module catalogue, the settings
// form (Content and Tap), the ring states (Ring) and the zone tools. The Look tab is in elements.js.

const SUBTABS = {
  key: [['content', 'Content'], ['look', 'Look'], ['tap', 'Tap']],
  knob: [['content', 'Content'], ['ring', 'Ring']],
  zone: [['content', 'Content'], ['look', 'Look']],
};
// Setting groups shown as folding sections; "tap" and "ring" have their own tab.
const GROUPS = [['thresholds', 'Warning levels'], ['gradient', 'Color scale']];
const CATEGORIES = ['System', 'Time', 'Weather & sky', 'Media & sound', 'Actions', 'Online data'];
const EMPTY_HINT = {
  key: 'This key is empty. Pick a module: a clock, a PC monitor, the weather, a program launcher, a shortcut…',
  knob: 'This knob is empty. Pick what turning and pressing it does.',
  zone: 'This strip zone is empty. Pick what it shows.',
};

function subTabsFor(kind, slot) {
  if (kind === 'knob') return SUBTABS.knob;
  return slot && slot.module ? SUBTABS[kind] : SUBTABS[kind].slice(0, 1);
}

function slotPanel() {
  const v = state.view;
  if (!v.kind) return overviewPanel();
  const slot = slotOf(v);
  const info = moduleOf(slot);
  const tabs = subTabsFor(v.kind, slot);
  const tab = tabs.find(([id]) => id === v.sub) || tabs[0];
  const head = el('div', { class: 'panel-head' },
    el('nav', { class: 'crumbs', 'aria-label': 'Breadcrumb' }, `${slotName(v.kind, v.index)} › ${moduleName(slot)} › ${tab[1]}`),
    el('h2', { id: 'panel-h', class: 'title' }, info ? info.name : `${slotName(v.kind, v.index)}: empty`),
    info && info.description ? el('p', { class: 'desc' }, info.description) : null,
    slotActions(v),
    el('div', { class: 'subtabs', role: 'tablist', 'aria-label': `Sections of ${slotName(v.kind, v.index)}` }, tabs.map(([id, label]) => el('button', {
      type: 'button', role: 'tab', id: `sub-${id}`, 'data-f': `sub-${id}`, 'aria-selected': String(id === tab[0]),
      'aria-controls': 'panel-body', tabindex: id === tab[0] ? 0 : -1, onclick: () => go({ sub: id }), onkeydown: tabKeys,
    }, label))));
  const content = tab[0] === 'look' ? slotLook(info, slot) : tab[0] === 'tap' ? tapTab(info, slot)
    : tab[0] === 'ring' ? ringTab(info, slot) : contentTab(info, slot);
  return [head, el('div', { class: 'panel-body', id: 'panel-body', role: 'tabpanel', 'aria-labelledby': `sub-${tab[0]}` },
    el('div', { id: 'slot-errors', class: 'notice bad', hidden: true }), ...content)];
}

// Arrow keys, Home and End in a row of buttons with one Tab stop (the sub-tabs, the Swap with…
// list): the focus moves there, round from end to end. `back` and `forth`: the keys for -1 and +1.
// Returns the button reached, or null for another key.
function roveKeys(e, items, back, forth) {
  const i = items.indexOf(e.target);
  const n = items.length;
  const j = back.includes(e.key) ? (i - 1 + n) % n : forth.includes(e.key) ? (i + 1) % n
    : e.key === 'Home' ? 0 : e.key === 'End' ? n - 1 : -1;
  if (i < 0 || j < 0 || e.altKey || e.ctrlKey || e.metaKey) return null;
  e.preventDefault();
  items.forEach((b, k) => { b.tabIndex = k === j ? 0 : -1; });
  items[j].focus();
  return items[j];
}

// Left and right arrows (Home, End) move between the sub-tabs and show them, as in any tab list;
// like the arrows on the deck, without a new history entry.
function tabKeys(e) {
  const next = roveKeys(e, [...e.currentTarget.parentNode.children], ['ArrowLeft'], ['ArrowRight']);
  if (next) go({ sub: next.dataset.f.slice(4) }, true);
}

// The first view: what is on the deck, and how to start.
function overviewPanel() {
  const items = [];
  for (const kind of ['key', 'zone', 'knob']) {
    for (let i = 0; i < slotCount(kind); i += 1) {
      items.push(el('li', {}, el('a', { href: formatHash({ tab: 'slots', kind, index: i }), 'data-f': `ov-${kind}-${i}` }, slotName(kind, i)),
        ` ${moduleName(listOf(state.doc, kind)[i])}`));
    }
  }
  return [el('div', { class: 'panel-head' }, el('h2', { id: 'panel-h', class: 'title' }, 'Your deck'),
    el('p', { class: 'desc' }, 'Click a key, strip zone or knob to choose what it shows. Changes apply instantly; Ctrl+Z undoes them.')),
  el('div', { class: 'panel-body', id: 'panel-body' }, el('div', { id: 'slot-errors', class: 'notice bad', hidden: true }),
    el('ul', { class: 'overview' }, items))];
}

// Copy, Paste, Swap with… / Move to…, Copy to…: dragging a slot, from the keyboard. The list of
// slots opens under its button (aria-expanded), one Tab stop that the arrow keys move through;
// Escape closes it and goes back to the button.
function slotActions(v) {
  const name = slotName(v.kind, v.index);
  const menu = el('div', { class: 'menu', id: 'slot-menu', hidden: true, role: 'group', onkeydown: (e) => {
    if (roveKeys(e, [...menu.children], ['ArrowUp', 'ArrowLeft'], ['ArrowDown', 'ArrowRight'])) return;
    if (e.key !== 'Escape' || menu.hidden) return;
    e.stopPropagation();
    close();
    $(`act-${menu.copy ? 'copyto' : 'move'}`).focus();
  } });
  const btn = (f, label, onclick, title) => el('button', { type: 'button', class: 'ghost', 'data-f': f, id: f, title, onclick }, label);
  const close = () => {
    menu.hidden = true;
    for (const b of menu.parentNode.querySelectorAll('[aria-expanded]')) b.setAttribute('aria-expanded', 'false');
  };
  const open = (copy, label) => (e) => {
    const shown = !menu.hidden && menu.copy === copy;
    close();
    if (shown) return;
    menu.hidden = false;
    menu.copy = copy;
    menu.setAttribute('aria-label', label);
    e.currentTarget.setAttribute('aria-expanded', 'true');
    menu.replaceChildren(...Array.from({ length: slotCount(v.kind) }, (_, i) => (i === v.index ? ''
      : btn(`to-${i}`, `${slotName(v.kind, i)} (${moduleName(listOf(state.doc, v.kind)[i])})`, () => placeSlot(v.kind, v.index, i, copy)))));
    for (const b of menu.children) b.tabIndex = b === menu.firstElementChild ? 0 : -1;
    menu.firstElementChild.focus();
  };
  const toggle = (f, label, copy, what) => {
    const b = btn(f, label, open(copy, what));
    b.setAttribute('aria-expanded', 'false');
    b.setAttribute('aria-controls', 'slot-menu');
    return b;
  };
  const more = slotCount(v.kind) > 1;
  return el('div', { class: 'head-actions' }, btn('act-copy', 'Copy', copySelected, 'Ctrl+C'),
    clipboard && clipboard.kind === v.kind ? btn('act-paste', 'Paste', pasteSelected, 'Ctrl+V') : null,
    more && (v.kind === 'zone' ? toggle('act-move', 'Move to…', false, `Move ${name} to the place of`)
      : toggle('act-move', 'Swap with…', false, `Swap ${name} with`)),
    more && toggle('act-copyto', 'Copy to…', true, `Copy ${name} to`), menu);
}

// ---------- Content ----------

function contentTab(info, slot) {
  const v = state.view;
  const out = [];
  if (v.kind === 'zone') out.push(zoneTools(v.index));
  out.push(moduleChooser(v.kind, info));
  // Tap fields have their own tab where the kind has one; ring states have the Ring tab.
  const ring = new Set((info && info.ring_states || []).flatMap((r) => [r.color, r.brightness]));
  const tap = SUBTABS[v.kind].some(([id]) => id === 'tap');
  if (info) out.push(settingsBlock(info, slot, (f) => !(tap && f.group === 'tap') && !ring.has(f.key), 'Settings'));
  return out;
}

function moduleChooser(kind, info) {
  const open = !info || state.picking;
  return el('section', { class: 'block' }, el('h3', {}, 'Module'),
    info ? el('div', { class: 'line' }, el('span', {}, info.name), el('button', {
      type: 'button', 'data-f': 'module', 'aria-expanded': String(open),
      onclick: () => { state.picking = !state.picking; renderPanel(); },
    }, open ? 'Cancel' : 'Change module…')) : el('p', { class: 'empty-state' }, EMPTY_HINT[kind]),
    open ? catalogue(kind, info ? info.id : '') : null);
}

// The modules this slot accepts, by category, with a one-line description and a search box. A
// choice is a click or Enter on one entry: moving through the list changes nothing.
function catalogue(kind, current) {
  const list = state.modules.filter((m) => m.slots.includes(kind));
  const search = el('input', { type: 'search', placeholder: 'Search modules', 'aria-label': 'Search modules', 'data-f': 'cat-search', autocomplete: 'off' });
  const box = el('div', { class: 'catalogue' });
  const draw = () => {
    const q = search.value.trim().toLowerCase();
    const hit = (m) => !q || `${m.name} ${m.description || ''} ${m.category || ''}`.toLowerCase().includes(q);
    const cats = [...CATEGORIES, ...new Set(list.map((m) => m.category || 'Other').filter((c) => !CATEGORIES.includes(c)))];
    const items = q || !current ? [] : [catItem(null, kind, current)];
    for (const cat of cats) {
      const own = list.filter((m) => (m.category || 'Other') === cat && hit(m));
      if (own.length) items.push(el('h4', {}, cat), ...own.map((m) => catItem(m, kind, current)));
    }
    if (!items.length) items.push(el('p', { class: 'empty-state' }, 'No module matches.'));
    box.replaceChildren(...items);
  };
  search.addEventListener('input', draw);
  draw();
  return el('div', { class: 'rows' }, list.length > 6 ? search : null, box);
}

function catItem(m, kind, current) {
  const id = m ? m.id : '';
  const b = el('button', { type: 'button', class: 'cat-item', 'data-f': `cat-${id || 'none'}`,
    'aria-current': id === current ? 'true' : null, onclick: () => setModule(id) },
  el('strong', {}, m ? m.name : 'Nothing (empty)'),
  el('small', {}, m ? m.description || '' : `Clears this ${KIND_LABEL[kind].toLowerCase()}.`));
  if (m && m.hue) b.style.setProperty('--cat-hue', m.hue);
  return b;
}

// A new module keeps the slot's font and italic; the rest belonged to the old module, so the
// change can be undone from the toast.
function setModule(id) {
  const v = { kind: state.view.kind, index: state.view.index };
  const before = slotOf(v);
  const old = moduleOf(before);
  const point = undoPoint(state.doc, v.kind, v.index);  // a zone: the whole strip, as it is now
  state.picking = false;
  if ((old ? old.id : '') === id) { renderPanel(); return; }
  edit((d) => {
    if (v.kind === 'zone') {
      const z = d.strip[v.index];
      z.module = id || null;
      if (id) z.settings = {};
      else delete z.settings;
      delete z.elements;
    } else {
      listOf(d, v.kind)[v.index] = id ? keepLook(before, id) : null;
    }
  }, { structural: true });
  const name = slotName(v.kind, v.index);
  if (old) toast(id ? `${name}: ${old.name} replaced by ${state.byId.get(id).name}.` : `${name} cleared (${old.name}).`, [['Undo', () => restoreSlot(point)]]);
}

// ---------- the settings form ----------

// The settings of the module with their defaults filled in.
function effective(info, slot) {
  const out = {};
  for (const f of info.schema.fields) out[f.key] = f.default;
  return Object.assign(out, slot && slot.settings);
}

// [[value, label]] of an enum field: options as strings or { value, label }, or labels in
// option_labels (tests/web/settings.test.js).
function optionList(f) {
  return (f.options || []).map((o, i) => (o !== null && typeof o === 'object'
    ? [String(o.value), String(o.label || o.value)]
    : [String(o), String((f.option_labels && f.option_labels[i]) || o)]));
}

// show_if: { field, equals } or { field, in: [...] }, maybe with and: { … }, or a list of them (any
// one is enough): the field matters only for those values.
function fieldVisible(f, values) {
  return conditionHolds(f.show_if, values);
}

function conditionHolds(c, values) {
  if (Array.isArray(c)) return c.some((x) => conditionHolds(x, values));
  if (!c || typeof c !== 'object') return true;
  const own = Array.isArray(c.in) ? c.in.includes(values[c.field]) : 'equals' in c ? values[c.field] === c.equals : true;
  return own && (c.and ? conditionHolds(c.and, values) : true);
}

function clampNumber(f, n) {
  let x = f.type === 'int' ? Math.round(n) : n;
  if (typeof f.min === 'number') x = Math.max(f.min, x);
  if (typeof f.max === 'number') x = Math.min(f.max, x);
  return x;
}

// The keys of the fields shown: those whose show_if holds, and those an error points at (an error
// is never hidden: its link must land on a field). show_if is not chained: only the value of the
// named field counts, not whether that field is shown itself.
function shownKeys(fields, values, flagged) {
  return new Set(fields.filter((f) => flagged.has(f.key) || fieldVisible(f, values)).map((f) => f.key));
}

// The settings of one slot that errors point at: `prefix` is "keys[3].settings.".
function erroredSettings(errors, prefix) {
  const out = new Set();
  for (const e of errors) {
    const p = errorTarget(e).path;
    if (p.startsWith(prefix)) out.add(p.slice(prefix.length));
  }
  return out;
}

// A fold is drawn only while one of its fields is shown.
function groupShown(own, shown) {
  return own.some((f) => shown.has(f.key));
}

function settingsBlock(info, slot, keep, title) {
  const fields = info.schema.fields.filter(keep);
  if (!fields.length) return null;
  const rows = new Map();
  const boxes = [];
  const values = effective(info, slot);
  const shownNow = (now) => shownKeys(fields, now,
    erroredSettings(state.errors.concat(state.fileErrors), `${slotPath(state.view)}.settings.`));
  const sync = () => {
    const shown = shownNow(effective(info, slotOf(state.view)));
    for (const f of fields) rows.get(f.key).hidden = !shown.has(f.key);
    for (const [box, own] of boxes) box.hidden = !groupShown(own, shown);
  };
  const make = (f) => {
    const r = settingRow(f, values[f.key], sync);
    rows.set(f.key, r);
    return r;
  };
  const first = shownNow(values);
  const loose = fields.filter((f) => !GROUPS.some(([g]) => g === f.group)).map(make);
  const groups = GROUPS.map(([g, label]) => {
    const own = fields.filter((f) => f.group === g);
    if (!own.length) return null;
    const changed = own.some((f) => first.has(f.key) && JSON.stringify(values[f.key]) !== JSON.stringify(f.default));
    const box = groupBox(`${info.id}.${g}`, label, own.map(make), changed);
    boxes.push([box, own]);
    return box;
  });
  sync();
  const block = el('section', { class: 'block' }, el('h3', {}, title), loose.length ? el('div', { class: 'rows' }, loose) : null, groups);
  block.syncShown = sync;  // renderErrors calls it: errors come and go without a redraw
  return block;
}

// A folding group; open or closed as the user left it last time (this browser only).
function groupBox(id, title, rows, changed) {
  const seen = readOpen()[id];
  const box = el('details', { class: 'group', open: seen == null ? changed : seen },
    el('summary', { 'data-f': `group-${id}` }, title, changed ? el('span', { class: 'sum' }, 'changed') : null),
    el('div', { class: 'rows' }, rows));
  box.addEventListener('toggle', () => writeOpen(id, box.open));
  return box;
}

function readOpen() {
  try { return JSON.parse(localStorage.getItem('lite-deck.open') || '{}') || {}; } catch { return {}; }
}

function writeOpen(id, open) {
  try {
    const all = readOpen();
    all[id] = open;
    localStorage.setItem('lite-deck.open', JSON.stringify(all));
  } catch { /* storage off: the group is just not remembered */ }
}

function settingRow(f, value, after) {
  const label = f.label || f.key;
  const differs = (x) => JSON.stringify(x) !== JSON.stringify(f.default);
  let control = null;
  const commit = (x, opts) => {
    setSetting(f.key, x, opts);
    showReset(reset, differs(x));
    after();
  };
  const reset = resetButton(label, () => {
    setControl(control, f, f.default);
    commit(f.default, { delay: 0 });
  }, differs(value));
  control = settingControl(f, value, commit);
  control.setAttribute('data-f', `set-${f.key}`);
  control.setAttribute('data-path', `${slotPath(state.view)}.settings.${f.key}`);
  return row(`f-${f.key}`, label, control, { hint: f.hint, inline: f.type === 'bool' || f.type === 'color', reset, changed: differs(value) });
}

function setControl(c, f, value) {
  if (f.type === 'bool') c.checked = !!value;
  else c.value = value == null ? '' : String(value);
}

// When each kind of control saves: switches, lists and colors (picker closed) at once; text and
// numbers after a 600 ms pause in typing and when the field is left; sliders 4 times a second.
function settingControl(f, value, commit) {
  if (f.type === 'bool') {
    return el('input', { type: 'checkbox', class: 'switch', checked: !!value, onchange: (e) => commit(e.target.checked, { delay: 0 }) });
  }
  if (f.type === 'color') {
    return el('input', { type: 'color', value, onchange: (e) => commit(e.target.value, { delay: 0 }) });
  }
  if (f.type === 'enum') {
    return el('select', { onchange: (e) => commit(e.target.value, { delay: 0 }) },
      optionList(f).map(([o, label]) => el('option', { value: o, selected: o === String(value) }, label)));
  }
  if (f.type === 'int' || f.type === 'number') return numberInput(f, value, commit);
  if (f.type === 'secret') {  // a token or a password: masked, never offered to autofill, saved when left
    return el('input', {
      type: 'password', value: value == null ? '' : String(value), autocomplete: 'off', spellcheck: 'false',
      onchange: (e) => commit(e.target.value, { delay: 0 }),
    });
  }
  return el('input', {
    type: 'text',
    value: value == null ? '' : String(value),
    placeholder: f.placeholder || '',  // an example of what the field takes (schema "placeholder")
    spellcheck: 'false',
    oninput: (e) => commit(e.target.value, { delay: TEXT_DELAY }),
    onchange: (e) => commit(e.target.value, { delay: 0 }),
  });
}

// A number is kept within its bounds on the page. Each keystroke puts the save back by 600 ms, so
// only the value typed last is saved, brought within bounds (the field keeps what was typed until it
// is left, then shows the saved value; a note says so). A burst of typing is one undo step.
function numberInput(f, value, commit) {
  let last = value;
  const input = el('input', { type: 'number', value: value == null ? '' : String(value), min: f.min, max: f.max,
    step: f.type === 'int' ? 1 : 'any', placeholder: f.placeholder || '' });
  const read = () => (input.value === '' || !Number.isFinite(Number(input.value)) ? null : Number(input.value));
  const bounded = (c) => (c === f.max ? `Max ${f.max}: ${c} is saved.` : c === f.min ? `Min ${f.min}: ${c} is saved.` : `Rounded to ${c}.`);
  input.addEventListener('input', () => {
    const n = read();
    if (n === null) {  // nothing to save yet: a save on its way waits for the next keystroke too
      if (state.saveTimer) scheduleSave(false, TEXT_DELAY);
      return;
    }
    const c = clampNumber(f, n);
    if (c !== n) fieldNote(input, bounded(c));
    last = c;
    commit(c, { delay: TEXT_DELAY });
  });
  input.addEventListener('change', () => {
    const n = read();
    if (n === null && f.optional && input.value === '') {  // empty: back to the default
      last = null;
      commit(null, { delay: 0 });
      return;
    }
    if (n === null) {
      input.value = last == null ? '' : String(last);
      fieldNote(input, 'A number is needed: the value is unchanged.');
      return;
    }
    const c = clampNumber(f, n);
    if (c !== n) {
      input.value = String(c);
      fieldNote(input, bounded(c));
    }
    last = c;
    commit(c, { delay: 0 });
  });
  return input;
}

function setSetting(key, value, opts) {
  const v = { kind: state.view.kind, index: state.view.index };
  edit((d) => {
    const slot = listOf(d, v.kind)[v.index];
    if (!slot || !slot.module) return;
    slot.settings = Object.assign({}, slot.settings, { [key]: value });
  }, Object.assign({ merge: `${slotPath(v)}.settings.${key}` }, opts));
}

// ---------- Tap and Ring ----------

function tapTab(info, slot) {
  const form = settingsBlock(info, slot, (f) => f.group === 'tap', 'On tap');
  return [form || el('p', { class: 'empty-state' }, `Nothing to set: a tap on ${info.name} does what its description says.`)];
}

function ringTab(info, slot) {
  const v = state.view;
  const bright = state.doc.device.ring_brightness;
  const scale = el('p', { class: 'note' }, `Brightness here is scaled by Hardware › Ring brightness (${bright} %). `,
    el('a', { href: '#hardware' }, 'Change it'));
  if (!info || !(info.ring_states || []).length) {
    if (!state.device.knob_rings) return [el('p', { class: 'empty-state' }, 'This device has no ring lights.')];
    const color = el('input', { type: 'color', value: state.doc.device.ring_colors[v.index] || '#202020', 'data-f': 'idle-ring',
      'data-path': `device.ring_colors[${v.index}]`,
      oninput: (e) => previewRing(v.index, e.target.value),
      onchange: (e) => edit((d) => { d.device.ring_colors[v.index] = e.target.value; }, { delay: 0 }) });
    return [el('section', { class: 'block' }, el('h3', {}, 'Idle ring color'),
      row('f-idle-ring', 'Ring color', color, { inline: true, hint: 'For a knob with no module. Also in Hardware › Default ring colors.' }), scale)];
  }
  const values = effective(info, slot);
  const lines = info.ring_states.map((r) => {
    const out = el('output', {}, `${values[r.brightness]} %`);
    const field = (key) => info.schema.fields.find((x) => x.key === key) || {};
    const changed = () => {
      const now = effective(info, slotOf(v));
      return now[r.color] !== field(r.color).default || now[r.brightness] !== field(r.brightness).default;
    };
    const reset = resetButton(`${r.label} ring`, () => {
      edit((d) => {
        const s = listOf(d, v.kind)[v.index].settings || {};
        delete s[r.color];
        delete s[r.brightness];
      }, { structural: true });
    }, changed());
    return el('div', { class: 'kv wide' }, el('span', { class: 'label' }, r.label),
      el('input', { type: 'color', value: values[r.color], 'aria-label': `${r.label}: ring color`, 'data-f': `ring-${r.key}`,
        'data-path': `${slotPath(v)}.settings.${r.color}`, onchange: (e) => { setSetting(r.color, e.target.value, { delay: 0 }); showReset(reset, changed()); } }),
      el('input', { type: 'range', min: 0, max: 100, step: 1, value: String(values[r.brightness]), 'aria-label': `${r.label}: brightness`,
        'data-f': `ringb-${r.key}`, 'data-path': `${slotPath(v)}.settings.${r.brightness}`,
        oninput: (e) => {
          out.textContent = `${e.target.value} %`;
          setSetting(r.brightness, Number(e.target.value), { delay: SLIDER_GAP, throttle: true });
          showReset(reset, changed());
        } }), out, reset);
  });
  return [el('section', { class: 'block' }, el('h3', {}, 'Ring colors'), el('div', { class: 'rows' }, lines), scale)];
}

// ---------- strip zone tools (Content tab of a zone) ----------

function zoneTools(i) {
  const zones = state.doc.strip;
  const n = zones.length;
  // Typed like any number (saved after a pause); the panel is redrawn once the field is left.
  const width = numberInput({ type: 'int', min: MIN_ZONE, max: stripWidth() - MIN_ZONE * (n - 1) }, zones[i].width,
    (w, o) => resizeZone(i, w, Object.assign({ merge: `strip[${i}].width`, structural: o.delay === 0 }, o)));
  Object.assign(width.dataset, { f: 'zone-width', path: `strip[${i}].width` });
  const canSplit = n < MAX_ZONES && zones[i].width >= 2 * MIN_ZONE;
  return el('section', { class: 'block' }, el('h3', {}, 'Layout'),
    row('f-zone-width', 'Width (px)', width, { hint: `Strip: ${stripWidth()} px. Each zone ≥ ${MIN_ZONE} px; neighbours adjust. You can also drag the borders on the deck.` }),
    el('div', { class: 'head-actions' },
      el('button', { type: 'button', 'data-f': 'zone-left', 'aria-label': 'Move zone left', disabled: i === 0, onclick: () => placeSlot('zone', i, i - 1, false) }, '← Move'),
      el('button', { type: 'button', 'data-f': 'zone-right', 'aria-label': 'Move zone right', disabled: i === n - 1, onclick: () => placeSlot('zone', i, i + 1, false) }, 'Move →'),
      el('button', { type: 'button', 'data-f': 'zone-split', disabled: !canSplit, title: canSplit ? 'Splits this zone in two' : 'Too narrow to split, or 8 zones already', onclick: () => splitZone(i) }, 'Split zone'),
      el('button', { type: 'button', class: 'danger', 'data-f': 'zone-merge-left', disabled: i === 0, onclick: () => mergeZone(i, -1) }, 'Merge into left'),
      el('button', { type: 'button', class: 'danger', 'data-f': 'zone-merge-right', disabled: i === n - 1, onclick: () => mergeZone(i, 1) }, 'Merge into right')));
}

// Whether the daemon's copy of a saved document changes what the panel shows (settings defaults
// filled in do not): only then is the panel redrawn, otherwise it stays as it is, with the focus
// and what is typed.
function panelChanged(a, b) {
  const v = state.view;
  const part = (d) => {
    if (v.tab === 'look') return d.appearance;
    if (v.tab === 'hardware') return d.device;
    if (!v.kind) return null;
    const slot = listOf(d, v.kind)[v.index];
    const info = moduleOf(slot);
    return info ? Object.assign({}, slot, { settings: effective(info, slot) }) : slot;
  };
  return JSON.stringify(part(a)) !== JSON.stringify(part(b));
}

// ---------- errors (the pure part, errorTarget and humanError, is in app.js) ----------

// Where an error link leads: the slot, on the sub-tab that shows its field (tests/web/errors.test.js).
function errorHref(t) {
  if (t.tab !== 'slots') return t.tab ? formatHash({ tab: t.tab }) : '#slots';
  return formatHash({ tab: 'slots', kind: t.kind, index: Math.max(0, t.index), sub: errorSub(t) });
}

function errorSub(t) {
  if (t.field.startsWith('elements') || t.field === 'font' || t.field === 'italic') return 'look';
  const key = t.field.startsWith('settings.') ? t.field.slice(9) : '';
  const info = key && t.index >= 0 ? moduleOf(listOf(state.doc, t.kind)[t.index]) : null;
  if (!info) return 'content';
  if ((info.ring_states || []).some((r) => r.color === key || r.brightness === key)) return 'ring';
  const f = info.schema.fields.find((x) => x.key === key);
  return f && f.group === 'tap' && SUBTABS[t.kind].some(([id]) => id === 'tap') ? 'tap' : 'content';
}

// The errors of the last save: a header chip ("2 errors") opening a list of links, each message
// under its field (aria-invalid), the others at the top of the panel; the deck marks the slots
// (deck.js). They stay until a save goes through; other fields can be changed meanwhile.
function renderErrors() {
  // The banner (role=alert) is read when it appears or its errors change, not at every check.
  const banner = $('banner');
  const said = JSON.stringify(state.fileErrors);
  banner.hidden = state.fileErrors.length === 0;
  if (banner.said !== said) {
    banner.said = said;
    banner.replaceChildren(
      el('strong', {}, 'config.json has errors, so the last good configuration is still in use. '
        + 'Fix the file, or change anything here to replace it.'),
      el('ul', {}, state.fileErrors.map((e) => el('li', {}, humanError(e)))));
  }
  const n = state.errors.length;
  if (n && !state.conflict && !state.saving && !state.saveTimer) setSaveState('bad', 'Not saved: see the errors');
  const btn = $('errors-btn');
  const list = $('error-list');
  btn.hidden = n === 0;
  btn.textContent = n === 1 ? '1 error' : `${n} errors`;
  if (!n) list.hidden = true;
  btn.setAttribute('aria-expanded', String(!list.hidden));
  list.replaceChildren(el('ul', {}, state.errors.map((e) => {
    const t = errorTarget(e);
    return el('li', {}, el('a', { href: errorHref(t), 'data-path': t.path }, humanError(e)));
  })));
  const panel = $('panel');
  for (const b of panel.querySelectorAll('section.block')) if (b.syncShown) b.syncShown();
  for (const c of panel.querySelectorAll('[aria-invalid=true]')) c.removeAttribute('aria-invalid');
  for (const p of panel.querySelectorAll('.field-error')) p.textContent = '';
  const loose = [];
  for (const e of state.errors) {
    const t = errorTarget(e);
    const c = t.path ? panel.querySelector(`[data-path="${CSS.escape(t.path)}"]`) : null;
    const box = c && c.id ? $(`${c.id}-err`) : null;
    if (c) {
      c.setAttribute('aria-invalid', 'true');
      const group = c.closest('details');
      if (group) group.open = true;
    }
    if (box) box.textContent = humanError(e).replace(/^.*: /, '');
    else if (onScreen(t)) loose.push(humanError(e));
  }
  const top = $('slot-errors');
  if (top) {
    top.hidden = loose.length === 0;
    top.replaceChildren(...loose.map((text) => el('p', {}, text)));
  }
}

// Is the error about what the panel shows? The overview (#slots) shows every slot's errors.
function onScreen(t) {
  const v = state.view;
  if (t.tab !== v.tab) return false;
  return t.tab !== 'slots' || !v.kind || (t.kind === v.kind && (t.index === v.index || t.index === -1));
}

function toggleErrors() {
  const list = $('error-list');
  list.hidden = !list.hidden;
  $('errors-btn').setAttribute('aria-expanded', String(!list.hidden));
  const a = list.querySelector('a');
  if (!list.hidden && a) a.focus();
}

// An error link goes to the slot and tab of the error, then to its field.
function onErrorLink(e) {
  const a = e.target.closest('a[data-path]');
  if (!a) return;
  state.focusPath = a.dataset.path;
  // A part's Size or Color is built only while the part is unfolded.
  const t = errorTarget({ path: a.dataset.path, message: '' });
  const part = t.tab === 'slots' && t.index >= 0 && /^elements\.(\w+)\./.exec(t.field);
  const info = part && moduleOf(listOf(state.doc, t.kind)[t.index]);
  if (info) openParts.add(`${info.id}.${part[1]}`);
  if (a.getAttribute('href') === location.hash) {
    e.preventDefault();
    renderPanel();
  }
}
