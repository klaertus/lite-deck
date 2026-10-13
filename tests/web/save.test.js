'use strict';
// node tests/web/save.test.js: what the answer to a save does to the changes it carried, and the save rate.
const assert = require('node:assert/strict');
const { load, get, plain } = require('./load');

const ctx = load('app.js');
// Only a 200 means the daemon has the changes; anything else keeps them in the page.
assert.equal(ctx.saveOutcome(200), 'saved');
assert.equal(ctx.saveOutcome(422), 'invalid');
assert.equal(ctx.saveOutcome(412), 'conflict');
assert.equal(ctx.saveOutcome(401), 'expired');
for (const status of [0, 400, 413, 500, 503]) assert.equal(ctx.saveOutcome(status), 'failed');
// A reload from the daemon never replaces changes not saved yet, nor one being saved, nor a conflict.
assert.equal(ctx.mayReload({ dirty: false, saving: false, conflict: 0 }), true);
assert.equal(ctx.mayReload({ dirty: true, saving: false, conflict: 0 }), false);
assert.equal(ctx.mayReload({ dirty: false, saving: true, conflict: 0 }), false);
assert.equal(ctx.mayReload({ dirty: false, saving: false, conflict: 7 }), false);
// The save gate: 4 saves at once, then about one per second.
const gate = ctx.makeSaveGate(4, 1000);
const t0 = 1000000;
for (let i = 0; i < 4; i += 1) {
  assert.equal(gate.wait(t0), 0);
  gate.take(t0);
}
assert.equal(gate.wait(t0), 1000);
assert.equal(gate.wait(t0 + 400), 600);
assert.equal(gate.wait(t0 + 1000), 0);
gate.take(t0 + 1000);
assert.equal(gate.wait(t0 + 1000), 1000);
assert.equal(gate.wait(t0 + 60000), 0);  // a pause refills the burst

// ---------- a pending conflict stays visible and nothing is sent until the user chooses ----------

// Just enough DOM for toast(), setSaveState() and el(): elements keep their children and listeners.
class FakeElement {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.attrs = {};
    this.listeners = {};
    this.hidden = false;
    this.className = '';
    this.textContent = '';
  }
  append(...c) { this.children.push(...c); }
  replaceChildren(...c) { this.children = c; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  setAttribute(k, v) { this.attrs[k] = v; }
  click() { (this.listeners.click || []).forEach((fn) => fn()); }
}
const byId = {};
ctx.document = {
  getElementById: (id) => (byId[id] ||= new FakeElement('div')),
  createElement: (tag) => new FakeElement(tag),
};
let puts = 0;
ctx.fetch = async (path, opts) => {
  if (opts.method === 'PUT') puts += 1;
  throw new Error('offline');  // a GET finds lite-deck unreachable
};
const buttons = () => byId.toast.children.filter((c) => c.tagName === 'BUTTON').map((b) => b.children[0]);
const state = get(ctx, 'state');

state.conflict = 9;
state.dirty = true;
ctx.showConflict();
assert.deepEqual(plain(buttons()), ['Keep my changes', 'Load the other version']);
// Another toast (an Undo, an error) does not replace the choice.
ctx.toast('Key 1: CPU replaced by Clock.', [['Undo', () => {}]]);
ctx.toast('lite-deck is not responding.');
assert.deepEqual(plain(buttons()), ['Keep my changes', 'Load the other version']);
assert.ok(byId.toast.children.length);
// A change made meanwhile says the truth, and sends nothing.
ctx.scheduleSave(false);
assert.equal(byId.save.children[0].children[0], 'Not saved: choose a version');  // shown, said by the toast
assert.equal(state.saveTimer, 0);
(async () => {
  await ctx.save();
  assert.equal(puts, 0);
  // "Load the other version" that cannot reach lite-deck keeps the conflict and the choice.
  byId.toast.replaceChildren();  // the toast closed (it empties: a live region is never hidden)
  await ctx.loadTheirs();
  assert.equal(state.conflict, 9);
  assert.equal(state.dirty, true);
  assert.ok(byId.toast.children.length);
  assert.deepEqual(plain(buttons()), ['Keep my changes', 'Load the other version']);
  state.conflict = 0;

  // ---------- Undo of a module change on a zone: the strip as it was, whatever happened since ----------

  const doc = {
    keys: [{ module: 'cpu', settings: { a: 1 } }, null],
    knobs: [],
    strip: [{ width: 200, module: 'clock', settings: { s: 1 } }, { width: 300, module: 'media' },
      { width: 300, module: 'gpu' }],
  };
  const before = plain(doc.strip);
  const zonePoint = ctx.undoPoint(doc, 'zone', 0);
  doc.strip[0].module = 'cpu';  // the change
  doc.strip[0].settings = {};
  [doc.strip[0], doc.strip[1]] = [doc.strip[1], doc.strip[0]];  // then a move
  ctx.applyUndo(doc, zonePoint);
  assert.deepEqual(plain(doc.strip), before);  // media untouched, clock back first
  const lastPoint = ctx.undoPoint(doc, 'zone', 2);
  doc.strip[2].module = 'net_speed';
  doc.strip.splice(2, 1);  // then the zone is deleted
  doc.strip[1].width += 300;
  ctx.applyUndo(doc, lastPoint);  // no throw
  assert.deepEqual(plain(doc.strip), before);
  // A key keeps its index: only that slot comes back.
  const keyPoint = ctx.undoPoint(doc, 'key', 0);
  doc.keys[0] = { module: 'clock', settings: {} };
  doc.keys[1] = { module: 'gpu', settings: {} };
  ctx.applyUndo(doc, keyPoint);
  assert.deepEqual(plain(doc.keys), [{ module: 'cpu', settings: { a: 1 } }, { module: 'gpu', settings: {} }]);
  // An Undo made before the document was reloaded from lite-deck does nothing.
  state.doc = doc;
  state.loads = 2;
  ctx.restoreSlot(Object.assign(ctx.undoPoint({ keys: [null] }, 'key', 0), { loads: 1 }));
  assert.equal(doc.keys[0].module, 'cpu');
  console.log('save.test.js: OK');
})().catch((e) => { console.error(e); process.exit(1); });
