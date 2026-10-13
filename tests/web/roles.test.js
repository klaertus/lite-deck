'use strict';
// node tests/web/roles.test.js - the pure helpers of the Typography panel.
// appearance.js only declares names at top level; its page helpers (el, state, …) are never called.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', '..', 'src', 'web', 'assets', 'appearance.js'), 'utf8'), ctx);
// Objects made inside the context have its own prototypes: compare plain copies.
const plain = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));
const roleWith = (...args) => plain(ctx.roleWith(...args));

// Setting one field keeps only that field.
assert.deepEqual(roleWith(undefined, 'value', 'size', 40), { value: { size: 40 } });
// Other roles and fields are kept as they are.
assert.deepEqual(roleWith({ label: { weight: 700 } }, 'label', 'font', 'Bahnschrift'),
  { label: { weight: 700, font: 'Bahnschrift' } });
assert.deepEqual(roleWith({ label: { weight: 700 } }, 'value', 'weight', 400),
  { label: { weight: 700 }, value: { weight: 400 } });
// Back to the default (null, or an empty value) removes the field, then the emptied role, then the table.
assert.deepEqual(roleWith({ value: { size: 40, weight: 600 } }, 'value', 'size', null), { value: { weight: 600 } });
assert.deepEqual(roleWith({ value: { size: 40 }, label: { size: 20 } }, 'value', 'size', null), { label: { size: 20 } });
assert.equal(roleWith({ value: { size: 40 } }, 'value', 'size', null), undefined);
assert.equal(roleWith({ value: { font: 'Arial' } }, 'value', 'font', ''), undefined);
// The table given is never modified: the page swaps the whole table in.
const before = { value: { size: 40 } };
ctx.roleWith(before, 'value', 'weight', 700);
assert.deepEqual(before, { value: { size: 40 } });
// The six roles, in the order of the Device panel, with their config names and labels.
assert.deepEqual(plain(vm.runInContext('ROLES', ctx)), [['label', 'Label'], ['value', 'Value'],
  ['long_value', 'Long value'], ['two_line', 'Two-line'], ['secondary', 'Secondary'], ['symbol', 'Symbol']]);
assert.deepEqual(plain(vm.runInContext('[ROLE_SIZE_MIN, ROLE_SIZE_MAX]', ctx)), [8, 60]);
console.log('roles.test.js: OK');
