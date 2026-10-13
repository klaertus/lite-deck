'use strict';
// Loads page scripts into a fresh context for node tests. The scripts only declare names at top
// level (app.js starts the page only when a document exists), so no DOM is needed for their pure
// helpers. Top-level const and let are reached with get(ctx, 'NAME').
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ASSETS = path.join(__dirname, '..', '..', 'src', 'web', 'assets');

function load(...files) {
  const ctx = vm.createContext({ console, setTimeout, clearTimeout, URL });
  for (const f of files) vm.runInContext(fs.readFileSync(path.join(ASSETS, f), 'utf8'), ctx, { filename: f });
  return ctx;
}

const get = (ctx, expr) => vm.runInContext(expr, ctx);
// Objects made in the context have its own prototypes: compare plain copies.
const plain = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));

module.exports = { load, get, plain, ASSETS };
