'use strict';
// The Look tab (font, text styles, status colors of the whole deck) and the Hardware tab. One
// <datalist> of the installed fonts serves every font field.

const DEFAULT_FONT = 'Segoe UI';
const WEIGHTS = [[400, 'Normal'], [600, 'Semibold'], [700, 'Bold']];
const installedFonts = new Set();  // lower-case family names, from GET /api/fonts (once per page)
// The text roles of the keys, in the order of the Look tab. The keys are the config names
// (role_name in src/render/look_json.cpp; test_web_assets checks them).
const ROLES = [['label', 'Label'], ['value', 'Value'], ['long_value', 'Long value'], ['two_line', 'Two-line'],
  ['secondary', 'Secondary'], ['symbol', 'Symbol']];
const ROLE_EXAMPLES = { label: 'CPU, RAM', value: '42 %', long_value: 'stopwatch', two_line: 'BTC, sunrise',
  secondary: 'upload, target', symbol: '₿, ☀' };
const ROLE_SIZE_MIN = 8;  // px, the bounds the daemon accepts (kMinRoleSize, kMaxRoleSize)
const ROLE_SIZE_MAX = 60;
// The defaults of appearance.states (StatePalette in src/render/look.h; test_web_assets checks them).
const STATE_DEFAULTS = { warning: '#ffb000', critical: '#ff453a', idle: '#8c8c8c', stale: '#5f5f5f' };
const STATE_LABELS = [['warning', 'Warning'], ['critical', 'Critical'], ['idle', 'No data'], ['stale', 'Out of date']];

async function loadFonts() {
  const r = await api('GET', '/api/fonts');
  if (r.status !== 200 || !Array.isArray(r.data)) return;
  r.data.forEach((f) => installedFonts.add(f.toLowerCase()));
  $('font-list').replaceChildren(...r.data.map((f) => el('option', { value: f })));
  for (const input of document.querySelectorAll('input[list=font-list]')) input.dispatchEvent(new Event('fontlist'));
}

// A font name field: the browser filters the installed families (<datalist>) as you type. A name
// missing on this PC is still saved (the config may come from another PC); the daemon then draws
// with Segoe UI, and the field says so.
function fontInput(value, placeholder, set, id, path) {
  const hint = el('small', { class: 'font-hint' });
  const input = el('input', { type: 'text', list: 'font-list', value, placeholder, spellcheck: 'false', autocomplete: 'off',
    'data-f': id, 'data-path': path });
  const check = () => {
    const name = input.value.trim();
    hint.textContent = name && installedFonts.size && !installedFonts.has(name.toLowerCase())
      ? `Not installed on this PC: ${DEFAULT_FONT} is used instead.` : '';
  };
  input.addEventListener('fontlist', check);
  input.addEventListener('change', () => {
    input.value = input.value.trim();
    check();
    set(input.value);
  });
  check();
  return { input, box: el('div', { class: 'font-input' }, input, hint) };
}

function look() {  // looked up at each change: the document may have been replaced
  return state.doc.appearance || { font: DEFAULT_FONT, italic: false };
}

function editLook(change, opts) {
  edit((d) => {
    if (!d.appearance) d.appearance = { font: DEFAULT_FONT, italic: false };
    change(d.appearance);
  }, opts);
}

// `roles` with one field of one role set, or removed with null or '' (back to the default). An
// emptied role, then an emptied table, are dropped: only what the user set is saved. Pure: returns a
// new table, or undefined when nothing is left.
function roleWith(roles, role, prop, value) {
  const all = Object.assign({}, roles);
  const one = Object.assign({}, all[role]);
  if (value == null || value === '') delete one[prop];
  else one[prop] = value;
  if (Object.keys(one).length) all[role] = one;
  else delete all[role];
  return Object.keys(all).length ? all : undefined;
}

// The keys whose module draws a text in `role`, for "used by" and the deck outline.
function roleUsers(role) {
  const out = [];
  state.doc.keys.slice(0, keyCount()).forEach((slot, index) => {
    const m = moduleOf(slot);
    if (m && (m.elements || []).some((e) => e.role === role)) out.push({ kind: 'key', index });
  });
  return out;
}

// The keys and zones with a part colored by status.
function statusUsers() {
  const out = [];
  for (const kind of ['key', 'zone']) {
    listOf(state.doc, kind).slice(0, slotCount(kind)).forEach((slot, index) => {
      const m = moduleOf(slot);
      const over = (slot && slot.elements) || {};
      if (m && (m.elements || []).some((e) => (over[e.key] && over[e.key].color ? over[e.key].color : e.color_mode) === 'state')) {
        out.push({ kind, index });
      }
    });
  }
  return out;
}

function usedBy(list) {
  if (!list.length) return 'Not used now';
  const keys = list.filter((s) => s.kind === 'key').length;
  const zones = list.length - keys;
  return `Used by ${[keys ? `${keys} key${keys > 1 ? 's' : ''}` : '', zones ? `${zones} zone${zones > 1 ? 's' : ''}` : ''].filter(Boolean).join(' and ')}`;
}

// Hovering or focusing a row outlines on the deck the slots it changes.
function outlines(node, list) {
  node.addEventListener('mouseenter', () => highlightSlots(list));
  node.addEventListener('mouseleave', () => highlightSlots([]));
  node.addEventListener('focusin', () => highlightSlots(list));
  node.addEventListener('focusout', () => highlightSlots([]));
  return node;
}

function panelHead(crumb, title, desc) {
  return el('div', { class: 'panel-head' }, el('nav', { class: 'crumbs', 'aria-label': 'Breadcrumb' }, crumb),
    el('h2', { id: 'panel-h', class: 'title' }, title), desc ? el('p', { class: 'desc' }, desc) : null);
}

// ---------- Look ----------

function lookPanel() {
  return [panelHead('Look', 'Look of the whole deck', 'Defaults every key and strip zone uses unless it sets its own.'),
    el('div', { class: 'panel-body', id: 'panel-body' }, el('div', { id: 'slot-errors', class: 'notice bad', hidden: true }),
      fontBlock(), stylesBlock(), statusBlock())];
}

function fontBlock() {
  const a = look();
  const own = [];
  for (const kind of ['key', 'zone']) {
    listOf(state.doc, kind).forEach((s, i) => { if (s && s.module && s.font && i < slotCount(kind)) own.push([kind, i]); });
  }
  const font = fontInput(a.font || DEFAULT_FONT, DEFAULT_FONT, (name) => editLook((x) => { x.font = name || DEFAULT_FONT; }, { delay: 0 }),
    'look-font', 'appearance.font');
  const italic = el('input', { type: 'checkbox', class: 'switch', checked: !!a.italic, 'data-f': 'look-italic', 'data-path': 'appearance.italic',
    onchange: (e) => editLook((x) => { x.italic = e.target.checked; }, { delay: 0 }) });
  return el('section', { class: 'block', 'data-look': 'font' }, el('h3', {}, 'Default font'),
    row('look-font', 'Font', font.input, { show: font.box }),
    row('look-italic', 'Italic', italic, { inline: true }),
    el('p', { class: 'note' }, own.length ? ['Set on their own: ', ...own.flatMap(([kind, i], j) => [j ? ', ' : '',
      el('a', { href: formatHash({ tab: 'slots', kind, index: i, sub: 'look' }) }, slotName(kind, i))])]
      : 'Used by every key and strip zone.'));
}

function stylesBlock() {
  const current = (role) => (look().roles && look().roles[role]) || {};
  const put = (role, prop, value, opts) => editLook((a) => {
    const roles = roleWith(a.roles, role, prop, value);
    if (roles) a.roles = roles;
    else delete a.roles;
  }, Object.assign({ merge: `roles.${role}.${prop}` }, opts));
  const lines = ROLES.map(([role, label]) => {
    const set = current(role);
    const any = () => Object.keys(current(role)).length > 0;
    const reset = resetButton(`${label} style`, () => {
      editLook((a) => {
        const roles = Object.assign({}, a.roles);
        delete roles[role];
        if (Object.keys(roles).length) a.roles = roles;
        else delete a.roles;
      }, { structural: true });
    }, any());
    const after = () => showReset(reset, any());
    const font = fontInput(set.font || '', 'Default', (name) => { put(role, 'font', name || null, { delay: 0 }); after(); },
      `role-${role}-font`, `appearance.roles.${role}.font`);
    font.input.setAttribute('aria-label', `${label}: font`);
    const size = numberInput({ type: 'int', min: ROLE_SIZE_MIN, max: ROLE_SIZE_MAX, placeholder: 'Default', optional: true }, set.size,
      (n, opts) => { put(role, 'size', n, opts); after(); });
    size.setAttribute('aria-label', `${label}: size in px`);
    size.setAttribute('data-f', `role-${role}-size`);
    size.setAttribute('data-path', `appearance.roles.${role}.size`);
    size.id = `role-${role}-size`;
    const weight = makeSelect([['', 'Default'], ...WEIGHTS.map(([w, name]) => [String(w), name])],
      set.weight != null ? String(set.weight) : '', `${label}: weight`, (x) => { put(role, 'weight', x ? Number(x) : null, { delay: 0 }); after(); });
    weight.setAttribute('data-f', `role-${role}-weight`);
    weight.setAttribute('data-path', `appearance.roles.${role}.weight`);
    const users = roleUsers(role);
    return outlines(el('div', { class: 'role-row', 'data-look': `styles/${role}` },
      el('div', { class: 'line' }, el('strong', {}, label), el('small', { class: 'note' }, ROLE_EXAMPLES[role]),
        el('span', { class: 'spacer' }), el('small', { class: 'note' }, usedBy(users)), reset),
      el('div', { class: 'role-controls' }, font.box, size, weight),
      el('p', { class: 'field-error', id: `role-${role}-size-err` })), users);
  });
  return el('section', { class: 'block', 'data-look': 'styles' }, el('h3', {}, 'Text styles'),
    el('p', { class: 'note' }, 'Applies to all keys (not the strip). Settings made on one key still take priority.'), lines);
}

function statusBlock() {
  const current = (key) => {
    const v = look().states && look().states[key];
    return typeof v === 'string' ? v.toLowerCase() : STATE_DEFAULTS[key];
  };
  // Sets one color, or removes it when it is the default; drops an emptied object.
  const put = (key, color, opts) => editLook((a) => {
    const states = Object.assign({}, a.states);
    if (color == null || color.toLowerCase() === STATE_DEFAULTS[key]) delete states[key];
    else states[key] = color.toLowerCase();
    if (Object.keys(states).length) a.states = states;
    else delete a.states;
  }, opts);
  const users = statusUsers();
  const lines = STATE_LABELS.map(([key, label]) => {
    const input = el('input', { type: 'color', value: current(key), 'aria-label': `${label} color`, 'data-f': `state-${key}`,
      'data-path': `appearance.states.${key}`, onchange: (e) => { put(key, e.target.value, { delay: 0 }); showReset(reset, current(key) !== STATE_DEFAULTS[key]); } });
    const reset = resetButton(`${label} color`, () => {
      put(key, null, { delay: 0 });
      input.value = STATE_DEFAULTS[key];
      showReset(reset, false);
    }, current(key) !== STATE_DEFAULTS[key]);
    return el('div', { class: 'kv' }, el('span', { class: 'label' }, label), input, el('span'), reset);
  });
  return outlines(el('section', { class: 'block', 'data-look': 'status' }, el('h3', {}, 'Status colors'),
    el('p', { class: 'note' }, `Used by parts colored "By status". Warning and critical follow each module's warning levels. ${usedBy(users)}.`),
    el('div', { class: 'rows' }, lines)), users);
}

// ---------- Hardware ----------

function slider(key, label) {
  const dev = () => state.doc.device;
  const out = el('output', { for: `hw-${key}` }, `${dev()[key]} %`);
  const input = el('input', { type: 'range', min: 0, max: 100, step: 1, value: String(dev()[key]), 'data-f': `hw-${key}`,
    'data-path': `device.${key}`,
    oninput: (e) => {
      out.textContent = `${e.target.value} %`;
      edit((d) => { d.device[key] = Number(e.target.value); }, { merge: `device.${key}`, delay: SLIDER_GAP, throttle: true });
    } });
  return row(`hw-${key}`, label, input, { show: el('div', { class: 'slider' }, input, out) });
}

function hardwarePanel() {
  const dv = state.device;
  const rings = dv.knob_rings ? state.doc.device.ring_colors.slice(0, dv.knobs).map((color, i) => {
    const input = el('input', { type: 'color', value: color, 'data-f': `hw-ring-${i}`, 'data-path': `device.ring_colors[${i}]`,
      oninput: (e) => previewRing(i, e.target.value),
      onchange: (e) => edit((d) => { d.device.ring_colors[i] = e.target.value; }, { delay: 0 }) });
    return row(`hw-ring-${i}`, `${slotName('knob', i)} (${moduleName(state.doc.knobs[i])})`, input, { inline: true });
  }) : [];
  const sizes = `${dv.rows} × ${dv.cols} keys of ${dv.key_size.width} × ${dv.key_size.height} px`;
  return [panelHead('Hardware', dv.name, 'Brightness and lights of the device.'),
    el('div', { class: 'panel-body', id: 'panel-body' }, el('div', { id: 'slot-errors', class: 'notice bad', hidden: true }),
      el('section', { class: 'block' }, el('h3', {}, 'Brightness'), slider('brightness', 'Screen brightness'),
        dv.knob_rings ? slider('ring_brightness', 'Ring brightness') : null),
      rings.length ? el('section', { class: 'block' }, el('h3', {}, 'Default ring colors'), rings,
        el('p', { class: 'note' }, "For knobs with no module. A knob's module sets its own ring colors, in its Ring tab.")) : null,
      el('section', { class: 'block' }, el('h3', {}, 'Device'), el('p', { class: 'note' }, [sizes,
        dv.strip ? `a ${dv.strip.width} × ${dv.strip.height} px touch strip` : 'no touch strip',
        `${dv.knobs} knob${dv.knobs === 1 ? '' : 's'}${dv.knob_rings ? ' with ring lights' : ''}`].join(', ') + '.')),
      dv.vendor_app ? el('section', { class: 'block' }, el('h3', {}, 'Manufacturer software'),
        el('p', {}, `While ${dv.vendor_app} runs, it takes the deck: lite-deck lets it go and takes it back when ${dv.vendor_app} closes.`)) : null)];
}
