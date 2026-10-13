# Web API

The configuration page talks to the daemon through a small HTTP API on `127.0.0.1`. It is made for
the bundled page, but you can call it from a script. The routes are implemented in
`src/web/api.cpp` and `src/web/server.cpp`.

If you only want to change the configuration from a script, the simplest way is to edit
`%APPDATA%\lite-deck\config.json` directly. The daemon reloads it 300 ms after each save, keeps the
previous configuration when the new one is invalid, and reports the errors in the tray menu and on
`GET /api/status`.

## Address

`http://127.0.0.1:<port>`, where the port is `web.port` in `config.json` (default `7878`). The server
listens on `127.0.0.1` only (IPv4). Each connection serves one request.

## Authentication

Every request must pass these checks:

1. **Host header.** It must be `127.0.0.1:<port>` or `localhost:<port>`. Anything else gets `403`.
   This check protects against DNS rebinding.
2. **Session cookie.** The daemon creates a random 128-bit token each time it starts. The tray icon
   opens `http://127.0.0.1:<port>/?t=<token>`, which answers `303` to `/` with
   `Set-Cookie: ld=<token>; HttpOnly; SameSite=Strict; Path=/`. Every other route needs that `ld`
   cookie. Without it, `/api/...` routes answer `401` (JSON) and pages answer `401` with a short
   HTML page. A wrong `t` gets `401` as well.
3. **Writes** (`PUT /api/config`) also need the header `X-Lite-Deck: 1` (otherwise `403`) and
   `Content-Type: application/json` (otherwise `415`). A cross-site page cannot set the custom header
   without a CORS preflight, and the server does not answer preflights.

The token is not written anywhere else. A script has to take it from the browser's `ld` cookie, and
it has to get it again after each daemon restart.

Every response carries `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer` and
`X-Frame-Options: DENY`. The API answers carry `Cache-Control: no-store`. The page's own files (`/`,
`/app.css`, the scripts) carry an `ETag` (a hash of the file) and `Cache-Control: no-cache`: the
browser asks again at each load and gets `304 Not Modified` while the file has not changed. HTML
responses also carry a strict `Content-Security-Policy` (`img-src 'self' blob:`: the previews are
fetched by the page, then shown from `blob:` addresses).

## Errors

Errors use a single JSON format:

```json
{ "errors": [ { "path": "keys[3].settings", "message": "warn: value too large (max 100)" } ] }
```

`path` is the place in the configuration document that the error refers to. It is `""` for errors
that are not about one place, such as `"session missing or expired"`.

## Routes

| Method | Path | Answer |
|---|---|---|
| `GET`/`HEAD` | `/?t=<token>` | `303` to `/` with the session cookie (see above). |
| `GET` | `/`, `/app.js`, `/app.css`, ... | The page and its assets, embedded in the executable. |
| `GET` | `/api/modules` | The module catalogue (see below). |
| `GET` | `/api/device` | The active device: grid, key size, strip, knobs and vendor app (see below). |
| `GET` | `/api/config` | The configuration document in force, with its revision as `ETag: "<n>"`. |
| `PUT` | `/api/config` | Replaces the configuration (see below). |
| `GET` | `/api/status` | `{"device": "connected" \| "busy" \| "disconnected" \| "simulator", "revision": n, "config_errors": [...]}`. `config_errors` lists the errors of the last configuration that was refused, for example after a bad hand edit of the file. |
| `GET` | `/api/fonts` | The installed font families, as a JSON array of strings. |
| `GET` | `/api/preview/key/<n>` | The last frame of key `n` (0 to rows × cols − 1, left to right then top to bottom), as JPEG at the device's quality (85), with an `ETag` (see below). `If-None-Match` with that `ETag` gets `304` while the frame is the same. |
| `GET` | `/api/preview/strip/0` | The last frame of the whole strip, as JPEG (quality 80), with an `ETag` as above. `404` on a device without a strip. |
| `GET` | `/api/events` | Server-sent events (see below). |

Other methods on `/api/config`, `/api/modules`, `/api/device`, `/api/status` and `/api/fonts` get `405`, and unknown
paths get `404`. If the daemon's event loop does not answer in time, the answer is `503` (`preview unavailable` for a
preview with no frame to give: ask again at the next `frame` event).

### `ETag` and `If-None-Match`

- **The page's files**: the `ETag` is a hash of the file's bytes (FNV-1a, 64 bits, in hex), so a new
  build of lite-deck gives a new one.
- **Previews**: the `ETag` is `"<epoch>-<generation>"`. The generation of a key or of the strip goes
  up with each new frame; the epoch goes up each time the previews are freed (when the last page
  closes, or 60 seconds after the last preview asked for while no page is open), so an old `ETag`
  never matches a newer frame. Each frame is encoded once, whatever the number of pages.
- **`If-None-Match`** takes one tag, a comma-separated list of tags, or `*`. A `W/` prefix is ignored
  (weak comparison). If one of them is the current `ETag`, the answer is `304 Not Modified` with no
  body.
- `GET /api/config` and `PUT /api/config` use the configuration's revision as `ETag` (`"<n>"`), for
  `If-Match` (see below).

### `GET /api/device`

The device lite-deck drives, chosen at startup (the one plugged in, else `device.model` in
`config.json`, else the MiraBox N4 Pro E). It does not change until the daemon restarts.

```json
{ "id": "mirabox-n4pro", "name": "MiraBox N4 Pro E", "rows": 2, "cols": 5,
  "key_size": { "width": 112, "height": 112 }, "strip": { "width": 800, "height": 112 },
  "knobs": 4, "knob_rings": true, "vendor_app": "StreamDock" }
```

`strip` is `null` on a device without a touch strip. `vendor_app` names the vendor software that takes
the device while it runs (the `busy` state), or is `null` when there is none. The configuration document always has at least
`rows × cols` keys, `knobs` knobs and as many ring colours; it may have more (slots kept from a larger
device, not shown). `PUT /api/config` refuses a module placed beyond the device (`422`, with the path,
for example `keys[10]`).

### `GET /api/modules`

The answer is an array with one object per registered module. This example is shortened to one
schema field and one element:

```json
{
  "id": "ram",
  "name": "Memory (RAM)",
  "description": "Memory used, in GB of the total.",
  "category": "System",
  "hue": "#5aa9ff",
  "slots": ["key"],
  "schema": { "fields": [ { "key": "warn", "type": "number", "default": 85, "min": 0, "max": 100,
                            "label": "Warning at (% used)", "group": "thresholds" } ] },
  "elements": [ { "key": "value", "label": "Value", "size": 36, "weight": 700, "min": 10, "max": 80,
                  "icon": false, "kind": "text", "row": "middle", "align": "center",
                  "color_mode": "state", "color": "#5aa9ff", "visible": true, "gradient": false,
                  "role": "value" } ],
  "two_line": false,
  "ring_states": []
}
```

- `description`, `category` and `hue` are always present (`""` when the module has none). `category`
  is one of `System`, `Time`, `Weather & sky`, `Media & sound`, `Actions` or `Online data`; `hue`
  (`#rrggbb` or `""`) is the page's accent while the module is selected.
- `slots` can contain `key`, `knob` and `zone`.
- `schema.fields[].type` is one of `int`, `number`, `bool`, `string`, `secret` (a string to mask),
  `color` (`#rrggbb`) or `enum` (with `options`, the list of values `config.json` takes). A field may
  add a `hint` (a help text), a `placeholder` (an example value), `option_labels` (the words the page
  shows for each option, in the same order), a `group` (fields shown together) and `show_if` (when
  the page shows the field). `option_labels` and `show_if` are described below.
- `options` is always the list of the values a setting may hold (what `config.json` and `PUT` use).
  When the module gives them readable names, `option_labels` follows, with one label per value in the
  same order: `"options": ["none", "task_manager", "open"]`, `"option_labels": ["Nothing", "Open Task
  Manager", "Open a link, file or program"]`. A field without labels has no `option_labels`.
- `show_if` (optional) tells when a field applies: `{"field": "kind", "equals": "program"}` or
  `{"field": "direction", "in": ["above", "below"]}`, where `field` is another key of the same schema.
  A condition may add `"and": {another condition}`, and `show_if` may be a list of conditions, any
  one of which is enough. Conditions are not chained: only the named field's value counts. The page
  hides the field otherwise; it is a display hint only: the daemon still checks the value, keeps it
  in the configuration and returns it.
- `role` is present on key texts only.
- `ring_states` (knobs) lists `{key, label, color, brightness}`, where `color` and `brightness` name
  the settings that hold that state's ring colour and brightness.

### `PUT /api/config`

The body is a whole configuration document, in the same format as `config.json` (version 1). The
maximum size is 64 KB and the maximum nesting depth is 64. Unknown keys (keys lite-deck does not
know at the top level and in `device`, `web`, `appearance` and `appearance.states`) are kept and written back to the file, up to 4 KB of them (their
JSON text). More is refused with `422`, path `""`, message `unknown keys take <n> bytes (at most
4096): remove some`, unless it is no more than the unknown keys already kept (a hand-written
`config.json` with more than 4 KB of them can still be saved from the page). Nothing is applied and `config.json` is not changed. To fix it, remove the keys
you added from the document you send (or from `config.json`, then reload the page).

- `If-Match: "<revision>"` (optional; `W/"n"`, `n` and `*` are accepted too) makes the write
  conditional. If the configuration changed since that revision, the answer is `412` with the current
  `ETag`. A header that cannot be parsed also gets `412`.
- `200`: the configuration was applied, or it did not change. The body is
  `{"revision": n, "config": {...}}` and the response has an `ETag`. The returned document is the
  normalised one: module settings are completed with their defaults, and unknown settings are dropped.
- `422`: the configuration is invalid. Nothing is applied, and `errors` lists every problem with its
  path.
- `400`: invalid JSON, invalid UTF-8, or JSON nested too deeply.
- `413`: the body is larger than 64 KB (`configuration too large`). The server may also close the
  connection before reading such a body. The file is not changed.
- `500`: the file could not be written. `errors` says why.

Example (PowerShell, with the cookie value copied from the browser):

```powershell
$s = New-Object Microsoft.PowerShell.Commands.WebRequestSession
$s.Cookies.Add((New-Object System.Net.Cookie("ld", "<token>", "/", "127.0.0.1")))
$r = Invoke-WebRequest -UseBasicParsing -WebSession $s http://127.0.0.1:7878/api/config
$cfg = $r.Content | ConvertFrom-Json
$cfg.device.brightness = 40
Invoke-RestMethod -Method Put -WebSession $s -Uri http://127.0.0.1:7878/api/config `
  -Headers @{ "X-Lite-Deck" = "1"; "If-Match" = $r.Headers["ETag"] } `
  -ContentType "application/json" -Body ($cfg | ConvertTo-Json -Depth 20)
```

### `GET /api/events`

This route needs the same Host and cookie checks as the others. The answer is a `text/event-stream`
that starts with `retry: 5000` and then sends these events. A `: ping` comment line, every 5 seconds
while nothing happens, keeps the connection open and frees the server thread of a page that closed.

| Event | Data |
|---|---|
| `config` | `{"revision": n, "errors": [...]}` after any configuration change or refused reload. |
| `device` | `{"state": "connected" \| "busy" \| "disconnected"}` |
| `frame` | `{"keys": [...], "strip": bool}`: which previews have a new frame. Several frames are combined into one event. |

At most two streams can be open at a time. A third gets `503`.
