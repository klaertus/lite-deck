'use strict';
// node tests/web/interact.edge.js: the editing loop in a real browser, on the mock server: saves,
// lost saves, undo, the module catalogue at the keyboard, focus, errors at their field, moving
// slots and zone borders. Needs Microsoft Edge.
const assert = require('node:assert/strict');
const { startMock } = require('./mock-server');
const { launch, READY, sleep } = require('./edge');

async function open(edge, scenario, hash) {
  const mock = await startMock({ scenario });
  const page = await edge.page(1280, 900);
  await page.goto(mock.url + hash);
  await page.waitFor(READY);
  return { mock, page, done: async () => { await page.close(); await mock.close(); } };
}

// Dispatches a key press on the focused element (as Ctrl+Z, arrows... would arrive).
const key = (k, extra = '') => `document.activeElement.dispatchEvent(new KeyboardEvent('keydown', { key: '${k}', bubbles: true, cancelable: true${extra} }))`;

const CASES = {
  async 'a text setting is saved after a pause in typing, and undo saves the previous value'(edge) {
    const t = await open(edge, 'default', '#slots/key-8/content');  // weather: City
    const { page, mock } = t;
    await page.evaluate("const c = document.querySelector('[data-f=set-city]'); c.focus(); c.value = 'Namur'; c.dispatchEvent(new Event('input', { bubbles: true }))");
    await sleep(300);
    assert.equal(mock.puts.length, 0, 'not saved while typing');
    await page.waitFor('state.saving === false && state.dirty === false', 3000);
    assert.equal(mock.puts.at(-1).keys[7].settings.city, 'Namur');
    await page.evaluate("document.body.focus(); document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    await page.waitFor("state.dirty === false && state.saving === false && state.doc.keys[7].settings.city !== 'Namur'", 3000);
    assert.notEqual(mock.puts.at(-1).keys[7].settings.city, 'Namur');
    await t.done();
  },
  async 'a save that gets no answer keeps the changes and never reloads over them'(edge) {
    const t = await open(edge, 'offline', '#slots/key-8/content');
    const { page } = t;
    await page.evaluate("setSetting('city', 'Liège', { delay: 0 })");
    await page.waitFor("state.saving === false && document.querySelector('#save').textContent.startsWith('Not saved')", 3000);
    assert.equal(await page.evaluate('state.dirty'), true);
    assert.equal(await page.evaluate('mayReload(state)'), false);
    assert.equal(await page.evaluate("state.doc.keys[7].settings.city"), 'Liège');
    await t.done();
  },
  async 'arrows in the module catalogue change nothing; a click changes the module and offers Undo'(edge) {
    const t = await open(edge, 'default', '#slots/key-3/content');
    const { page, mock } = t;
    await page.evaluate("document.querySelector('[data-f=module]').click()");
    await page.evaluate("document.querySelector('[data-f=cat-clock]').focus()");
    for (const k of ['ArrowDown', 'ArrowDown', 'ArrowUp']) await page.evaluate(key(k));
    await sleep(500);
    assert.equal(mock.puts.length, 0);
    await page.evaluate("document.querySelector('[data-f=cat-clock]').click()");
    await page.waitFor("state.dirty === false && state.saving === false && state.doc.keys[2].module === 'clock'", 3000);
    assert.equal(mock.puts.at(-1).keys[2].module, 'clock');
    assert.match(await page.evaluate("document.querySelector('#toast').textContent"), /Undo/);
    await t.done();
  },
  async 'the keyboard focus survives a redraw'(edge) {
    const t = await open(edge, 'default', '#slots/key-3/content');
    const { page } = t;
    await page.evaluate("document.querySelector('[data-f=key-2]').focus(); renderAll()");
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'key-2');
    await page.evaluate("document.querySelector('[data-f=module]').focus(); renderPanel()");
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'module');
    await t.done();
  },
  async 'text typed and not committed yet survives a redraw of the panel'(edge) {
    const t = await open(edge, 'default', '#slots/key-8/content');
    const { page, mock } = t;
    await page.evaluate("const c = document.querySelector('[data-f=set-city]'); c.focus(); c.value = 'Nam'; c.dispatchEvent(new Event('input', { bubbles: true })); renderAll()");
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'set-city');
    assert.equal(await page.evaluate('document.activeElement.value'), 'Nam');
    assert.equal(mock.puts.length, 0);
    await t.done();
  },
  async 'text typed in one slot is saved to that slot, never carried to another one by back or forward'(edge) {
    const t = await open(edge, 'default', '#slots/key-4/content');  // GPU, like key 2
    const { page, mock } = t;
    await page.evaluate("location.hash = '#slots/key-2/content'");
    await page.waitFor('state.view.index === 1');
    await page.evaluate("const c = document.querySelector('[data-f=set-index]'); c.focus(); document.execCommand('selectAll'); document.execCommand('insertText', false, '3')");
    await page.evaluate('history.back()');  // the focus stays in the field: nothing blurs it
    await page.waitFor('state.view.index === 3');
    await page.evaluate('document.activeElement.blur()');
    await page.waitFor('state.dirty === false && state.saving === false && state.saveTimer === 0', 3000);
    assert.equal(await page.evaluate('state.doc.keys[1].settings.index'), 3);
    assert.equal(await page.evaluate('state.doc.keys[3].settings.index'), 0);
    assert.equal(await page.evaluate("document.querySelector('[data-f=set-index]').value"), '0');
    assert.ok(mock.puts.every((d) => d.keys[3].settings.index === 0));
    await page.evaluate('history.forward()');
    await page.waitFor('state.view.index === 1');
    assert.equal(await page.evaluate("document.querySelector('[data-f=set-index]').value"), '3');
    await t.done();
  },
  async 'merging a zone can be undone, and the strip comes back whole'(edge) {
    const t = await open(edge, 'default', '#slots/zone-2/content');
    const { page } = t;
    const before = await page.evaluate('JSON.stringify(state.doc.strip)');
    await page.evaluate("document.querySelector('[data-f=zone-merge-left]').click()");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.notEqual(await page.evaluate('JSON.stringify(state.doc.strip)'), before);
    await page.evaluate("[...document.querySelectorAll('#toast button')].find((b) => b.textContent === 'Undo').click()");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(await page.evaluate('JSON.stringify(state.doc.strip)'), before);
    await t.done();
  },
  async 'a change made elsewhere meanwhile is a conflict to settle, never a silent reload'(edge) {
    const t = await open(edge, 'default', '#slots/key-8/content');
    const { page, mock } = t;
    mock.revision += 1;  // another tab saved since this page loaded
    await page.evaluate("setSetting('city', 'Mine', { delay: 0 })");
    await page.waitFor("state.conflict > 0 && document.querySelector('#toast').textContent.includes('Keep my changes')", 3000);
    assert.equal(await page.evaluate('state.doc.keys[7].settings.city'), 'Mine');  // still there
    assert.equal(await page.evaluate('mayReload(state)'), false);
    await page.evaluate("[...document.querySelectorAll('#toast button')].find((b) => b.textContent === 'Keep my changes').click()");
    await page.waitFor('state.dirty === false && state.saving === false && state.conflict === 0', 3000);
    assert.equal(mock.doc.keys[7].settings.city, 'Mine');
    await t.done();
  },
  async 'a number out of bounds is brought back in when the field is left, with a note'(edge) {
    const t = await open(edge, 'default', '#slots/key-3/content');  // Memory (RAM): Warning at (% used), 0-100
    const { page, mock } = t;
    await page.evaluate("document.querySelector('details.group').open = true");
    await page.evaluate("const c = document.querySelector('[data-f=set-warn]'); c.value = '150'; c.dispatchEvent(new Event('input', { bubbles: true })); c.dispatchEvent(new Event('change', { bubbles: true }))");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(mock.puts.at(-1).keys[2].settings.warn, 100);
    assert.equal(await page.evaluate("document.querySelector('[data-f=set-warn]').value"), '100');
    assert.match(await page.evaluate("document.querySelector('#f-warn-note').textContent"), /Max 100/);
    await t.done();
  },
  async 'a number typed quickly is saved once, within bounds, and one undo puts back the value before typing'(edge) {
    const t = await open(edge, 'default', '#slots/key-3/content');  // Memory (RAM): Warning at (% used), 0-100, 85
    const { page, mock } = t;
    await page.evaluate("document.querySelector('details.group').open = true");
    assert.equal(await page.evaluate('state.doc.keys[2].settings.warn'), 85);
    for (const v of ['1', '15', '150']) {
      await page.evaluate(`{ const c = document.querySelector('[data-f=set-warn]'); c.focus(); c.value = '${v}'; c.dispatchEvent(new Event('input', { bubbles: true })); }`);
      await sleep(100);
    }
    // Ctrl+Z inside the field is the field's own: the page's undo is not used.
    await page.evaluate(key('z', ', ctrlKey: true'));
    assert.equal(await page.evaluate('state.doc.keys[2].settings.warn'), 100);
    await page.waitFor('state.saving === false && state.dirty === false', 3000);
    await sleep(300);
    assert.deepEqual(mock.puts.map((d) => d.keys[2].settings.warn), [100], 'one save, of the bounded value');
    assert.equal(await page.evaluate("document.querySelector('[data-f=set-warn]').value"), '150');  // as typed until left
    await page.evaluate("document.querySelector('[data-f=set-warn]').dispatchEvent(new Event('change', { bubbles: true })); document.activeElement.blur()");
    assert.equal(await page.evaluate("document.querySelector('[data-f=set-warn]').value"), '100');
    await sleep(300);
    assert.equal(mock.puts.length, 1, 'leaving the field sends nothing more');
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    await page.waitFor('state.saving === false && state.dirty === false && state.doc.keys[2].settings.warn === 85', 3000);
    assert.equal(mock.puts.at(-1).keys[2].settings.warn, 85);
    assert.equal(await page.evaluate('undoLog.canUndo()'), false, 'the typing was one step');
    await t.done();
  },
  async 'undo and redo during a conflict send nothing until the user chooses'(edge) {
    const t = await open(edge, 'default', '#slots/key-8/content');
    const { page, mock } = t;
    const city = await page.evaluate('state.doc.keys[7].settings.city');
    mock.revision += 1;  // another tab saved since this page loaded
    await page.evaluate("setSetting('city', 'Mine', { delay: 0 })");
    await page.waitFor('state.conflict > 0', 3000);
    const puts = mock.puts.length;
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    assert.equal(await page.evaluate('state.doc.keys[7].settings.city'), city);
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'y', ctrlKey: true, bubbles: true }))");
    assert.equal(await page.evaluate('state.doc.keys[7].settings.city'), 'Mine');
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    await sleep(500);
    assert.equal(mock.puts.length, puts, 'nothing sent during the conflict');
    assert.equal(await page.evaluate('state.dirty'), true);
    assert.match(await page.evaluate("document.querySelector('#toast').textContent"), /Keep my changes/);
    await page.evaluate("[...document.querySelectorAll('#toast button')].find((b) => b.textContent === 'Keep my changes').click()");
    await page.waitFor('state.dirty === false && state.saving === false && state.conflict === 0', 3000);
    assert.equal(mock.doc.keys[7].settings.city, city);
    await t.done();
  },
  async 'a fold whose fields are all hidden by show_if is hidden, and comes back with them'(edge) {
    const t = await open(edge, 'default', '#slots/key-4/content');  // gpu, Memory used
    const { page, mock } = t;
    const hidden = (sel) => page.evaluate(`(() => { const e = document.querySelector('${sel}'); return e.closest('details, .row').hidden; })()`);
    const fold = (g) => hidden(`[data-f="group-gpu.${g}"]`);
    const field = (k) => page.evaluate(`document.querySelector('#f-${k}').closest('.row').hidden`);
    const choose = async (mode) => {
      await page.evaluate(`{ const s = document.querySelector('[data-f=set-mode]'); s.value = '${mode}'; s.dispatchEvent(new Event('change', { bubbles: true })); }`);
      await page.waitFor(`state.dirty === false && state.saving === false && state.doc.keys[3].settings.mode === '${mode}'`, 3000);
    };
    assert.equal(await fold('gradient'), true, 'no Color scale in Memory used');
    assert.equal(await fold('thresholds'), false);
    assert.equal(await field('temp_warn'), true);
    assert.equal(await field('vram_warn'), false);
    await choose('usage');  // the temperature is off: its levels and scale stay hidden
    assert.equal(await fold('gradient'), true);
    assert.equal(await fold('thresholds'), true, 'nothing left in Warning levels');
    assert.equal(await field('show_temperature'), false);
    await choose('temp');
    assert.equal(await fold('gradient'), false);
    assert.equal(await fold('thresholds'), false);
    assert.equal(await field('temp_warn'), false);
    assert.equal(await field('vram_warn'), true);
    await choose('vram');
    assert.equal(await fold('gradient'), true);
    assert.equal(mock.puts.at(-1).keys[3].settings.temp_warn, 75, 'hidden values are kept');
    await t.done();
  },
  async 'a field an error points at is shown even when show_if hides it, and its link lands on it'(edge) {
    const t = await open(edge, 'default', '#slots/key-4/content');  // gpu, Memory used: temperature hidden
    const { page, mock } = t;
    mock.reject = () => [{ path: 'keys[3].settings', message: 'temp_warn: value too large (max 120)' }];
    await page.evaluate('scheduleSave(true)');
    await page.waitFor("!document.querySelector('#errors-btn').hidden", 3000);
    assert.equal(await page.evaluate("document.querySelector('#f-temp_warn').closest('.row').hidden"), false);
    assert.equal(await page.evaluate("document.querySelector('#f-temp_warn').closest('details').hidden"), false);
    assert.equal(await page.evaluate("document.querySelector('#f-temp_warn').closest('details').open"), true);
    assert.equal(await page.evaluate("document.querySelector('#f-temp_crit').closest('.row').hidden"), true, 'only the field in error');
    await page.evaluate("document.querySelector('#errors-btn').click()");
    await page.evaluate("document.querySelector('#error-list a[data-path=\"keys[3].settings.temp_warn\"]').click()");
    await page.waitFor("document.activeElement && document.activeElement.dataset.path === 'keys[3].settings.temp_warn'", 3000);
    // Fixed: once a save goes through, the field follows its show_if again.
    mock.reject = null;
    await page.evaluate("setSetting('index', 1, { delay: 0 })");
    await page.waitFor('state.dirty === false && state.saving === false && state.errors.length === 0', 3000);
    assert.equal(await page.evaluate("document.querySelector('#f-temp_warn').closest('.row').hidden"), true);
    await t.done();
  },
  async 'a refused save shows the error under its field and on the deck'(edge) {
    const t = await open(edge, 'error', '#slots/key-3/content');
    const { page } = t;
    await page.evaluate('scheduleSave(true)');
    await page.waitFor("!document.querySelector('#errors-btn').hidden", 3000);
    assert.equal(await page.evaluate("document.querySelector('#panel [data-path=\"keys[2].settings.warn\"]').getAttribute('aria-invalid')"), 'true');
    assert.match(await page.evaluate("document.querySelector('#panel [data-path=\"keys[2].settings.warn\"]').closest('.row').querySelector('.field-error').textContent"), /too large/);
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-2]').classList.contains('error')"), true);
    assert.equal(await page.evaluate("document.querySelector('#errors-btn').textContent"), '2 errors');
    await t.done();
  },
  async 'errors are listed on the overview, other changes still save, and the errors go once a save goes through'(edge) {
    const t = await open(edge, 'error', '#slots');
    const { page, mock } = t;
    await page.evaluate('scheduleSave(true)');
    await page.waitFor("!document.querySelector('#errors-btn').hidden", 3000);
    assert.match(await page.evaluate("document.querySelector('#save').textContent"), /^Not saved: see the errors$/);
    const top = await page.evaluate("document.querySelector('#slot-errors').hidden ? '' : document.querySelector('#slot-errors').textContent");
    assert.match(top, /Key 3 › .*too large \(max 100\)/);
    assert.match(top, /Strip: zone widths add up to 790 px/);
    assert.equal(await page.evaluate("document.querySelector('[data-f=zone-0]').classList.contains('error')"), true);
    // The header list leads to the field, focused.
    await page.evaluate("document.querySelector('#errors-btn').click()");
    assert.equal(await page.evaluate("document.querySelector('#error-list').hidden"), false);
    await page.evaluate("document.querySelector('#error-list a[data-path=\"keys[2].settings.warn\"]').click()");
    await page.waitFor("document.activeElement && document.activeElement.dataset.path === 'keys[2].settings.warn'", 3000);
    assert.equal(await page.evaluate('location.hash'), '#slots/key-3/content');
    // The errors are fixed: the next change saves, and every mark goes.
    mock.reject = null;
    const puts = mock.puts.length;
    await page.evaluate("setSetting('warn', 90, { delay: 0 })");
    await page.waitFor('state.dirty === false && state.saving === false && state.errors.length === 0', 3000);
    assert.ok(mock.puts.length > puts);
    assert.equal(await page.evaluate("document.querySelector('#errors-btn').hidden && document.querySelector('#error-list').hidden"), true);
    assert.equal(await page.evaluate("document.querySelectorAll('#panel [aria-invalid=true], .slot.error, .zone.error').length"), 0);
    assert.equal(await page.evaluate("document.querySelector('#f-warn-err').textContent"), '');
    await t.done();
  },
  async 'a dragged key swaps with the one it is dropped on; Ctrl+C and Ctrl+V copy one; one undo step each'(edge) {
    const t = await open(edge, 'default', '#slots/key-1/content');
    const { page, mock } = t;
    await page.evaluate(`{
      const data = new DataTransfer();
      const from = document.querySelector('[data-f=key-0]');
      const to = document.querySelector('[data-f=key-4]');
      from.dispatchEvent(new DragEvent('dragstart', { dataTransfer: data, bubbles: true }));
      to.dispatchEvent(new DragEvent('dragover', { dataTransfer: data, bubbles: true, cancelable: true }));
      to.dispatchEvent(new DragEvent('drop', { dataTransfer: data, bubbles: true, cancelable: true }));
    }`);
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(mock.puts.at(-1).keys[4].module, 'cpu_load');
    assert.equal(mock.puts.at(-1).keys[0].module, 'stopwatch');
    assert.equal(await page.evaluate('location.hash'), '#slots/key-5/content');
    await page.evaluate("document.body.focus(); document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'c', ctrlKey: true, bubbles: true }))");
    await page.evaluate("location.hash = '#slots/key-7/content'");
    await page.waitFor("location.hash === '#slots/key-7/content'");
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'v', ctrlKey: true, bubbles: true }))");
    await page.waitFor("state.dirty === false && state.saving === false && state.doc.keys[6].module === 'cpu_load'", 3000);
    assert.equal(mock.puts.at(-1).keys[6].module, 'cpu_load');
    await page.evaluate('undo()');
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(mock.puts.at(-1).keys[6].module, 'date');
    await t.done();
  },
  async 'Swap with... swaps from the keyboard and its Undo puts both back; Ctrl+C in a text field copies no slot'(edge) {
    const t = await open(edge, 'default', '#slots/key-1/content');
    const { page, mock } = t;
    await page.evaluate("document.querySelector('[data-f=act-move]').click()");
    assert.match(await page.evaluate('document.activeElement.textContent'), /^Key 2 /);
    await page.evaluate("[...document.querySelectorAll('.menu button')].find((b) => b.textContent.startsWith('Key 5 ')).click()");
    await page.waitFor("state.dirty === false && state.saving === false && state.doc.keys[4].module === 'cpu_load'", 3000);
    assert.equal(mock.puts.at(-1).keys[0].module, 'stopwatch');
    assert.equal(await page.evaluate('location.hash'), '#slots/key-5/content');
    await page.evaluate("[...document.querySelectorAll('#toast button')].find((b) => b.textContent === 'Undo').click()");
    await page.waitFor("state.dirty === false && state.saving === false && state.doc.keys[0].module === 'cpu_load'", 3000);
    assert.equal(mock.puts.at(-1).keys[4].module, 'stopwatch');
    await page.evaluate("location.hash = '#slots/key-8/content'");
    await page.waitFor("document.querySelector('[data-f=set-city]')");
    await page.evaluate("const c = document.querySelector('[data-f=set-city]'); c.focus(); c.select()");
    await page.evaluate(key('c', ', ctrlKey: true'));
    assert.equal(await page.evaluate('clipboard'), null);
    // A zone moved by its Move → button keeps its width and stays selected.
    await page.evaluate("location.hash = '#slots/zone-1/content'");
    await page.waitFor("document.querySelector('[data-f=zone-right]')");
    const z0 = await page.evaluate('JSON.stringify(state.doc.strip[0])');
    await page.evaluate("document.querySelector('[data-f=zone-right]').click()");
    await page.waitFor("state.dirty === false && state.saving === false && location.hash === '#slots/zone-2/content'", 3000);
    assert.equal(JSON.stringify(mock.puts.at(-1).strip[1]), z0);
    await t.done();
  },
  async 'Swap with... is a labelled list that Escape closes; error marks follow a swap during a conflict, and undo puts them back'(edge) {
    const t = await open(edge, 'error', '#slots/key-3/content');
    const { page, mock } = t;
    await page.evaluate('scheduleSave(true)');
    await page.waitFor("!document.querySelector('#errors-btn').hidden", 3000);
    await page.evaluate("document.querySelector('[data-f=act-move]').click()");
    assert.equal(await page.evaluate("document.querySelector('[data-f=act-move]').getAttribute('aria-expanded')"), 'true');
    assert.equal(await page.evaluate("document.querySelector('[data-f=act-move]').getAttribute('aria-controls')"), 'slot-menu');
    assert.equal(await page.evaluate("document.querySelector('#slot-menu').getAttribute('aria-label')"), 'Swap Key 3 with');
    assert.equal(await page.evaluate("document.activeElement.parentNode.id"), 'slot-menu');
    await page.evaluate(key('Escape'));
    assert.equal(await page.evaluate("document.querySelector('#slot-menu').hidden"), true);
    assert.equal(await page.evaluate("document.activeElement.dataset.f"), 'act-move');
    assert.equal(await page.evaluate("document.querySelector('[data-f=act-move]').getAttribute('aria-expanded')"), 'false');
    // Another page saved meanwhile: the swap is held back, and its marks follow the swapped keys.
    mock.reject = null;
    mock.revision += 1;
    await page.evaluate("document.querySelector('[data-f=act-move]').click()");
    await page.evaluate("document.querySelector('#slot-menu [data-f=to-4]').click()");
    await page.waitFor('state.conflict > 0', 3000);
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-4]').classList.contains('error')"), true);
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-2]').classList.contains('error')"), false);
    assert.equal(await page.evaluate("document.querySelector('#errors-btn').textContent"), '2 errors');
    assert.match(await page.evaluate("document.querySelector('#error-list').textContent"), /Key 5 › /);
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-2]').classList.contains('error')"), true);
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-4]').classList.contains('error')"), false);
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'y', ctrlKey: true, bubbles: true }))");
    assert.equal(await page.evaluate("document.querySelector('[data-f=key-4]').classList.contains('error')"), true);
    await t.done();
  },
  async 'a zone border moved with the keyboard changes only its two zones, in one save'(edge) {
    const t = await open(edge, 'default', '#slots/zone-1/content');
    const { page, mock } = t;
    const [w0, w1] = await page.evaluate('state.doc.strip.map((z) => z.width)');
    await page.evaluate("document.querySelector('[data-f=handle-1]').focus()");
    await page.evaluate(key('ArrowRight'));
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.deepEqual(mock.puts.at(-1).strip.map((z) => z.width), [w0 + 10, w1 - 10]);
    assert.equal(mock.puts.length, 1);
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'handle-1');
    assert.equal(await page.evaluate("document.querySelector('[data-f=zone-width]').value"), String(w0 + 10));  // Zone 1, the left one
    // Held down: one save and one undo step when the key is released.
    for (let n = 0; n < 5; n += 1) await page.evaluate(key('ArrowLeft', n ? ', repeat: true' : ''));
    await page.evaluate("document.activeElement.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowLeft', bubbles: true }))");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(mock.puts.length, 2);
    assert.deepEqual(mock.puts.at(-1).strip.map((z) => z.width), [w0 - 40, w1 + 40]);
    await page.evaluate("document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true }))");
    assert.deepEqual(await page.evaluate('state.doc.strip.map((z) => z.width)'), [w0 + 10, w1 - 10]);
    await t.done();
  },
  async 'the zone Width field saves a pause after typing, without leaving it'(edge) {
    const t = await open(edge, 'default', '#slots/zone-1/content');
    const { page, mock } = t;
    const total = await page.evaluate('state.doc.strip.reduce((s, z) => s + z.width, 0)');
    await page.evaluate("const f = document.querySelector('[data-f=zone-width]'); f.focus(); f.value = '250'; f.dispatchEvent(new Event('input', { bubbles: true }))");
    await page.waitFor('state.dirty === false && state.saving === false && state.saveTimer === 0', 3000);
    assert.equal(mock.puts.length, 1);
    assert.equal(mock.puts.at(-1).strip[0].width, 250);
    assert.equal(mock.puts.at(-1).strip.reduce((s, z) => s + z.width, 0), total);
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'zone-width');
    await t.done();
  },
  async 'a saved change leaves the panel as it is: no redraw, nothing typed lost'(edge) {
    const t = await open(edge, 'default', '#slots/key-8/content');
    const { page } = t;
    await page.evaluate("document.querySelector('#panel-body').dataset.mark = 'kept'");
    await page.evaluate("setSetting('city', 'Namur', { delay: 0 })");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(await page.evaluate("document.querySelector('#panel-body').dataset.mark"), 'kept');
    await page.evaluate("setModule('clock')");  // a new module draws the panel once, not again after the save
    await page.evaluate("document.querySelector('#panel-body').dataset.mark = 'kept'");
    await page.waitFor('state.dirty === false && state.saving === false', 3000);
    assert.equal(await page.evaluate("document.querySelector('#panel-body').dataset.mark"), 'kept');
    await t.done();
  },
  async 'arrows move across the deck and Escape comes back from the panel'(edge) {
    const t = await open(edge, 'default', '#slots/key-1/content');
    const { page } = t;
    await page.evaluate("document.querySelector('[data-f=key-0]').focus()");
    await page.evaluate(key('ArrowRight'));
    await page.evaluate(key('ArrowDown'));
    assert.equal(await page.evaluate('location.hash'), '#slots/key-7/content');
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'key-6');
    await page.evaluate("document.querySelector('[data-f=module]').focus()");
    await page.evaluate(key('Escape'));
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'key-6');
    await t.done();
  },
  async 'the deck keys: one Tab stop per group, Home and End, no history entry, Alt+arrows left to the browser'(edge) {
    const t = await open(edge, 'default', '#slots/key-3/content');
    const { page, mock } = t;
    const stops = "[...document.querySelectorAll('#keys [data-kind]')].filter((b) => b.tabIndex === 0).map((b) => b.dataset.f).join()";
    assert.equal(await page.evaluate(stops), 'key-2');
    const length = await page.evaluate('window.history.length');
    await page.evaluate("document.querySelector('[data-f=key-2]').focus()");
    await page.evaluate(key('End'));
    assert.equal(await page.evaluate('location.hash'), '#slots/key-10/content');
    assert.equal(await page.evaluate(stops), 'key-9');
    await page.evaluate(key('ArrowDown'));  // the bottom row: nowhere to go
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'key-9');
    await page.evaluate(key('Home'));
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'key-0');
    assert.equal(await page.evaluate('window.history.length'), length);
    assert.equal(await page.evaluate(key('ArrowLeft', ', altKey: true')), true, 'Alt+Left belongs to the browser');
    await page.evaluate("document.querySelector('[data-f=zone-0]').focus()");
    await page.evaluate(key('ArrowRight'));
    assert.equal(await page.evaluate('location.hash'), '#slots/zone-2/content');
    assert.equal(mock.puts.length, 0, 'moving across the deck saves nothing');
    // Escape in the search box of the catalogue empties it first.
    await page.evaluate("document.querySelector('[data-f=key-0]').click()");
    await page.waitFor("location.hash === '#slots/key-1/content' && !!document.querySelector('[data-f=module]')");
    await page.evaluate("document.querySelector('[data-f=module]').click()");
    await page.evaluate("const s = document.querySelector('[data-f=cat-search]'); s.focus(); s.value = 'cpu'");
    assert.equal(await page.evaluate(key('Escape')), true);
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'cat-search');
    await t.done();
  },
  async 'the sub-tabs and the Swap with... list move with the arrow keys'(edge) {
    const t = await open(edge, 'default', '#slots/key-1/content');
    const { page } = t;
    await page.evaluate("document.querySelector('[data-f=sub-content]').focus()");
    assert.equal(await page.evaluate("document.querySelector('[data-f=sub-look]').tabIndex"), -1);
    await page.evaluate(key('ArrowRight'));
    assert.equal(await page.evaluate('location.hash'), '#slots/key-1/look');
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'sub-look');
    assert.equal(await page.evaluate("document.querySelector('[data-f=sub-look]').tabIndex"), 0);
    await page.evaluate(key('ArrowLeft'));
    await page.evaluate(key('ArrowLeft'));  // wraps to the last tab
    assert.equal(await page.evaluate('location.hash'), '#slots/key-1/tap');
    await page.evaluate("document.querySelector('[data-f=act-move]').click()");
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'to-1');
    await page.evaluate(key('ArrowDown'));
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'to-2');
    await page.evaluate(key('End'));
    assert.equal(await page.evaluate('document.activeElement.dataset.f'), 'to-9');
    assert.equal(await page.evaluate("[...document.querySelectorAll('#slot-menu button')].filter((b) => b.tabIndex === 0).length"), 1);
    await t.done();
  },
  async 'a zone border dragged with the mouse saves once on release; a lost capture cancels the drag'(edge) {
    const t = await open(edge, 'default', '#slots/zone-1/content');
    const { page, mock } = t;
    const [w0, w1] = await page.evaluate('state.doc.strip.map((z) => z.width)');
    const at = "(() => { const r = document.querySelector('[data-f=handle-1]').getBoundingClientRect(); return [r.x + r.width / 2, r.y + r.height / 2, document.querySelector('#strip').clientWidth / stripWidth()]; })()";
    let [x, y, scale] = await page.evaluate(at);
    assert.equal(await page.evaluate("document.querySelector('[data-f=handle-1]').getAttribute('aria-valuetext')"), `Zone 1: ${w0} px, zone 2: ${w1} px`);
    await page.mouse('mousePressed', x, y);
    await page.mouse('mouseMoved', x + 26 * scale, y);
    await page.mouse('mouseMoved', x + 52 * scale, y);
    assert.equal(mock.puts.length, 0, 'nothing saved while the border moves');
    await page.mouse('mouseReleased', x + 52 * scale, y);
    await page.waitFor('state.dirty === false && state.saving === false && state.saveTimer === 0', 3000);
    assert.equal(mock.puts.length, 1);
    assert.deepEqual(mock.puts.at(-1).strip.slice(0, 2).map((z) => z.width), [w0 + 50, w1 - 50]);
    // The capture lost in the middle (another window, the border redrawn): nothing changes.
    [x, y] = await page.evaluate(at);
    await page.mouse('mousePressed', x, y);
    await page.mouse('mouseMoved', x + 80 * scale, y);
    await page.evaluate("document.querySelector('[data-f=handle-1]').releasePointerCapture(1)");
    await page.mouse('mouseMoved', x + 90 * scale, y);  // the capture is let go before the next pointer event
    assert.equal(await page.evaluate("document.querySelector('[data-f=handle-1]').classList.contains('active')"), false);
    await page.mouse('mouseReleased', x + 80 * scale, y);
    await sleep(300);
    assert.equal(mock.puts.length, 1);
    assert.deepEqual(await page.evaluate('state.doc.strip.slice(0, 2).map((z) => z.width)'), [w0 + 50, w1 - 50]);
    assert.equal(await page.evaluate("document.querySelector('[data-f=handle-1]').style.left === pct(state.doc.strip[0].width)"), true, 'the border is back');
    // Alt and Ctrl with an arrow are not the border's.
    await page.evaluate("document.querySelector('[data-f=handle-1]').focus()");
    assert.equal(await page.evaluate(key('ArrowLeft', ', altKey: true')), true);
    assert.equal(await page.evaluate(key('ArrowRight', ', ctrlKey: true')), true);
    await sleep(300);
    assert.equal(mock.puts.length, 1);
    await t.done();
  },
  async 'refusals, conflicts and the end of the session are said once, by the one alert of the page'(edge) {
    const t = await open(edge, 'error', '#slots/key-3/content');
    const { page, mock } = t;
    await page.evaluate(`window.said = []; window.realFetch = window.fetch;
      new MutationObserver(() => { const a = document.getElementById('alert'); if (a.textContent) said.push(a.textContent); })
        .observe(document.getElementById('alert'), { childList: true, characterData: true, subtree: true })`);
    // What the polite regions say: their text, less what is aria-hidden.
    const spoken = (id) => `(() => { const c = document.getElementById('${id}').cloneNode(true); for (const h of c.querySelectorAll('[aria-hidden=true]')) h.remove(); return c.textContent; })()`;
    const said = () => page.evaluate('window.said');
    // A refused save (422), twice: said twice, by the alert only.
    await page.evaluate('scheduleSave(true)');
    await page.waitFor('window.said.length === 1', 3000);
    assert.match((await said())[0], /^Not saved: 2 errors\. Key 3 › /);
    assert.equal(await page.evaluate(spoken('save')), '');
    assert.match(await page.evaluate("document.querySelector('#save').textContent"), /^Not saved/);
    await page.evaluate('scheduleSave(true)');
    await page.waitFor('window.said.length === 2', 3000);
    // Refused for good (413).
    mock.reject = null;
    await page.evaluate("window.fetch = (u, o) => (o && o.method === 'PUT' ? Promise.resolve(new Response('{}', { status: 413 })) : realFetch(u, o))");
    await page.evaluate('scheduleSave(true)');
    await page.waitFor('window.said.length === 3', 3000);
    assert.match((await said())[2], /error 413/);
    assert.equal(await page.evaluate(spoken('toast')), '');
    // A conflict (412): the alert says it, the toast only offers its two buttons.
    await page.evaluate('window.fetch = realFetch');
    mock.revision += 1;
    await page.evaluate('scheduleSave(true)');
    await page.waitFor('window.said.length === 4', 3000);
    assert.match((await said())[3], /changed outside this page/);
    assert.equal(await page.evaluate(spoken('toast')), 'Keep my changesLoad the other version');
    await page.evaluate('scheduleSave(false)');  // a change meanwhile: not said again
    await sleep(200);
    assert.equal((await said()).length, 4);
    // The session ends (401): the alert stays in the page and says it.
    await page.evaluate("window.fetch = (u, o) => (o && o.method === 'PUT' ? Promise.resolve(new Response('{}', { status: 401 })) : realFetch(u, o))");
    await page.evaluate("[...document.querySelectorAll('#toast button')].find((b) => b.textContent === 'Keep my changes').click()");
    await page.waitFor('window.said.length === 5', 3000);
    assert.match((await said())[4], /^Session expired/);
    assert.equal(await page.evaluate("document.querySelectorAll('[role=alert]#alert').length"), 1);
    await t.done();
  },
  async 'text typed in a font field and not committed yet asks before the page is left, and fatal() offers it'(edge) {
    const t = await open(edge, 'default', '#look');
    const { page, mock } = t;
    const leave = "(() => { const e = new Event('beforeunload', { cancelable: true }); window.dispatchEvent(e); return e.defaultPrevented; })()";
    assert.equal(await page.evaluate(leave), false, 'nothing to lose yet');
    await page.evaluate("const f = document.querySelector('[data-f=look-font]'); f.focus(); f.select()");
    await page.type('Bahnschrift');  // a real keyboard entry: a script's value would fire no change when left
    await sleep(400);
    assert.equal(mock.puts.length, 0, 'a font name is committed when left, not while typed');
    assert.equal(await page.evaluate('state.dirty'), false);
    assert.equal(await page.evaluate(leave), true, 'leaving asks first');
    await page.evaluate("fatal('Session expired')");
    assert.match(await page.evaluate("document.querySelector('.fatal textarea').value"), /"font": "Bahnschrift"/);
    await t.done();
  },
  async 'a stream refused while the daemon answers (a third tab) says to use one tab, not that lite-deck is down'(edge) {
    const mock = await startMock({ scenario: 'default' });
    mock.full = true;
    const page = await edge.page(1280, 900);
    await page.goto(mock.url + '#slots');
    await page.waitFor("document.querySelector('#device').textContent.startsWith('Open in one tab')", 5000);
    mock.full = false;
    await page.close();
    await mock.close();
  },
};

async function main() {
  const edge = await launch();
  let failed = 0;
  try {
    for (const [name, run] of Object.entries(CASES)) {
      try {
        await run(edge);
        console.log(`ok   ${name}`);
      } catch (e) {
        failed += 1;
        console.log(`FAIL ${name}\n     ${e.message}`);
      }
    }
  } finally {
    await edge.close();
  }
  if (failed) process.exit(1);
  console.log('interact.edge.js: OK');
}

main().catch((e) => { console.error(e); process.exit(1); });
