'use strict';
// node tests/web/errors.test.js: errors of PUT /api/config in plain words, at their place.
const assert = require('node:assert/strict');
const { load, get, plain } = require('./load');

const ctx = load('app.js', 'settings.js');
const ram = { id: 'ram', name: 'Memory (RAM)', schema: { fields: [{ key: 'warn', label: 'Warning at (%)' }] } };
get(ctx, 'state').byId = new Map([['ram', ram]]);
get(ctx, 'state').doc = { keys: [null, null, null, { module: 'ram', settings: {} }], knobs: [], strip: [] };
const target = (path, message) => plain(ctx.errorTarget({ path, message }));
assert.deepEqual(target('keys[3].settings', 'warn: value too large (max 100)'),
  { tab: 'slots', kind: 'key', index: 3, field: 'settings.warn', path: 'keys[3].settings.warn' });
assert.deepEqual(target('keys[3].elements.value.size', 'must be between 8 and 60'),
  { tab: 'slots', kind: 'key', index: 3, field: 'elements.value.size', path: 'keys[3].elements.value.size' });
assert.deepEqual(target('strip[1].width', 'x'), { tab: 'slots', kind: 'zone', index: 1, field: 'width', path: 'strip[1].width' });
assert.deepEqual(target('strip', 'x'), { tab: 'slots', kind: 'zone', index: -1, field: 'width', path: 'strip' });
assert.equal(target('appearance.roles.value.size', 'x').tab, 'look');
assert.equal(target('device.ring_colors[2]', 'x').tab, 'hardware');
assert.equal(target('', 'session missing').tab, '');
// Numbered from 1, named by the field's label, in plain words.
const human = (path, message) => ctx.humanError({ path, message });
assert.equal(human('keys[3].settings', 'warn: value too large (max 100)'), 'Key 4 › Warning at (%): too large (max 100)');
assert.equal(human('keys[3].settings', 'warn: integer expected'), 'Key 4 › Warning at (%): a whole number is needed');
assert.equal(human('keys[0].module', 'module launcher cannot be placed here'), 'Key 1: this module does not fit here');
assert.equal(human('strip', 'the widths must add up to 800 (currently 790)'), 'Strip: zone widths add up to 790 px; the strip needs 800 px');
assert.equal(human('', 'cannot write config.json'), 'cannot write config.json');
assert.equal(human('knobs[0].settings', 'step: value too small (min 1)'), 'Knob 1 › step: too small (min 1)');
// An error link leads to the slot (numbered from 1 in the address) and the sub-tab of its field.
const href = (path, message) => ctx.errorHref(ctx.errorTarget({ path, message }));
assert.equal(href('keys[3].settings', 'warn: x'), '#slots/key-4/content');
assert.equal(href('keys[3].elements.value.size', 'x'), '#slots/key-4/look');
assert.equal(href('strip', 'x'), '#slots/zone-1/content');
assert.equal(href('appearance.roles.value.size', 'x'), '#look');
assert.equal(href('', 'x'), '#slots');
// A tap field leads to Tap (keys), a ring state's colour or brightness to Ring (knobs).
get(ctx, 'state').byId.set('vol', { id: 'vol', ring_states: [{ color: 'color', brightness: 'brightness' }],
  schema: { fields: [{ key: 'step' }, { key: 'color' }, { key: 'brightness' }] } });
ram.schema.fields.push({ key: 'tap_action', group: 'tap' });
get(ctx, 'state').doc.knobs = [{ module: 'vol', settings: {} }];
assert.equal(href('keys[3].settings', 'tap_action: x'), '#slots/key-4/tap');
assert.equal(href('knobs[0].settings', 'brightness: x'), '#slots/knob-1/ring');
assert.equal(href('knobs[0].settings', 'color: x'), '#slots/knob-1/ring');
assert.equal(href('knobs[0].settings', 'step: x'), '#slots/knob-1/content');
assert.equal(href('keys[3].settings', 'nothing: x'), '#slots/key-4/content');
console.log('errors.test.js: OK');
