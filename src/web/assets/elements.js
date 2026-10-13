'use strict';
// The Look tab of a key or strip zone: font and italic, with where the default comes from, and its
// parts (the texts, icon and graph the module draws). No top-level name may repeat another script's.

const ROWS = [['top', 'Top'], ['middle', 'Middle'], ['bottom', 'Bottom'], ['line1', 'Upper line'], ['line2', 'Lower line']];
const ALIGNS = [['left', 'Left'], ['center', 'Center'], ['right', 'Right']];
const COLOR_MODES = [['fixed', 'One color'], ['state', 'By status'], ['gradient', 'Color scale']];
const openParts = new Set();  // '<module>.<part>' unfolded in this page

// The page label of a text role (ROLES, appearance.js); '' for none.
function roleLabel(role) {
  const found = ROLES.find(([key]) => key === role);
  return found ? found[1] : '';
}

// A part as its Size and Weight see it: on a key, the default is its text style's size (kept within
// the part's bounds) and weight when Look › Text styles sets them, else the module's.
function withRoleDefaults(e, kind) {
  const roles = kind === 'key' && e.role && state.doc.appearance && state.doc.appearance.roles;
  const r = (roles && roles[e.role]) || {};
  const out = Object.assign({}, e);
  if (typeof r.size === 'number') out.size = Math.min(e.max, Math.max(e.min, r.size));
  if (typeof r.weight === 'number' && e.kind === 'text') out.weight = r.weight;
  return out;
}

// <select> of [value, label] pairs.
function makeSelect(options, value, label, onchange) {
  return el('select', { 'aria-label': label, onchange: (ev) => onchange(ev.target.value) },
    options.map(([v, text]) => el('option', { value: v, selected: v === value }, text)));
}

// The rows a part may go to: a graph in the middle or bottom row only; the two lines on two-line
// keys only. The daemon refuses the others: the page never offers them.
function rowsFor(info, e) {
  if (e.kind === 'graph') return ROWS.filter(([r]) => r === 'middle' || r === 'bottom');
  return ROWS.filter(([r]) => info.two_line || (r !== 'line1' && r !== 'line2'));
}

// A new module on a key keeps the slot's font and italic; part sizes belonged to the old module.
function keepLook(old, id) {
  const slot = { module: id, settings: {} };
  if (old && old.font) slot.font = old.font;
  if (old && typeof old.italic === 'boolean') slot.italic = old.italic;
  return slot;
}

// Sets (or, with null, removes) one property of the selected slot: font, italic, or one property of
// one part's override; emptied objects are dropped.
function setLook(prop, value, part, opts) {
  const v = { kind: state.view.kind, index: state.view.index };
  edit((d) => {
    const slot = listOf(d, v.kind)[v.index];
    if (!slot || !slot.module) return;
    if (!part) {
      if (value == null) delete slot[prop];
      else slot[prop] = value;
      return;
    }
    const all = Object.assign({}, slot.elements);
    const one = Object.assign({}, all[part]);
    if (value == null) delete one[prop];
    else one[prop] = value;
    if (Object.keys(one).length) all[part] = one;
    else delete all[part];
    if (Object.keys(all).length) slot.elements = all;
    else delete slot.elements;
  }, Object.assign({ merge: `${slotPath(v)}.${part || ''}.${prop}` }, opts));
}

// "from Look › Text styles › Value", with a link to that place.
function provenance(text, href, place) {
  return el('small', { class: 'prov' }, text, href ? el('a', { href }, place) : null);
}

function slotLook(info, slot) {
  if (!info) return [el('p', { class: 'empty-state' }, 'Choose a module first, in Content.')];
  const v = state.view;
  const what = KIND_LABEL[v.kind].toLowerCase();
  const global = (state.doc.appearance && state.doc.appearance.font) || DEFAULT_FONT;
  const globalItalic = !!(state.doc.appearance && state.doc.appearance.italic);
  const fontReset = resetButton('font', () => setLook('font', null, '', { structural: true }), !!slot.font);
  const font = fontInput(slot.font || '', `Default (${global})`, (name) => {
    setLook('font', name || null, '', { delay: 0 });
    showReset(fontReset, !!name);
  }, 'slot-font', `${slotPath(v)}.font`);
  const italic = makeSelect([['', `Default (${globalItalic ? 'on' : 'off'})`], ['on', 'On'], ['off', 'Off']],
    typeof slot.italic === 'boolean' ? (slot.italic ? 'on' : 'off') : '', 'Italic',
    (x) => setLook('italic', x === '' ? null : x === 'on', '', { delay: 0 }));
  italic.setAttribute('data-f', 'slot-italic');
  italic.setAttribute('data-path', `${slotPath(v)}.italic`);
  return [el('section', { class: 'block' }, el('h3', {}, 'Font'),
    row('slot-font', 'Font', font.input, { show: font.box, reset: fontReset, changed: !!slot.font,
      extra: slot.font ? provenance(`Set on this ${what}.`) : provenance(`${v.kind === 'key' ? 'Text styles may set their own; else ' : ''}from `, '#look/font', 'Look › Default font') }),
    row('slot-italic', 'Italic', italic, { changed: typeof slot.italic === 'boolean',
      extra: typeof slot.italic === 'boolean' ? provenance(`Set on this ${what}.`) : provenance('from ', '#look/font', 'Look › Default font') })),
  partsTable(info, slot, v.kind)];
}

function partsTable(info, slot, kind) {
  const parts = info.elements || [];
  if (!parts.length) return null;
  const over = slot.elements || {};
  return el('section', { class: 'block' }, el('h3', {}, 'Parts'),
    el('p', { class: 'note' }, 'Show, move, resize or recolor each part of the display. A dot marks a changed part.'),
    el('div', { class: 'parts' }, parts.map((e) => partEntry(info, withRoleDefaults(e, kind), over[e.key] || {}, kind))));
}

// "Middle · Center · 36 px · Bold · By status": the part's look in one line, folded.
function partSummary(e, over) {
  const pick = (list, value) => (list.find(([k]) => k === value) || [value, value])[1];
  const out = [];
  if (over.visible === false || (over.visible == null && !e.visible)) return 'Hidden';
  if (e.kind !== 'graph') out.push(pick(ROWS, over.row || e.row));
  if (e.kind === 'text' || e.kind === 'icon') out.push(pick(ALIGNS, over.align || e.align), `${over.size != null ? over.size : Math.round(e.size)} px`);
  if (e.kind === 'text') out.push(pick(WEIGHTS.map(([w, n]) => [w, n]), over.weight || e.weight));
  const color = typeof over.color === 'string' ? over.color : e.color_mode;
  out.push(color.startsWith('#') ? 'One color' : pick(COLOR_MODES, color));
  return out.join(' · ');
}

function partEntry(info, e, over, kind) {
  const id = `${info.id}.${e.key}`;
  const path = `${slotPath(state.view)}.elements.${e.key}`;
  const box = el('div', { class: Object.keys(over).length ? 'part changed' : 'part' });
  const body = el('div', { class: 'part-body', id: `part-${e.key}`, hidden: !openParts.has(id) });
  const sum = el('span', { class: 'sum' }, partSummary(e, over));
  const toggle = el('button', { type: 'button', class: 'part-toggle', 'data-f': `part-${e.key}`,
    'aria-expanded': String(openParts.has(id)), 'aria-controls': body.id }, el('span', {}, e.label), sum);
  const fill = () => body.replaceChildren(...partControls(info, e, over, kind, path, set));
  const set = (prop, value, opts) => {
    setLook(prop, value, e.key, opts);
    const now = (slotOf(state.view).elements || {})[e.key] || {};
    box.classList.toggle('changed', Object.keys(now).length > 0);
    showReset(reset, Object.keys(now).length > 0);
    sum.textContent = partSummary(e, now);
    over = now;
  };
  toggle.addEventListener('click', () => {
    const open = body.hidden;
    body.hidden = !open;
    toggle.setAttribute('aria-expanded', String(open));
    if (open) {
      openParts.add(id);
      fill();
    } else {
      openParts.delete(id);
    }
  });
  if (openParts.has(id)) fill();
  const shown = el('input', { type: 'checkbox', class: 'switch', 'aria-label': `Show ${e.label}`, 'data-f': `show-${e.key}`,
    'data-path': `${path}.visible`, checked: over.visible != null ? over.visible : e.visible,
    onchange: (ev) => set('visible', ev.target.checked === e.visible ? null : ev.target.checked, { delay: 0 }) });
  const reset = resetButton(e.label, () => {
    const v = { kind: state.view.kind, index: state.view.index };
    const point = undoPoint(state.doc, v.kind, v.index);
    edit((d) => {
      const slot = listOf(d, v.kind)[v.index];
      if (!slot || !slot.elements) return;
      delete slot.elements[e.key];
      if (!Object.keys(slot.elements).length) delete slot.elements;
    }, { structural: true });
    toast(`${e.label}: back to the module's look.`, [['Undo', () => restoreSlot(point)]]);
  }, Object.keys(over).length > 0);
  const chip = e.role && kind === 'key' ? el('a', { class: 'role-chip', href: `#look/styles/${e.role}`,
    title: 'Shared text style, set in Look › Text styles' }, `${roleLabel(e.role)} style`) : el('span');
  box.append(el('div', { class: 'part-head' }, toggle, chip, shown, reset), body);
  return box;
}

// The controls of an unfolded part, each with a visible label.
function partControls(info, e, over, kind, path, set) {
  const out = [];
  const one = (key, label, control, extra) => {
    control.setAttribute('data-f', `p-${e.key}-${key}`);
    control.setAttribute('data-path', `${path}.${key}`);
    out.push(row(`p-${e.key}-${key}`, label, control, { extra }));
  };
  if (kind === 'key') {
    one('row', 'Position', makeSelect(rowsFor(info, e), over.row || e.row, `${e.label}: position`,
      (x) => set('row', x === e.row ? null : x, { delay: 0 })));
    if (e.kind !== 'graph') {
      one('align', 'Align', makeSelect(ALIGNS, over.align || e.align, `${e.label}: align`,
        (x) => set('align', x === e.align ? null : x, { delay: 0 })));
    }
  }
  const role = e.role && kind === 'key' && state.doc.appearance && state.doc.appearance.roles && state.doc.appearance.roles[e.role];
  // Where a value comes from: this part, a shared text style (Look) or the module.
  const from = (here, prop) => (here ? provenance('Set here.') : role && typeof role[prop] === 'number'
    ? provenance('from ', `#look/styles/${e.role}`, `Look › Text styles › ${roleLabel(e.role)}`) : provenance('Module default.'));
  if (e.kind !== 'graph') {
    const size = numberInput({ type: 'int', min: e.min, max: e.max }, over.size != null ? over.size : Math.round(e.size),
      (x, opts) => set('size', x === Math.round(e.size) ? null : x, opts));
    one('size', 'Size (px)', size, from(over.size != null, 'size'));
  }
  if (e.kind === 'text') {
    one('weight', 'Weight', makeSelect(WEIGHTS.map(([w, name]) => [String(w), name]), String(over.weight || e.weight), `${e.label}: weight`,
      (x) => set('weight', Number(x) === e.weight ? null : Number(x), { delay: 0 })), from(over.weight != null, 'weight'));
  }
  const chosen = typeof over.color === 'string' ? over.color : null;
  const mode = chosen === 'state' || chosen === 'gradient' ? chosen : chosen ? 'fixed' : e.color_mode;
  const fixed = (c) => (e.color_mode === 'fixed' && c.toLowerCase() === String(e.color).toLowerCase() ? null : c);
  const picker = el('input', { type: 'color', value: chosen && chosen.startsWith('#') ? chosen : e.color, 'aria-label': `${e.label}: color`,
    'data-f': `p-${e.key}-picker`, 'data-path': `${path}.color`,
    onchange: (ev) => set('color', fixed(ev.target.value), { delay: 0 }) });
  picker.hidden = mode !== 'fixed';
  const modes = makeSelect(COLOR_MODES.filter(([m]) => m !== 'gradient' || e.gradient), mode, `${e.label}: color`, (m) => {
    picker.hidden = m !== 'fixed';
    set('color', m === 'fixed' ? fixed(picker.value) : m === e.color_mode ? null : m, { delay: 0 });
  });
  one('color', 'Color', modes, el('span', { class: 'line' }, picker,
    mode === 'state' ? provenance('', '#look/status', 'Status colors') : null));
  return out;
}
