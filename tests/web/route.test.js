'use strict';
// node tests/web/route.test.js: the address bar holds the view, numbered from 1.
const assert = require('node:assert/strict');
const { load, plain } = require('./load');

const ctx = load('app.js');
const parse = (h) => plain(ctx.parseHash(h));
assert.deepEqual(parse('#slots/key-4/look'), { tab: 'slots', kind: 'key', index: 3, sub: 'look' });
assert.deepEqual(parse('#slots/knob-1'), { tab: 'slots', kind: 'knob', index: 0 });
assert.deepEqual(parse('#slots/zone-2/content'), { tab: 'slots', kind: 'zone', index: 1, sub: 'content' });
assert.deepEqual(parse('#look/styles/value'), { tab: 'look', look: 'styles/value' });
assert.deepEqual(parse('#hardware'), { tab: 'hardware' });
// Nothing, or anything unknown, is the overview of the Slots tab.
for (const h of ['', '#', '#slots', '#nope', '#slots/key-0', '#slots/key-x', '#slots/key-123', '#slots/disk-1']) {
  assert.deepEqual(parse(h), { tab: 'slots' }, h);
}
assert.deepEqual(parse('#slots/key-2/bogus'), { tab: 'slots', kind: 'key', index: 1 });
const fmt = (v) => ctx.formatHash(v);
assert.equal(fmt({ tab: 'slots', kind: 'key', index: 3, sub: 'look' }), '#slots/key-4/look');
assert.equal(fmt({ tab: 'slots', kind: '' }), '#slots');
assert.equal(fmt({ tab: 'look', look: '' }), '#look');
assert.equal(fmt({ tab: 'look', look: 'status' }), '#look/status');
assert.equal(fmt({ tab: 'hardware' }), '#hardware');
for (const h of ['#slots/key-10/tap', '#slots/knob-4/ring', '#look/styles/two_line', '#hardware']) assert.equal(fmt(ctx.parseHash(h)), h);
console.log('route.test.js: OK');
