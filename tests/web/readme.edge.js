'use strict';
// node tests/web/readme.edge.js: the README's pictures of the page, docs/images/web-ui.png and
// docs/images/web-ui-look.png, taken with Edge headless on the mock server (made-up data; the key
// images are tests/golden, the strip is tests/web/fixtures/strip.png from the "doc: deck image" test).
const fs = require('node:fs');
const path = require('node:path');
const { startMock } = require('./mock-server');
const { launch, READY } = require('./edge');

const OUT = path.join(__dirname, '..', '..', 'docs', 'images');
const SHOTS = [
  ['web-ui.png', '#slots/key-1/look', "document.querySelector('[data-f=part-value]').click()"],
  ['web-ui-look.png', '#look', ''],
];

async function main() {
  const edge = await launch();
  const failures = [];
  try {
    for (const [file, hash, then] of SHOTS) {
      const mock = await startMock({ scenario: 'readme' });
      const page = await edge.page(1280, 800);
      try {
        await page.goto(mock.url + hash);
        await page.waitFor(READY);
        if (then) await page.evaluate(then);
        await page.waitFor('[...document.images].every((i) => i.complete && i.naturalWidth > 0)');
        await page.evaluate('new Promise((r) => setTimeout(r, 300))');
        const out = path.join(OUT, file);
        await page.screenshot(out);
        const kb = Math.round(fs.statSync(out).size / 1024);
        console.log(`${out} (${kb} KB)`);
        if (kb > 400) failures.push(`${file}: ${kb} KB, more than 400 KB`);
        if (page.problems.length) failures.push(`${file}: ${page.problems.join(' | ')}`);
      } finally {
        await page.close();
        await mock.close();
      }
    }
  } finally {
    await edge.close();
  }
  if (failures.length) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
