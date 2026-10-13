'use strict';
// The deck: built once from GET /api/device, then patched slot by slot. Previews, selection, the
// "changed" markers and the strip zones. A zone's marker compares by index: after a split, merge or
// move, the zones that shifted show as changed.

const previews = new Map();  // 'key/3', 'strip/0' -> { img, busy, again, etag, url }
const baseline = new Map();  // 'key-3' -> the slot's JSON when the page opened
let previewsPaused = false;
let dragFrom = null;         // { kind, index } while a slot is dragged
let clipboard = null;        // { kind, slot }: Ctrl+C on a key, knob or zone

// ---------- previews ----------

// One <img> per target, created once. A refresh asked while one is on its way becomes a single
// extra request when it ends. The ETag of the last frame makes an unchanged one cost a 304.
function preview(target) {
  let p = previews.get(target);
  if (!p) {
    p = { img: el('img', { alt: '', draggable: 'false', decoding: 'async' }), busy: false, again: false, etag: '', url: '' };
    previews.set(target, p);
    refresh(target);
  }
  return p.img;
}

async function refresh(target) {
  const p = previews.get(target);
  if (!p) return;
  if (p.busy || previewsPaused) { p.again = true; return; }
  p.busy = true;
  try {
    const res = await fetch(`/api/preview/${target}`, { cache: 'no-store', headers: p.etag ? { 'If-None-Match': p.etag } : {} });
    if (res.status === 200) {
      const url = URL.createObjectURL(await res.blob());
      p.img.src = url;
      if (p.url) URL.revokeObjectURL(p.url);
      p.url = url;
      p.etag = res.headers.get('ETag') || '';
    }
  } catch { /* the event stream reports a daemon gone */ }
  p.busy = false;
  if (p.again && !previewsPaused) {
    p.again = false;
    refresh(target);
  }
}

function refreshAll() { for (const target of previews.keys()) refresh(target); }

// No preview while the deck is off screen: the daemon encodes nothing for pixels nobody sees.
// One callback can bring several entries for the deck (out and back in while the page was busy): the last one is now.
function watchDeck() {
  new IntersectionObserver((entries) => {
    previewsPaused = !entries[entries.length - 1].isIntersecting;
    if (!previewsPaused) for (const [target, p] of previews) if (p.again) { p.again = false; refresh(target); }
  }).observe($('deck'));
}

// ---------- building and patching ----------

// "3 CPU load" under a slot: its number, as everywhere on the page, and its module.
function caption(i) {
  return el('span', { class: 'cap', 'aria-hidden': 'true' }, el('span', { class: 'num' }, String(i + 1)), el('span', { class: 'name' }));
}

function slotButton(kind, i, face) {
  return el('button', { type: 'button', class: `slot ${kind}`, 'data-f': `${kind}-${i}`, 'data-kind': kind, 'data-i': i,
    draggable: 'true' }, face, caption(i), el('span', { class: 'mark' }));
}

function buildDeck() {
  const dv = state.device;
  $('model').textContent = dv.name;
  $('device-name').textContent = dv.name;
  const keys = $('keys');
  keys.style.setProperty('--cols', String(dv.cols));
  keys.style.setProperty('--key-ratio', `${dv.key_size.width} / ${dv.key_size.height}`);
  keys.replaceChildren(...Array.from({ length: keyCount() },
    (_, i) => slotButton('key', i, el('span', { class: 'face' }, preview(`key/${i}`)))));
  const strip = $('strip');
  strip.hidden = !dv.strip;  // no strip: the section stays in config.json, unseen and not built
  $('strip-caps').hidden = !dv.strip;
  if (dv.strip) strip.style.setProperty('--strip-ratio', `${dv.strip.width} / ${dv.strip.height}`);
  const knobs = $('knobs');
  knobs.hidden = dv.knobs === 0;
  knobs.style.setProperty('--knob-cols', String(Math.max(dv.knobs, 1)));
  knobs.replaceChildren(...Array.from({ length: dv.knobs }, (_, i) => slotButton('knob', i, el('span', { class: 'dial' }))));
  const deck = $('deck');
  deck.addEventListener('click', onDeckClick);
  deck.addEventListener('keydown', onDeckKey);
  deck.addEventListener('dragstart', onDragStart);
  deck.addEventListener('dragover', onDragOver);
  deck.addEventListener('dragleave', onDragLeave);
  deck.addEventListener('drop', onDrop);
  deck.addEventListener('dragend', onDragEnd);
  for (const kind of ['key', 'knob', 'zone']) {
    listOf(state.doc, kind).forEach((slot, i) => baseline.set(`${kind}-${i}`, JSON.stringify(slot)));
  }
  watchDeck();
  patchDeck();
}

const pct = (px) => `${(px / stripWidth()) * 100}%`;

function place(node, x, width) {
  node.style.left = pct(x);
  if (width != null) node.style.width = pct(width);
  return node;
}

// The zones follow the layout: rebuilt when a width or the count changes, patched otherwise.
function buildZones() {
  const zones = state.doc.strip;
  const strip = $('strip');
  const sig = zones.map((z) => z.width).join(',');
  if (strip.dataset.sig !== sig) {
    strip.dataset.sig = sig;
    const parts = [preview('strip/0')];
    const caps = [];
    let x = 0;
    zones.forEach((z, i) => {
      if (i > 0) parts.push(borderHandle(i, x));
      parts.push(place(el('button', { type: 'button', class: 'zone', 'data-f': `zone-${i}`, 'data-kind': 'zone', 'data-i': i,
        draggable: 'true' }, el('span', { class: 'mark' })), x, z.width));
      caps.push(place(caption(i), x, z.width));
      x += z.width;
    });
    strip.replaceChildren(...parts);
    $('strip-caps').replaceChildren(...caps);
  }
  zones.forEach((_, i) => patchSlot('zone', i));
}

function patchDeck() {
  if (!state.doc) return;
  if (state.device.strip) buildZones();
  for (let i = 0; i < keyCount(); i += 1) patchSlot('key', i);
  for (let i = 0; i < state.device.knobs; i += 1) patchSlot('knob', i);
  patchSelection();
}

const slotNode = (kind, i) => $('deck').querySelector(`[data-kind="${kind}"][data-i="${i}"]`);
const isChanged = (kind, i) => baseline.get(`${kind}-${i}`) !== JSON.stringify(listOf(state.doc, kind)[i]);

// Refused by the last save (a red dot); an error about the whole strip marks every zone.
function hasError(kind, i) {
  return state.errors.some((e) => {
    const t = errorTarget(e);
    return t.kind === kind && (t.index === i || t.index === -1);
  });
}

function patchSlot(kind, i) {
  const b = slotNode(kind, i);
  if (!b) return;
  const slot = listOf(state.doc, kind)[i];
  const info = moduleOf(slot);
  const changed = isChanged(kind, i);
  const error = hasError(kind, i);
  const cap = kind === 'zone' ? $('strip-caps').children[i] : b.querySelector('.cap');
  cap.lastChild.textContent = kind === 'zone' ? `${moduleName(slot)} · ${slot.width} px` : moduleName(slot);
  b.setAttribute('aria-label', `${slotName(kind, i)}, ${moduleName(slot)}${error ? ', has an error' : changed ? ', changed' : ''}`);
  b.classList.toggle('empty', !info);
  b.classList.toggle('changed', changed);
  b.classList.toggle('error', error);
  for (const n of [b, cap]) {
    if (info && info.hue) n.style.setProperty('--slot-hue', info.hue);
    else n.style.removeProperty('--slot-hue');
  }
  if (kind === 'knob' && state.device.knob_rings) b.style.setProperty('--ring', ringColor(slot, i));
}

// A color #rrggbb as the eye sees the LEDs: at full brightness (docs/images/deck.png does the same).
function ledColor(hex) {
  if (!/^#[0-9a-f]{6}$/i.test(hex)) return hex;
  const c = [1, 3, 5].map((k) => parseInt(hex.slice(k, k + 2), 16));
  const top = Math.max(...c, 1);
  return `#${c.map((x) => Math.floor((x * 255) / top).toString(16).padStart(2, '0')).join('')}`;
}
// What a knob's ring shows at rest, as the daemon sets it: its module's first ring state; device
// ring_colors only for a knob with no module (or one without ring states).
function ringColor(slot, i) {
  const info = moduleOf(slot);
  const r = info && (info.ring_states || [])[0];
  return ledColor(r ? effective(info, slot)[r.color] : state.doc.device.ring_colors[i] || '#202020');
}
// An idle color being picked shows at once, on a knob that uses it.
function previewRing(i, color) {
  const info = moduleOf(state.doc.knobs[i]);
  if (!(info && (info.ring_states || []).length)) slotNode('knob', i).style.setProperty('--ring', ledColor(color));
}

// The selected slot is aria-current; it is the one Tab stop of its group (roving tabindex), or the
// first slot of a group without it. In the strip, the borders on either side of that zone are Tab
// stops too (the others are reached by moving to their zone).
function patchSelection() {
  const v = state.view;
  for (const group of ['keys', 'strip', 'knobs']) {
    const nodes = [...$(group).querySelectorAll('[data-kind]')];
    const sel = nodes.find((b) => v.tab === 'slots' && b.dataset.kind === v.kind && Number(b.dataset.i) === v.index);
    const stop = sel || nodes[0];
    nodes.forEach((b) => {
      if (b === sel) b.setAttribute('aria-current', 'true');
      else b.removeAttribute('aria-current');
      b.tabIndex = b === stop ? 0 : -1;
    });
    const at = stop ? Number(stop.dataset.i) : -1;
    for (const h of $(group).querySelectorAll('.handle')) h.tabIndex = [at, at + 1].includes(Number(h.dataset.i)) ? 0 : -1;
  }
}

// Keys or zones a Look row is about, outlined on the deck while the row is hovered or focused.
function highlightSlots(list) {
  for (const n of $('deck').querySelectorAll('.hl')) n.classList.remove('hl');
  for (const s of list || []) {
    const b = slotNode(s.kind, s.index);
    if (b) b.classList.add('hl');
  }
}

// ---------- pointer and keyboard ----------

function onDeckClick(e) {
  const b = e.target.closest('[data-kind]');
  if (!b) return;
  go({ tab: 'slots', kind: b.dataset.kind, index: Number(b.dataset.i) });
  if (e.detail && matchMedia('(max-width: 959px)').matches) $('panel').scrollIntoView({ block: 'start' });
}

// Arrows move across a group of the deck, Home and End go to its ends; the selection follows,
// without a new history entry. Enter and Space click (the panel opens). Keys with Alt, Ctrl or
// Meta are left to the browser (Alt+Left: back). Escape in the panel comes back here (onPanelKey).
function onDeckKey(e) {
  const b = e.target.closest('[data-kind]');
  if (!b || e.altKey || e.ctrlKey || e.metaKey || e.shiftKey) return;
  const kind = b.dataset.kind;
  const i = Number(b.dataset.i);
  const count = slotCount(kind);
  const cols = kind === 'key' ? state.device.cols : count;
  const step = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: cols, ArrowUp: -cols, Home: -i, End: count - 1 - i }[e.key];
  if (step === undefined) return;
  e.preventDefault();
  const j = i + step;
  if (j === i || j < 0 || j >= count) return;  // an edge: nowhere to go
  go({ tab: 'slots', kind, index: j }, true);
  const n = slotNode(kind, j);
  if (n) n.focus();
}

// Escape in the panel puts the focus back on its slot; not in an open list or a search box that
// Escape empties first, nor in the Swap with… list (it closes itself).
function onPanelKey(e) {
  const t = e.target;
  if (e.key !== 'Escape' || e.defaultPrevented || t.tagName === 'SELECT' || (t.type === 'search' && t.value)) return;
  const n = state.view.tab === 'slots' && state.view.kind ? slotNode(state.view.kind, state.view.index) : null;
  if (n) n.focus();
}

// ---------- moving and copying slots ----------

let lastMove = null;  // the last move or copy: the documents and error marks before and after it

// The error marks for a document an undo or a redo brings back (applyStep).
function stepErrors(text) {
  const m = lastMove;
  if (m && text === m.doc && state.errors === m.after) return m.before;
  if (m && text === m.done && state.errors === m.before) return m.after;
  return state.errors;
}

const clone = (slot) => (slot == null ? null : JSON.parse(JSON.stringify(slot)));

function swapSlots(d, kind, a, b) {
  const l = listOf(d, kind);
  [l[a], l[b]] = [l[b] ?? null, l[a] ?? null];
}

// A copy of `slot` on slot `to`; a zone keeps its width.
function putSlot(d, kind, to, slot) {
  const l = listOf(d, kind);
  l[to] = kind === 'zone' ? Object.assign(clone(slot) || { module: null }, { width: l[to].width }) : clone(slot);
}

function copySlotTo(d, kind, from, to) { putSlot(d, kind, to, listOf(d, kind)[from]); }

function moveZoneTo(zones, from, to) {
  const [z] = zones.splice(from, 1);
  zones.splice(to, 0, z);
}

// Where the slot at index i goes (-1: replaced, its marks go): a swap, a copy's target, a zone moved.
const swapMap = (a, b) => (i) => (i === a ? b : i === b ? a : i);
const copyMap = (to) => (i) => (i === to ? -1 : i);
const moveMap = (from, to) => (i) => (i === from ? to : from < to && i > from && i <= to ? i - 1
  : from > to && i >= to && i < from ? i + 1 : i);

// The marks of a refused save follow the slots they are about, so that nothing refused goes unseen
// during a conflict or offline; the strip total ('strip') stays (tests/web/deck.test.js).
function remapErrors(errors, kind, at) {
  const list = slotPath({ kind, index: 0 }).slice(0, -3);  // 'keys'
  return errors.flatMap((e) => {
    const m = /^(\w+)\[(\d+)\]/.exec(String(e.path || ''));
    if (!m || m[1] !== list) return [e];
    const i = at(Number(m[2]));
    return i < 0 ? [] : [Object.assign({}, e, { path: e.path.replace(/\[\d+\]/, `[${i}]`) })];
  });
}

// One undo step; the toast's Undo puts back the slots touched (`points`) only. Undo (toast or
// Ctrl+Z) and redo put the marks back too, while no save answered meanwhile (stepErrors).
function changeSlots(kind, to, points, at, change, text) {
  const before = state.errors;
  const after = remapErrors(before, kind, at);
  const doc = JSON.stringify(state.doc);
  state.errors = after;
  edit(change, { structural: true });
  lastMove = { doc, before, done: JSON.stringify(state.doc), after };
  toast(text, [['Undo', () => {
    if (state.errors === after && points[0].loads === state.loads) state.errors = before;
    restoreSlot(points);
  }]]);
  go({ tab: 'slots', kind, index: to });
}

// Drag and drop, Swap with… / Move to…, Copy to…: keys and knobs swap, zones move.
function placeSlot(kind, from, to, copy) {
  if (from === to) return;
  const zone = kind === 'zone';
  const [a, b] = [slotName(kind, from), slotName(kind, to)];
  const points = [undoPoint(state.doc, kind, to)];
  if (!copy) points.push(undoPoint(state.doc, kind, from));
  changeSlots(kind, to, points, copy ? copyMap(to) : zone ? moveMap(from, to) : swapMap(from, to), (d) => {
    if (copy) copySlotTo(d, kind, from, to);
    else if (zone) moveZoneTo(d.strip, from, to);
    else swapSlots(d, kind, from, to);
  }, copy ? `Copied ${a} to ${b}.` : zone ? `Moved ${a} to place ${to + 1}.` : `Swapped ${a} and ${b}.`);
}

function onDragStart(e) {
  const b = e.target.closest('[data-kind]');
  if (!b || b.classList.contains('handle')) return;
  dragFrom = { kind: b.dataset.kind, index: Number(b.dataset.i) };
  e.dataTransfer.effectAllowed = 'copyMove';
  e.dataTransfer.setData('application/x-lite-deck', '');  // not text: a text field refuses the drop
}

function dropTarget(e) {
  const b = e.target.closest('[data-kind]');
  return b && dragFrom && b.dataset.kind === dragFrom.kind && Number(b.dataset.i) !== dragFrom.index ? b : null;
}

function onDragOver(e) {
  const b = dropTarget(e);
  if (!b) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = e.ctrlKey ? 'copy' : 'move';
  b.classList.add('drop');
}

function onDragLeave(e) {
  const b = e.target.closest('[data-kind]');
  if (b && !b.contains(e.relatedTarget)) b.classList.remove('drop');
}

function onDrop(e) {
  const b = dropTarget(e);
  if (!b) return;
  e.preventDefault();
  const from = dragFrom;
  onDragEnd();
  placeSlot(from.kind, from.index, Number(b.dataset.i), e.ctrlKey);
}

function onDragEnd() {
  dragFrom = null;
  for (const n of document.querySelectorAll('.drop')) n.classList.remove('drop');
}

// Ctrl+C / Ctrl+V on the selected slot: module, settings and look.
function copySelected() {
  const v = state.view;
  if (v.tab !== 'slots' || !v.kind) return;
  const slot = slotOf(v);
  clipboard = { kind: v.kind, slot: clone(slot) };
  toast(`Copied ${slotName(v.kind, v.index)} (${moduleName(slot)}).`);
  renderPanel();  // the Paste button
}

function pasteSelected() {
  const v = state.view;
  if (!clipboard || v.tab !== 'slots' || !v.kind) return;
  if (v.kind !== clipboard.kind) { toast(`This copy goes on a ${KIND_LABEL[clipboard.kind].toLowerCase()} only.`); return; }
  changeSlots(v.kind, v.index, [undoPoint(state.doc, v.kind, v.index)], copyMap(v.index),
    (d) => putSlot(d, v.kind, v.index, clipboard.slot), `Pasted on ${slotName(v.kind, v.index)}.`);
}

// ---------- strip zones ----------

// Brings the total back to the strip width after zone `fixed` changed: the other zones grow or
// shrink, the nearest first, never below MIN_ZONE (the Width field).
function rebalance(zones, fixed) {
  const n = zones.length;
  if (n === 1) { zones[0].width = stripWidth(); return; }
  const max = stripWidth() - MIN_ZONE * (n - 1);
  zones[fixed].width = Math.min(max, Math.max(MIN_ZONE, Math.round(zones[fixed].width) || MIN_ZONE));
  let diff = stripWidth() - zones.reduce((sum, z) => sum + z.width, 0);
  for (let d = 1; d < n && diff !== 0; d += 1) {
    for (const j of [fixed + d, fixed - d]) {
      if (j < 0 || j >= n || diff === 0) continue;
      const change = diff > 0 ? diff : -Math.min(-diff, zones[j].width - MIN_ZONE);
      zones[j].width += change;
      diff -= change;
    }
  }
}

function resizeZone(i, width, opts = { structural: true }) {
  edit((d) => {
    d.strip[i].width = width;
    rebalance(d.strip, i);
  }, opts);
}

// The border before zone `i` moves by `delta` px, rounded to `step` px from where it is (a typed
// width of 333 moves to 343): only zones i-1 and i change, both kept >= `min`. Pure: returns the
// new widths (tests/web/deck.test.js).
function dragBorder(widths, i, delta, min, step) {
  const out = widths.slice();
  const pair = out[i - 1] + out[i];
  const left = Math.max(min, Math.min(pair - min, out[i - 1] + Math.round(delta / step) * step));
  out[i - 1] = left;
  out[i] = pair - left;
  return out;
}

let presses = 0;    // arrow presses on a border: each press (held or not) is one undo step
let held = false;   // an arrow key is down on a border (rebuilt at each step: not a property of it)

// What a border says to assistive technology: the width of the zone on its left, within its
// bounds, and both widths in words.
function borderValue(h, i, widths) {
  const [a, b] = [widths[i - 1], widths[i]];
  h.setAttribute('aria-valuemin', MIN_ZONE);
  h.setAttribute('aria-valuemax', a + b - MIN_ZONE);
  h.setAttribute('aria-valuenow', a);
  h.setAttribute('aria-valuetext', `Zone ${i}: ${a} px, zone ${i + 1}: ${b} px`);
}

// A border between two zones (a focusable separator): dragged with the mouse, or moved by 10 px
// with the arrow keys, to an end with Home and End. A key held down saves once, when it is
// released (or after a pause). Alt, Ctrl or Meta with an arrow are left to the browser.
function borderHandle(i, x) {
  const h = place(el('div', { class: 'handle', role: 'separator', tabindex: -1, 'aria-orientation': 'vertical',
    'aria-label': `Border between strip zones ${i} and ${i + 1}`, 'data-f': `handle-${i}`, 'data-i': i,
    title: 'Drag to resize; arrow keys move it by 10 px' }), x);
  borderValue(h, i, state.doc.strip.map((z) => z.width));
  h.addEventListener('pointerdown', (e) => dragHandle(e, i));
  h.addEventListener('keydown', (e) => {
    const delta = { ArrowLeft: -ZONE_STEP, ArrowRight: ZONE_STEP, Home: -stripWidth(), End: stripWidth() }[e.key];
    if (!delta || e.altKey || e.ctrlKey || e.metaKey) return;
    e.preventDefault();
    e.stopPropagation();
    if (!e.repeat) presses += 1;
    held = true;
    setWidths(dragBorder(state.doc.strip.map((z) => z.width), i, delta, MIN_ZONE, ZONE_STEP),
      { merge: `handle-${i}-${presses}`, delay: TEXT_DELAY });
    showZoneWidth();
  });
  h.addEventListener('keyup', (e) => {
    if (!held || !['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
    held = false;
    edit(() => {}, { structural: true });  // the save waiting goes now; the panel shows the new widths
  });
  return h;
}

// The Width field of the zone shown, while a border moves under the keyboard (no redraw meanwhile).
function showZoneWidth() {
  const f = $('panel').querySelector('[data-f=zone-width]');
  const v = state.view;
  if (f && !f.typed && v.kind === 'zone' && state.doc.strip[v.index]) f.value = String(state.doc.strip[v.index].width);
}

function setWidths(widths, opts = { structural: true }) {
  if (widths.every((w, j) => w === state.doc.strip[j].width)) return;
  edit((d) => d.strip.forEach((z, j) => { z.width = widths[j]; }), opts);
}

// While the border moves, only the page moves (showWidths); one save and one undo step when the
// mouse is released. A capture lost before (another window took the mouse, the border was
// redrawn) cancels the drag: the zones go back and nothing is saved.
function dragHandle(e, i) {
  if (e.button !== 0) return;
  e.preventDefault();
  const h = e.currentTarget;
  h.setPointerCapture(e.pointerId);
  h.classList.add('active');
  const start = state.doc.strip.map((z) => z.width);
  const scale = stripWidth() / $('strip').clientWidth;
  let widths = start;
  const move = (ev) => {
    widths = dragBorder(start, i, (ev.clientX - e.clientX) * scale, MIN_ZONE, ZONE_STEP);
    showWidths(widths);
  };
  const end = (ev) => {
    h.removeEventListener('pointermove', move);
    h.removeEventListener('pointerup', end);
    h.removeEventListener('pointercancel', end);
    h.removeEventListener('lostpointercapture', end);
    h.classList.remove('active');
    showWidths(start);
    // Not if zones came or went, or the widths changed (an undo) meanwhile.
    if (ev.type === 'pointerup' && state.doc.strip.every((z, j) => z.width === start[j]) && state.doc.strip.length === start.length) {
      setWidths(widths);
    }
  };
  h.addEventListener('pointermove', move);
  h.addEventListener('pointerup', end);
  h.addEventListener('pointercancel', end);
  h.addEventListener('lostpointercapture', end);
}

// The zones, their captions and borders placed at `widths`, without saving anything.
function showWidths(widths) {
  const zones = $('strip').querySelectorAll('.zone');
  const handles = $('strip').querySelectorAll('.handle');
  const caps = $('strip-caps').children;
  if (zones.length !== widths.length) return;  // zones came or went meanwhile
  let x = 0;
  widths.forEach((w, j) => {
    place(zones[j], x, w);
    place(caps[j], x, w).lastChild.textContent = `${moduleName(state.doc.strip[j])} · ${w} px`;
    if (j > 0) borderValue(place(handles[j - 1], x), j, widths);
    x += w;
  });
}

// Splits zone `i` in two halves; the new zone is empty and selected.
function splitZone(i) {
  const zones = state.doc.strip;
  if (zones.length >= MAX_ZONES || zones[i].width < 2 * MIN_ZONE) return;
  edit((d) => {
    const half = Math.floor(d.strip[i].width / 2);
    d.strip[i].width -= half;
    d.strip.splice(i + 1, 0, { width: half, module: null });
  }, { structural: true });
  go({ tab: 'slots', kind: 'zone', index: i + 1 });
}

// Zone `i` goes; its width goes to its neighbour on `side` (-1 left, 1 right).
function mergeZone(i, side) {
  const zones = state.doc.strip;
  const into = i + side;
  if (zones.length < 2 || into < 0 || into >= zones.length) return;
  const gone = moduleName(zones[i]);
  const point = undoPoint(state.doc, 'zone', i);
  edit((d) => {
    const [z] = d.strip.splice(i, 1);
    d.strip[side < 0 ? into : i].width += z.width;
  }, { structural: true });
  const kept = side < 0 ? into : i;
  toast(`${slotName('zone', i)} (${gone}) merged into its neighbour.`, [['Undo', () => restoreSlot(point)]]);
  go({ tab: 'slots', kind: 'zone', index: kept });
}
