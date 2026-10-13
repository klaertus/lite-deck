'use strict';
// node tests/web/budget.edge.js: the page's budgets that only a browser can measure (spec §7), on
// the mock server with a typical N4 Pro configuration and 260 installed fonts:
// - at most 1,500 elements in the DOM on every screen;
// - at most 7 static requests at load (the page, its stylesheet and scripts);
// - each preview fetched once at load (no double load);
// - no script error and no CSP refusal.
const { startMock } = require('./mock-server');
const { launch, READY, sleep } = require('./edge');

const MAX_NODES = 1500;
const MAX_STATIC = 7;

async function main() {
  const mock = await startMock();
  const edge = await launch();
  const failures = [];
  try {
    const page = await edge.page(1280, 900);
    await page.goto(`${mock.url}#slots/key-1/content`);
    await page.waitFor(READY);
    await sleep(1500);
    const statics = mock.requests.filter((r) => r.startsWith('GET ') && !r.startsWith('GET /api/') && r !== 'GET /favicon.ico');
    if (statics.length > MAX_STATIC) failures.push(`${statics.length} static requests at load (max ${MAX_STATIC}): ${statics.join(', ')}`);
    const previews = mock.requests.filter((r) => r.startsWith('GET /api/preview/'));
    const targets = new Set(previews);
    if (previews.length !== targets.size) failures.push(`previews fetched twice at load: ${previews.length} requests for ${targets.size} targets`);
    // Every screen: each slot and each of its tabs, Look and Hardware, the catalogue open, all parts unfolded.
    const views = await page.evaluate(`(() => {
      if (typeof subTabsFor !== 'function') return [location.hash];  // a page without addresses
      const out = ['#slots', '#look', '#hardware'];
      for (const kind of ['key', 'knob', 'zone']) {
        const n = kind === 'key' ? keyCount() : kind === 'knob' ? state.device.knobs : state.doc.strip.length;
        for (let i = 0; i < n; i += 1) for (const [sub] of subTabsFor(kind, listOf(state.doc, kind)[i])) out.push(formatHash({ tab: 'slots', kind, index: i, sub }));
      }
      return out;
    })()`);
    let worst = ['', 0];
    for (const hash of views) {
      await page.evaluate(`location.hash = '${hash}'`);
      await page.waitFor(`location.hash === '${hash}' && !!document.querySelector('#panel h2')`);
      await page.evaluate(`{
        for (const b of document.querySelectorAll('.part-toggle[aria-expanded=false]')) b.click();
        const pick = document.querySelector('button[data-f=module][aria-expanded=false]');
        if (pick) pick.click();
      }`);
      const nodes = await page.evaluate("document.getElementsByTagName('*').length");
      if (nodes > worst[1]) worst = [hash, nodes];
      if (nodes > MAX_NODES) failures.push(`${hash}: ${nodes} elements (max ${MAX_NODES})`);
    }
    const device = await page.evaluate("(() => { const b = document.querySelector('#device-btn'); if (!b) return 0; b.click(); return document.getElementsByTagName('*').length; })()");
    if (device > MAX_NODES) failures.push(`Device panel: ${device} elements (max ${MAX_NODES})`);
    if (device > worst[1]) worst = ['the Device panel', device];
    console.log(`largest DOM: ${worst[1]} elements on ${worst[0]} (${views.length} screens)`);
    if (page.problems.length) failures.push(...page.problems);
  } finally {
    await edge.close();
    await mock.close();
  }
  if (failures.length) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
  console.log('budget.edge.js: OK');
}

main().catch((e) => { console.error(e); process.exit(1); });
