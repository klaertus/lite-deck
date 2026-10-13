'use strict';
// node tests/web/settings.test.js: option labels, conditional fields and number bounds.
const assert = require('node:assert/strict');
const { load, plain } = require('./load');

const ctx = load('app.js', 'settings.js');
// Options as plain values (older modules), as {value, label}, or with option_labels.
assert.deepEqual(plain(ctx.optionList({ options: ['none', 'open'] })), [['none', 'none'], ['open', 'open']]);
assert.deepEqual(plain(ctx.optionList({ options: [{ value: 'none', label: 'Nothing' }, { value: 'open' }] })),
  [['none', 'Nothing'], ['open', 'open']]);
assert.deepEqual(plain(ctx.optionList({ options: ['bytes', 'bits'], option_labels: ['MB/s', 'Mb/s'] })),
  [['bytes', 'MB/s'], ['bits', 'Mb/s']]);
assert.deepEqual(plain(ctx.optionList({})), []);
// show_if hides a field unless another field has one of the given values.
const values = { kind: 'program', direction: 'off' };
assert.equal(ctx.fieldVisible({ key: 'x' }, values), true);
assert.equal(ctx.fieldVisible({ show_if: { field: 'kind', equals: 'program' } }, values), true);
assert.equal(ctx.fieldVisible({ show_if: { field: 'kind', equals: 'open' } }, values), false);
assert.equal(ctx.fieldVisible({ show_if: { field: 'direction', in: ['above', 'below'] } }, values), false);
assert.equal(ctx.fieldVisible({ show_if: { field: 'direction', in: ['off'] } }, values), true);
assert.equal(ctx.fieldVisible({ show_if: 'junk' }, values), true);
// A list: any one condition; "and": both. The gpu's temperature settings: temp mode, or usage with the temperature on.
const gpuTemp = { show_if: [{ field: 'mode', equals: 'temp' },
  { field: 'mode', equals: 'usage', and: { field: 'show_temperature', equals: true } }] };
assert.equal(ctx.fieldVisible(gpuTemp, { mode: 'temp', show_temperature: false }), true);
assert.equal(ctx.fieldVisible(gpuTemp, { mode: 'usage', show_temperature: false }), false);
assert.equal(ctx.fieldVisible(gpuTemp, { mode: 'usage', show_temperature: true }), true);
assert.equal(ctx.fieldVisible(gpuTemp, { mode: 'vram', show_temperature: true }), false);
// Numbers are kept within their bounds; integers are rounded.
assert.equal(ctx.clampNumber({ type: 'int', min: 1, max: 10 }, 150), 10);
assert.equal(ctx.clampNumber({ type: 'int', min: 1, max: 10 }, -3), 1);
assert.equal(ctx.clampNumber({ type: 'int', min: 1, max: 10 }, 4.6), 5);
assert.equal(ctx.clampNumber({ type: 'number', min: 0.5, max: 10 }, 2.25), 2.25);
assert.equal(ctx.clampNumber({ type: 'number' }, -1e12), -1e12);
// A field an error points at stays shown whatever its show_if; a fold goes with its last field.
const disk = [
  { key: 'mode', type: 'enum' },
  { key: 'free_warn', group: 'thresholds', show_if: { field: 'mode', equals: 'space' } },
  { key: 'free_crit', group: 'thresholds', show_if: { field: 'mode', equals: 'space' } },
];
const io = { mode: 'io' };
assert.deepEqual([...ctx.shownKeys(disk, io, new Set())], ['mode']);
assert.deepEqual([...ctx.shownKeys(disk, { mode: 'space' }, new Set())], ['mode', 'free_warn', 'free_crit']);
const errors = [{ path: 'keys[2].settings', message: 'free_warn: value too large (max 100)' },
  { path: 'keys[5].settings', message: 'free_crit: value too large (max 100)' }, { path: 'strip', message: 'x' }];
const flagged = ctx.erroredSettings(errors, 'keys[2].settings.');
assert.deepEqual([...flagged], ['free_warn']);
assert.deepEqual([...ctx.shownKeys(disk, io, flagged)], ['mode', 'free_warn']);
const levels = disk.slice(1);
assert.equal(ctx.groupShown(levels, ctx.shownKeys(disk, io, new Set())), false);
assert.equal(ctx.groupShown(levels, ctx.shownKeys(disk, io, flagged)), true);
assert.equal(ctx.groupShown(levels, ctx.shownKeys(disk, { mode: 'space' }, new Set())), true);
console.log('settings.test.js: OK');
