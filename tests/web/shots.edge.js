'use strict';
// node tests/web/shots.edge.js [out-dir]: screenshots of the key screens of the page, on the mock
// server, with Edge headless. Default folder: build/web-shots (git-ignored). Fails on a script error
// or a CSP refusal in the page. Shown to the user at the visual checkpoints; never compared by a test.
const path = require('node:path');
const { startMock } = require('./mock-server');
const { launch, READY } = require('./edge');

const WIDE = [1280, 900];
const NARROW = [800, 900];
// [file, scenario, address, size, then: a script run once the page is ready, wait: what to wait for]
const SHOTS = [
  ['01-overview', 'default', '#slots', WIDE],
  ['02-key-content', 'default', '#slots/key-2/content', WIDE],
  ['03-key-look', 'default', '#slots/key-1/look', WIDE, "document.querySelector('[data-f=part-value]')?.click()"],
  ['04-key-tap', 'default', '#slots/key-1/tap', WIDE],
  ['05-key-catalogue', 'default', '#slots/key-3/content', WIDE, "document.querySelector('button[data-f=module]')?.click()"],
  ['06-knob-content', 'default', '#slots/knob-1/content', WIDE],
  ['07-knob-ring', 'default', '#slots/knob-1/ring', WIDE],
  ['08-zone-content', 'default', '#slots/zone-1/content', WIDE],
  ['09-zone-look', 'default', '#slots/zone-2/look', WIDE],
  ['10-look', 'default', '#look', WIDE],
  ['11-hardware', 'default', '#hardware', WIDE],
  ['12-error', 'error', '#slots/key-3/content', WIDE, 'scheduleSave(true)', 'state.errors.length > 0'],
  ['13-empty-key', 'empty', '#slots/key-1/content', WIDE],
  ['14-file-error', 'file-error', '#slots/key-1/content', WIDE, '', "!document.querySelector('#banner').hidden"],
  ['15-narrow', 'default', '#slots/key-4/content', NARROW],
];

async function main() {
  const out = path.resolve(process.argv[2] || path.join(__dirname, '..', '..', 'build', 'web-shots'));
  const only = process.env.LD_SHOTS ? new RegExp(process.env.LD_SHOTS) : null;
  const edge = await launch();
  const failures = [];
  try {
    for (const [name, scenario, hash, [w, h], then, wait] of SHOTS) {
      if (only && !only.test(name)) continue;
      const mock = await startMock({ scenario });
      const page = await edge.page(w, h);
      try {
        await page.goto(mock.url + hash);
        await page.waitFor(READY);
        if (then) await page.evaluate(then);
        if (wait) await page.waitFor(wait);
        await page.waitFor("[...document.images].every((i) => i.complete)");
        await page.evaluate('new Promise((r) => setTimeout(r, 300))');
        await page.screenshot(path.join(out, `${name}.png`));
        if (page.problems.length) failures.push(`${name}: ${page.problems.join(' | ')}`);
        console.log(`${name}.png`);
      } catch (e) {
        failures.push(`${name}: ${e.message}`);
      } finally {
        await page.close();
        await mock.close();
      }
    }
  } finally {
    await edge.close();
  }
  console.log(`screenshots in ${out}`);
  if (failures.length) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
