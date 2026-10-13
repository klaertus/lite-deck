'use strict';
// node tests/web/a11y.edge.js: what assistive technology gets from the page, on every screen: every
// control has a name; the deck slots are named "Key 4, CPU load" and
// the selected one is aria-current; each deck group is one Tab stop (roving tabindex); the save
// state, the device state and the toast are live regions that stay in the accessibility tree;
// the zone borders are focusable separators with a value; targets are at least 24 px. Needs Microsoft Edge.
const { startMock } = require('./mock-server');
const { launch, READY } = require('./edge');

async function main() {
  const mock = await startMock();
  const edge = await launch();
  const failures = [];
  try {
    const page = await edge.page(1280, 900);
    await page.goto(`${mock.url}#slots/key-1/content`);
    await page.waitFor(READY);
    const fixed = await page.evaluate(`[
      document.querySelector('#save').getAttribute('role'), document.querySelector('#device').getAttribute('role'),
      document.querySelector('#toast').getAttribute('role'), document.querySelector('#banner').getAttribute('role'),
      document.querySelector('[data-f=key-3]').getAttribute('aria-label')]`);
    if (fixed.slice(0, 4).join(' ') !== 'status status status alert') failures.push(`live regions: ${fixed.slice(0, 4).join(' ')}`);
    if (!/^Key 4, \S/.test(fixed[4])) failures.push(`slot name: ${fixed[4]}`);
    // A live region hidden with display: none is out of the accessibility tree: what is written
    // into it as it appears is often not read. Empty, they stay in the tree.
    const gone = await page.evaluate(`['save', 'device', 'toast', 'alert'].filter((id) => {
      const n = document.getElementById(id);
      n.textContent = '';
      return n.hidden || getComputedStyle(n).display === 'none';
    })`);
    if (gone.length) failures.push(`live regions out of the tree when empty: ${gone.join(' ')}`);
    // One alert for refusals and the end of the session, there from the start (interact.edge.js).
    if (await page.evaluate("document.querySelectorAll('[role=alert]#alert').length") !== 1) failures.push('no #alert role=alert at load');
    // The zone borders: focusable separators (not buttons) with a value.
    const sep = await page.evaluate(`(() => {
      const h = document.querySelector('[data-f=handle-1]');
      return h ? [h.tagName, h.getAttribute('role'), h.getAttribute('aria-orientation'), h.tabIndex >= -1 && h.hasAttribute('tabindex'),
        h.getAttribute('aria-valuenow'), h.getAttribute('aria-valuemin'), h.getAttribute('aria-valuemax'), h.getAttribute('aria-valuetext')] : null;
    })()`);
    if (!sep || sep[0] === 'BUTTON' || sep[1] !== 'separator' || sep[2] !== 'vertical' || !sep[3] || sep.slice(4).some((x) => !x)) {
      failures.push(`zone border: ${JSON.stringify(sep)}`);
    }
    for (const hash of ['#slots', '#slots/key-1/content', '#slots/key-1/look', '#slots/key-1/tap', '#slots/knob-1/ring',
      '#slots/zone-1/content', '#slots/zone-2/look', '#look', '#hardware']) {
      await page.evaluate(`location.hash = '${hash}'`);
      await page.waitFor(`location.hash === '${hash}' && !!document.querySelector('#panel h2')`);
      await page.evaluate("for (const b of document.querySelectorAll('.part-toggle[aria-expanded=false]')) b.click();");
      const unnamed = await page.evaluate(`(() => {
        const out = [];
        for (const n of document.querySelectorAll('button, input, select, textarea, a[href], [role=tab], [role=separator]')) {
          if (n.closest('[hidden]') || n.type === 'hidden' || getComputedStyle(n).visibility === 'hidden') continue;
          const label = n.getAttribute('aria-label') || (n.id && document.querySelector('label[for="' + n.id + '"]') ? 'label' : '')
            || n.getAttribute('aria-labelledby') || n.textContent.trim() || n.getAttribute('title') || n.getAttribute('placeholder');
          if (!label) out.push(n.outerHTML.slice(0, 120));
          const r = n.getBoundingClientRect();
          if (r.width > 0 && (r.width < 24 || r.height < 24) && !n.closest('.crumbs, .prov, .note, .notice, .overview')) out.push('small target: ' + n.outerHTML.slice(0, 100) + ' ' + Math.round(r.width) + 'x' + Math.round(r.height));
        }
        if (document.querySelectorAll('#deck [aria-current=true]').length > 1) out.push('two slots are aria-current');
        // Roving tabindex: one Tab stop per group of the deck, the selected slot if it is there.
        for (const g of ['keys', 'strip', 'knobs']) {
          const stops = [...document.getElementById(g).querySelectorAll('[data-kind]')].filter((b) => b.tabIndex === 0);
          if (stops.length !== 1) out.push(g + ': ' + stops.length + ' Tab stops');
          const cur = document.getElementById(g).querySelector('[aria-current=true]');
          if (cur && cur.tabIndex !== 0) out.push(g + ': the selected slot is not the Tab stop');
        }
        // Sub-tabs: one Tab stop, the selected tab.
        const tabs = [...document.querySelectorAll('#panel [role=tab]')];
        if (tabs.length && tabs.filter((t) => t.tabIndex === 0).map((t) => t.getAttribute('aria-selected')).join() !== 'true') out.push('sub-tabs: Tab stop is not the selected tab');
        return out;
      })()`);
      for (const u of unnamed) failures.push(`${hash}: ${u}`);
    }
    if (page.problems.length) failures.push(...page.problems);
  } finally {
    await edge.close();
    await mock.close();
  }
  if (failures.length) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
  console.log('a11y.edge.js: OK');
}

main().catch((e) => { console.error(e); process.exit(1); });
