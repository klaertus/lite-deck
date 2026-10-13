'use strict';
// node tests/web/deck.test.js: moving, copying and resizing slots.
const assert = require('node:assert/strict');
const { load, get, plain } = require('./load');

const ctx = load('app.js', 'deck.js');
// A dragged border moves only the two zones around it, in 10 px steps, each kept >= 60 px.
assert.equal(get(ctx, 'ZONE_STEP'), 10);
assert.deepEqual(plain(ctx.dragBorder([400, 400], 1, 33, 60, 10)), [430, 370]);
assert.deepEqual(plain(ctx.dragBorder([400, 400], 1, -1000, 60, 10)), [60, 740]);
assert.deepEqual(plain(ctx.dragBorder([200, 300, 300], 2, 1000, 60, 10)), [200, 540, 60]);
assert.deepEqual(plain(ctx.dragBorder([200, 300, 300], 1, 4, 60, 10)), [200, 300, 300]);
// The steps count from the width as it is (typed, or split in halves): 333 moves to 343, not 340.
assert.deepEqual(plain(ctx.dragBorder([333, 467], 1, 10, 60, 10)), [343, 457]);
assert.deepEqual(plain(ctx.dragBorder([333, 467], 1, 0, 60, 10)), [333, 467]);
assert.deepEqual(plain(ctx.dragBorder([333, 467], 1, -14, 60, 10)), [323, 477]);
const sum = (a) => a.reduce((x, y) => x + y, 0);
for (const delta of [-500, -37, 0, 12, 333]) assert.equal(sum(ctx.dragBorder([250, 250, 300], 2, delta, 60, 10)), 800);
// Swap and copy keys; a copied zone keeps the target's width; zones move with their width.
const doc = () => ({ keys: [{ module: 'a', settings: { x: 1 } }, null, { module: 'c' }], knobs: [],
  strip: [{ width: 300, module: 'p' }, { width: 500, module: 'q' }] });
let d = doc();
ctx.swapSlots(d, 'key', 0, 2);
assert.deepEqual(plain(d.keys), [{ module: 'c' }, null, { module: 'a', settings: { x: 1 } }]);
d = doc();
ctx.copySlotTo(d, 'key', 0, 1);
d.keys[1].settings.x = 2;  // a deep copy
assert.equal(d.keys[0].settings.x, 1);
d = doc();
ctx.copySlotTo(d, 'key', 1, 0);  // copying an empty key empties the target
assert.equal(d.keys[0], null);
d = doc();
ctx.copySlotTo(d, 'zone', 0, 1);
assert.deepEqual(plain(d.strip), [{ width: 300, module: 'p' }, { width: 500, module: 'p' }]);
d = doc();
ctx.moveZoneTo(d.strip, 0, 1);
assert.deepEqual(plain(d.strip), [{ width: 500, module: 'q' }, { width: 300, module: 'p' }]);
// A knob list shorter than the device (an older config): the missing knob counts as empty.
d = { keys: [], knobs: [{ module: 'volume' }], strip: [] };
ctx.swapSlots(d, 'knob', 0, 2);
assert.deepEqual(plain(d.knobs), [null, null, { module: 'volume' }]);
// The marks of a refused save follow the slots: swapped, moved with a zone (the strip total
// stays), gone only from a copy's target.
const errs = [{ path: 'keys[0].settings', message: 'warn: too large' }, { path: 'keys[2]', message: 'x' },
  { path: 'strip[0].width', message: 'w' }, { path: 'strip', message: 'total' }, { path: 'device.brightness', message: 'b' }];
const paths = (l) => plain(l.map((e) => e.path));
const remap = (kind, map) => paths(ctx.remapErrors(errs, kind, map));
assert.deepEqual(remap('key', get(ctx, 'swapMap')(0, 2)), ['keys[2].settings', 'keys[0]', 'strip[0].width', 'strip', 'device.brightness']);
assert.deepEqual(remap('key', get(ctx, 'copyMap')(2)), ['keys[0].settings', 'strip[0].width', 'strip', 'device.brightness']);
assert.deepEqual(remap('knob', get(ctx, 'swapMap')(0, 2)), paths(errs));
assert.deepEqual(remap('zone', get(ctx, 'moveMap')(0, 2)), ['keys[0].settings', 'keys[2]', 'strip[2].width', 'strip', 'device.brightness']);
assert.equal(ctx.remapErrors(errs, 'key', get(ctx, 'swapMap')(0, 2))[0].message, 'warn: too large');
// A zone moved: the others shift like moveZoneTo shifts them.
for (const [from, to] of [[3, 1], [0, 2], [1, 3]]) {
  const z = [0, 1, 2, 3].map((i) => ({ i }));
  ctx.moveZoneTo(z, from, to);
  assert.deepEqual([0, 1, 2, 3].map(get(ctx, 'moveMap')(from, to)), [0, 1, 2, 3].map((i) => z.findIndex((x) => x.i === i)));
}
// The Width field: the other zones absorb the change, the nearest first.
get(ctx, 'state').device = { strip: { width: 800 } };
const zones = [{ width: 200 }, { width: 300 }, { width: 300 }];
zones[0].width = 500;
ctx.rebalance(zones, 0);
assert.deepEqual(plain(zones.map((z) => z.width)), [500, 60, 240]);
// Previews pause off screen, following the latest observation of the deck: two entries in one
// callback (scrolled out and back while the page was busy) leave them running.
let observed = null;
ctx.document = { getElementById: (id) => ({ id }) };
ctx.IntersectionObserver = class { constructor(cb) { observed = cb; } observe() {} };
ctx.watchDeck();
observed([{ isIntersecting: false }]);
assert.equal(get(ctx, 'previewsPaused'), true);
observed([{ isIntersecting: false }, { isIntersecting: true }]);
assert.equal(get(ctx, 'previewsPaused'), false);
observed([{ isIntersecting: true }, { isIntersecting: false }]);
assert.equal(get(ctx, 'previewsPaused'), true);
// A knob's ring as the eye sees the LEDs (docs/images/deck.png alike): at full brightness; the
// module's first ring state, or for a knob with no module its idle color (device.ring_colors).
const ringCtx = load('settings.js', 'app.js', 'deck.js');
assert.equal(ringCtx.ledColor('#00283e'), '#00a4ff');
assert.equal(ringCtx.ledColor('#000000'), '#000000');
assert.equal(ringCtx.ledColor('red'), 'red');
get(ringCtx, 'state').byId = new Map([['vol', { id: 'vol', ring_states: [{ color: 'color', brightness: 'b' }, { color: 'muted', brightness: 'mb' }],
  schema: { fields: [{ key: 'color', default: '#00283e' }, { key: 'muted', default: '#3a3a3a' }] } }]]);
get(ringCtx, 'state').doc = { device: { ring_colors: ['#3e0000', '#002132'] } };
assert.equal(ringCtx.ringColor({ module: 'vol' }, 0), '#00a4ff');
assert.equal(ringCtx.ringColor({ module: 'vol', settings: { color: '#1e0e3e' } }, 0), '#7b39ff');
assert.equal(ringCtx.ringColor(null, 1), '#00a8ff');
console.log('deck.test.js: OK');
