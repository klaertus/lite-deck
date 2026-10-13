'use strict';
// A fake lite-deck for the page: serves src/web/assets as web.rc lists them, with the daemon's CSP,
// and a fake API from tests/web/fixtures (modules, device, config, fonts, previews, event stream).
// Development tool only, never shipped. By hand: node tests/web/mock-server.js [--port 8123]
// [--scenario name], then open the printed address. LD_WEB_ASSETS serves another assets folder.
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');

const ROOT = path.join(__dirname, '..', '..');
const ASSETS = process.env.LD_WEB_ASSETS || path.join(ROOT, 'src', 'web', 'assets');
const FIXTURES = path.join(__dirname, 'fixtures');
const TYPES = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };

// The CSP of the real page, read from src/web/api.cpp so that both never drift apart.
function daemonCsp() {
  const src = fs.readFileSync(path.join(ROOT, 'src', 'web', 'api.cpp'), 'utf8');
  const m = /kCsp\s*=\s*((?:"[^"]*"\s*)+);/.exec(src);
  if (!m) throw new Error('kCsp not found in src/web/api.cpp');
  return [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]).join('');
}

// '/' and '/<file>' for each RCDATA line of web.rc, like src/web/assets.cpp.
function assetFiles() {
  const rc = fs.readFileSync(path.join(ASSETS, 'web.rc'), 'utf8');
  const out = new Map();
  for (const m of rc.matchAll(/^\s*\w+\s+RCDATA\s+"([^"]+)"/gm)) out.set(m[1] === 'index.html' ? '/' : `/${m[1]}`, m[1]);
  return out;
}

const fixture = (name) => JSON.parse(fs.readFileSync(path.join(FIXTURES, name), 'utf8'));

// Real key images for the previews: the reference images of tests/golden (the key engine's output on
// fixed data), by module; the strip from fixtures/strip.png (written by the "doc: deck image" test).
const GOLDEN = { cpu_load: 'cpu_load_hot', ram: 'ram_value', stopwatch: 'stopwatch_running', clock: 'clock_seconds',
  date: 'date_wednesday', weather: 'weather_sunny', crypto_gold: 'crypto_prices', launcher: 'launcher_glyph',
  hotkey: 'hotkey_title', disk: 'disk_space', net_speed: 'net_speed', ping: 'ping', http_sensor: 'http_sensor' };

function previewFile(slot, strip) {
  if (strip) return path.join(FIXTURES, 'strip.png');
  if (!slot || !slot.module) return '';
  const mode = slot.settings && slot.settings.mode;
  const name = slot.module === 'gpu' ? `gpu_${mode === 'temp' || mode === 'usage' ? mode : 'vram'}` : GOLDEN[slot.module];
  return name ? path.join(ROOT, 'tests', 'golden', `${name}.png`) : '';
}

// Named situations for screenshots and tests.
const SCENARIOS = {
  default: {},
  empty: {
    config(doc) {
      doc.keys = doc.keys.map(() => null);
      doc.knobs = doc.knobs.map(() => null);
      doc.strip = [{ width: 800, module: null }];
      return doc;
    },
  },
  error: {  // every save is refused (422)
    reject: () => [{ path: 'keys[2].settings', message: 'warn: value too large (max 100)' },
      { path: 'strip', message: 'the widths must add up to 800 (currently 790)' }],
  },
  readme: {  // the README picture: the default layout with a launcher and a hotkey
    config(doc) {
      doc.keys[8] = { module: 'launcher', settings: {} };
      doc.keys[9] = { module: 'hotkey', settings: {} };
      return doc;
    },
  },
  'file-error': { fileErrors: [{ path: 'keys[0].module', message: 'unknown module: cpu' }] },
  offline: { putStatus: 503 },
};

function startMock(options = {}) {
  const scenario = SCENARIOS[options.scenario || 'default'];
  if (!scenario) throw new Error(`unknown scenario ${options.scenario}`);
  const files = assetFiles();
  const csp = daemonCsp();
  const modules = fixture('modules.json');
  const device = fixture('device.json');
  const fonts = ['Segoe UI', 'Bahnschrift', 'Consolas', ...Array.from({ length: 257 }, (_, i) => `Font ${String(i + 1).padStart(3, '0')}`)];
  const mock = {
    doc: scenario.config ? scenario.config(fixture('config.json')) : fixture('config.json'),
    revision: 1,
    puts: [],          // every PUT body, parsed
    requests: [],      // every request path, in order
    frames: new Map(), // preview target -> generation
    streams: new Set(),
    reject: scenario.reject || null,  // a test may lift the refusal: mock.reject = null
  };
  const send = (res, status, type, body, headers = {}) => {
    res.writeHead(status, Object.assign({ 'Content-Type': type, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' }, headers));
    res.end(body);
  };
  const json = (res, status, data, headers) => send(res, status, 'application/json; charset=utf-8', JSON.stringify(data), headers);
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    const p = url.pathname;
    mock.requests.push(`${req.method} ${p}`);
    if (req.method === 'GET' && files.has(p)) {
      const file = files.get(p);
      const headers = file.endsWith('.html') ? { 'Content-Security-Policy': csp } : {};
      send(res, 200, TYPES[path.extname(file)], fs.readFileSync(path.join(ASSETS, file)), headers);
      return;
    }
    if (p === '/favicon.ico') { send(res, 204, 'text/plain', ''); return; }
    if (p === '/api/modules') { json(res, 200, modules); return; }
    if (p === '/api/device') { json(res, 200, device); return; }
    if (p === '/api/fonts') { json(res, 200, fonts); return; }
    if (p === '/api/status') {
      json(res, 200, { device: 'connected', revision: mock.revision, config_errors: scenario.fileErrors || [] });
      return;
    }
    if (p === '/api/config' && req.method === 'GET') { json(res, 200, mock.doc, { ETag: `"${mock.revision}"` }); return; }
    if (p === '/api/config' && req.method === 'PUT') {
      let body = '';
      req.on('data', (c) => { body += c; });
      req.on('end', () => {
        const doc = JSON.parse(body);
        mock.puts.push(doc);
        if (scenario.putStatus) { json(res, scenario.putStatus, { errors: [{ path: '', message: 'unavailable' }] }); return; }
        const errors = mock.reject ? mock.reject(doc) : [];
        if (errors.length) { json(res, 422, { errors }); return; }
        const match = req.headers['if-match'];
        if (match && match !== '*' && Number(match.replace(/\D/g, '')) !== mock.revision) {
          json(res, 412, { errors: [{ path: '', message: 'the configuration was changed elsewhere' }] }, { ETag: `"${mock.revision}"` });
          return;
        }
        mock.revision += 1;
        mock.doc = doc;
        json(res, 200, { revision: mock.revision, config: doc }, { ETag: `"${mock.revision}"` });
      });
      return;
    }
    const pv = /^\/api\/preview\/(key\/\d+|strip\/0)$/.exec(p);
    if (pv) {
      const gen = mock.frames.get(pv[1]) || 1;
      const tag = `"g${gen}"`;
      if (req.headers['if-none-match'] === tag) { send(res, 304, 'image/svg+xml', '', { ETag: tag }); return; }
      const strip = pv[1] === 'strip/0';
      const slot = strip ? null : mock.doc.keys[Number(pv[1].slice(4))];
      const file = previewFile(slot, strip);
      if (file && fs.existsSync(file)) { send(res, 200, 'image/png', fs.readFileSync(file), { ETag: tag }); return; }
      const text = strip ? 'strip' : slot && slot.module ? slot.module : '';
      const [w, h] = strip ? [800, 112] : [112, 112];
      send(res, 200, 'image/svg+xml', `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}"><rect width="100%" height="100%" fill="#000"/>`
        + `<text x="50%" y="55%" fill="#8c8c8c" font-family="Segoe UI" font-size="16" text-anchor="middle">${text}</text></svg>`, { ETag: tag });
      return;
    }
    if (p === '/api/events') {
      if (mock.full) { json(res, 503, { errors: [{ path: '', message: 'too many pages open' }] }); return; }  // a third tab
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' });
      res.write('retry: 5000\n\n');
      const ping = setInterval(() => res.write(': ping\n\n'), 5000);
      mock.streams.add(res);
      req.on('close', () => { clearInterval(ping); mock.streams.delete(res); });
      return;
    }
    json(res, 404, { errors: [{ path: '', message: 'not found' }] });
  });
  // Pushes an event to the open pages, as the daemon's EventHub does.
  mock.publish = (event, data) => { for (const s of mock.streams) s.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`); };
  return new Promise((resolve) => {
    server.listen(options.port || 0, '127.0.0.1', () => {
      mock.port = server.address().port;
      mock.url = `http://127.0.0.1:${mock.port}/`;
      mock.close = () => new Promise((done) => {
        for (const s of mock.streams) s.destroy();
        server.close(done);
        server.closeAllConnections();  // the browser keeps its connections alive
      });
      resolve(mock);
    });
  });
}

module.exports = { startMock, SCENARIOS };

if (require.main === module) {
  const arg = (name) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : undefined; };
  startMock({ port: Number(arg('--port') || 8123), scenario: arg('--scenario') }).then((m) => console.log(`mock lite-deck on ${m.url}`));
}
