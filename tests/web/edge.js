'use strict';
// Microsoft Edge without a window (--headless), driven through the DevTools protocol with the
// WebSocket built into Node 22. A fresh profile in %TEMP% each time: the user's Edge, profile and
// settings are never touched. LD_EDGE names msedge.exe when it is not in the usual place.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function findEdge() {
  const places = [
    process.env.LD_EDGE,
    path.join(process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)', 'Microsoft', 'Edge', 'Application', 'msedge.exe'),
    path.join(process.env.ProgramFiles || 'C:\\Program Files', 'Microsoft', 'Edge', 'Application', 'msedge.exe'),
  ].filter(Boolean);
  const found = places.find((p) => fs.existsSync(p));
  if (!found) throw new Error(`Microsoft Edge not found (looked in ${places.join(', ')}): set LD_EDGE to msedge.exe`);
  return found;
}

async function launch() {
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'ld-edge-'));
  const proc = spawn(findEdge(), ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--hide-scrollbars', '--mute-audio', `--user-data-dir=${profile}`, '--remote-debugging-port=0',
    'about:blank'], { stdio: 'ignore' });
  const portFile = path.join(profile, 'DevToolsActivePort');
  for (let i = 0; i < 200 && !fs.existsSync(portFile); i += 1) await sleep(50);
  if (!fs.existsSync(portFile)) { proc.kill(); throw new Error('Edge did not start (no DevToolsActivePort)'); }
  await sleep(50);
  const [port, wsPath] = fs.readFileSync(portFile, 'utf8').trim().split(/\r?\n/);
  const ws = new WebSocket(`ws://127.0.0.1:${port}${wsPath}`);
  await new Promise((ok, ko) => { ws.onopen = ok; ws.onerror = () => ko(new Error('cannot reach Edge DevTools')); });
  let next = 0;
  const waiting = new Map();
  const listeners = new Set();
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && waiting.has(msg.id)) {
      const { ok, ko } = waiting.get(msg.id);
      waiting.delete(msg.id);
      if (msg.error) ko(new Error(msg.error.message));
      else ok(msg.result);
    } else {
      for (const l of listeners) l(msg);
    }
  };
  const send = (method, params = {}, sessionId = undefined) => new Promise((ok, ko) => {
    next += 1;
    waiting.set(next, { ok, ko });
    ws.send(JSON.stringify({ id: next, method, params, sessionId }));
  });

  // A tab of `width` x `height` CSS px. `problems` collects script errors and CSP refusals.
  async function page(width, height) {
    const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
    const s = (method, params) => send(method, params, sessionId);
    const problems = [];
    listeners.add((msg) => {
      if (msg.sessionId !== sessionId) return;
      if (msg.method === 'Runtime.exceptionThrown') {
        const d = msg.params.exceptionDetails;
        problems.push((d.exception && d.exception.description) || d.text);
      } else if (msg.method === 'Log.entryAdded' && msg.params.entry.level === 'error' && msg.params.entry.source !== 'network') {
        problems.push(msg.params.entry.text);
      } else if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') {
        problems.push(msg.params.args.map((a) => a.value || a.description).join(' '));
      }
    });
    await s('Page.enable');
    await s('Runtime.enable');
    await s('Log.enable');
    await s('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
    await s('Emulation.setFocusEmulationEnabled', { enabled: true });  // the page stays focused and visible
    const evaluate = async (expression) => {
      const r = await s('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) throw new Error(`${expression}: ${r.exceptionDetails.text} ${(r.exceptionDetails.exception || {}).description || ''}`);
      return r.result.value;
    };
    return {
      problems,
      evaluate,
      goto: (url) => s('Page.navigate', { url }),
      type: (text) => s('Input.insertText', { text }),  // as typed at the keyboard, in the focused field
      mouse: (type, x, y) => s('Input.dispatchMouseEvent', { type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1 }),
      async waitFor(expression, ms = 8000) {
        const end = Date.now() + ms;
        for (;;) {
          try { if (await evaluate(expression)) return; } catch { /* the page is still loading */ }
          if (Date.now() > end) throw new Error(`timed out waiting for: ${expression}`);
          await sleep(50);
        }
      },
      async screenshot(file) {
        const { data } = await s('Page.captureScreenshot', { format: 'png' });
        fs.mkdirSync(path.dirname(file), { recursive: true });
        fs.writeFileSync(file, Buffer.from(data, 'base64'));
      },
      close: () => send('Target.closeTarget', { targetId }),
    };
  }

  async function close() {
    try { await send('Browser.close'); } catch { /* already gone */ }
    ws.close();
    for (let i = 0; i < 100 && proc.exitCode === null; i += 1) await sleep(50);
    if (proc.exitCode === null) proc.kill();
    for (let i = 0; i < 20; i += 1) {
      try { fs.rmSync(profile, { recursive: true, force: true }); break; } catch { await sleep(100); }
    }
  }
  return { page, close };
}

// True once the page has drawn its deck and its panel.
const READY = "typeof state === 'object' && !!state.doc && document.querySelectorAll('#keys button').length > 0 && !!document.querySelector('#panel h2')";

module.exports = { launch, findEdge, sleep, READY };
