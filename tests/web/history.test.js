'use strict';
// node tests/web/history.test.js: undo and redo of whole documents.
const assert = require('node:assert/strict');
const { load } = require('./load');

const ctx = load('app.js');
const h = ctx.makeHistory(3);
assert.equal(h.canUndo(), false);
assert.equal(h.undo('A'), null);
// Each change keeps the document as it was before it.
h.record('A', '', 0);
h.record('B', '', 10000);
assert.equal(h.undo('C'), 'B');
assert.equal(h.undo('B'), 'A');
assert.equal(h.undo('A'), null);
assert.equal(h.redo('A'), 'B');
assert.equal(h.redo('B'), 'C');
assert.equal(h.redo('C'), null);
// A new change drops what could be redone.
h.undo('C');
h.record('B', '', 20000);
assert.equal(h.canRedo(), false);
// Changes with the same key less than 1.5 s apart make one step (typing in a field).
const t = ctx.makeHistory(50);
t.record('v1', 'keys[0].settings.city', 0);
t.record('v2', 'keys[0].settings.city', 500);
t.record('v3', 'keys[0].settings.city', 1200);
assert.equal(t.undo('v4'), 'v1');
assert.equal(t.canUndo(), false);
t.record('w1', 'k', 0);
t.record('w2', 'k', 5000);  // later: a new step
assert.equal(t.undo('w3'), 'w2');
// The same document twice in a row is one step.
const d = ctx.makeHistory(50);
d.record('X', '', 0);
d.record('X', '', 9000);
assert.equal(d.undo('Y'), 'X');
assert.equal(d.canUndo(), false);
// At most `limit` steps: the oldest go.
const l = ctx.makeHistory(3);
for (const s of ['a', 'b', 'c', 'd', 'e']) l.record(s, '', s.charCodeAt(0) * 10000);
assert.deepEqual([l.undo('f'), l.undo('e'), l.undo('d'), l.undo('c')], ['e', 'd', 'c', null]);
// A document reloaded from lite-deck starts a new history: an undo never crosses a reload.
const r = ctx.makeHistory(50);
r.record('p', '', 0);
r.record('q', '', 9000);
r.undo('s');
r.clear();
assert.equal(r.canUndo(), false);
assert.equal(r.canRedo(), false);
console.log('history.test.js: OK');
