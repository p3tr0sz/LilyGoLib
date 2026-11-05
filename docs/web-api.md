# T-Watch Ultra: web API v1

The HTTP and WebSocket API the watch serves to its web desktop (`web-roadmap.md`). The mock watch `examples/factory/tools/web_mock_device.mjs` implements it on the PC; `examples/factory/tools/web_api_test.mjs` checks any server (mock or watch) against it.

## Conventions

- JSON in and out, UTF-8. File contents are raw bytes.
- Errors: HTTP status + `{"error":"<code>","message":"..."}`. Codes: `bad_request` 400, `unpaired` 401, `not_found` 404, `conflict` 409, `too_large` 413, `locked` 429, `sd_missing` 503, `busy` 503 (a radio or tool is in use on the watch).
- **public** routes work before pairing; all others need the session cookie, else `401 unpaired`.
- A route listed for a later phase answers `404 not_found` until its capability is in `/api/caps`.

## Pairing

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/auth` | public | → `{paired, method:"pin"}` |
| `POST /api/auth/pair` | public | `{pin}` → `{ok:true}` + `Set-Cookie: session=<token>; Path=/; Max-Age=7776000; HttpOnly; SameSite=Strict`. Wrong PIN: `401 bad_pin`. 5 misses: `429 locked` with `retry_ms`, for 30 s |
| `POST /api/auth/logout` | paired | `{all?:bool}` → `{ok, revoked}`. `all` ends every session, including this one |

The PIN (6 digits) is shown on the watch while its Web page is open and is never sent over HTTP. Sessions survive reboots (NVS), expire after 90 days unused.

## System

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/caps` | public | → `{api:1, board:"twatch-ultra", fw:"<version>", caps:[...]}` |
| `GET /api/device` | public | → see below. Without pairing only `board`, `fw`, `battery`, `sd.present` |
| `POST /api/time` | paired | `{epoch, tz}` (`tz` = IANA name or POSIX string) → `{ok}`. The desktop sends the browser's clock when the watch's is not synced |
| `GET /api/settings` | paired | → `{brightness, screen_timeout_s, lang, tz, web_idle_off_s}` |
| `PATCH /api/settings` | paired | any subset of those fields → the full settings. Publishes `settings.changed` |
| `POST /api/system/reboot` | paired | → `{ok}` |

`GET /api/device`:

```json
{ "board": "twatch-ultra", "fw": "1.4.0+web.1", "uptime_s": 812,
  "battery": { "percent": 76, "charging": false, "mv": 3968 },
  "sd": { "present": true, "mounted": true },
  "flash": { "total": 8257536, "free": 102400 },
  "net": { "mode": "sta", "ssid": "home", "ip": "192.168.1.50", "rssi": -58, "host": "watch.local" },
  "time": { "epoch": 1762369200, "tz": "Europe/Warsaw", "synced": true },
  "mem": { "heap_free": 182000, "psram_free": 6100000 } }
```

## SD card

| Route | Auth | Answer |
|---|---|---|
| `GET /api/sd` | paired | `{present, mounted, fs:"FAT32", card:"SDHC", total, free, error}`. `error` is `null` or a code such as `not_fat32`, `mount_failed`, `io_error` |
| `POST /api/sd/unmount` | paired | `{ok}`, safe to remove. A later insert mounts again |
| `POST /api/sd/mount` | paired | `{ok}` or `503 sd_missing` |

Insert, removal, mount and unmount publish `sd.changed` with the same object as `GET /api/sd`.

## Files

Paths are absolute and start with a root: `/sd/...` (SD card) or `/flash/...` (internal FFat). `..` is refused with `400`. A path on a missing card answers `503 sd_missing`.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/files?path=` | paired | → `{path, entries:[{name, dir, size, mtime}]}`, folders first then by name. Missing: `404` |
| `GET /api/files/content?path=` | paired | → the bytes, `Content-Type` from the extension, `Range` supported (`206`). Missing: `404` |
| `PUT /api/files/content?path=` | paired | body = bytes → `{ok, size}`. Creates missing parent folders. Written to `<path>.tmp` then renamed, so a cut never leaves half a file. Publishes `files.changed` |
| `PUT /api/files/content?path=&offset=N` | paired | appends at byte N for resumable uploads: only if the file is exactly N bytes, else `409 conflict` with `size`. Not atomic |
| `POST /api/files/mkdir` | paired | `{path}` → `{ok}`; existing folder is fine |
| `POST /api/files/move` | paired | `{from, to, overwrite?}` → `{ok}`. Existing `to` without `overwrite`: `409` |
| `DELETE /api/files?path=` | paired | → `{ok}`. A non-empty folder needs `&recursive=1`, else `409` |

## Events

`GET /api/events` upgrades to a WebSocket (paired only).

- Every message from the watch: `{"t":"<topic>","seq":<n>,"d":{...}}`, `seq` +1 per message.
- First message: `{"t":"hello","seq":0,"d":{"api":1,"caps":[...]}}`.
- **One desktop at a time:** a new connection sends `{"t":"seat.lost"}` to the previous one and closes it.
- Messages from the browser are ignored in v1.

| Topic | `d` | Phase |
|---|---|---|
| `battery` | `{percent, charging, mv}`, on change | P1 |
| `sd.changed` | as `GET /api/sd` | P1 |
| `files.changed` | `{op:"write"\|"mkdir"\|"move"\|"delete", path, to?}` | P1 |
| `settings.changed` | the full settings | P1 |
| `net` | `{mode, ssid, ip, rssi}` | P1 |
| `notify` | `{id, title, body, level}` shown on the watch | P4 |
| `lora.rx` | `{rssi, snr, len, data_b64}` | P4 |
| `gnss.fix` | `{lat, lon, alt, sats, hdop, fix}` | P4 |
| `ir.rx` | `{protocol, address, command, raw_len}` | P4 |
| `nfc.tag` | `{uid, type}` | P4 |

## Static files

- `/` and `/<asset>`: `/sd/web/<asset>` if `/sd/web/manifest.json` exists and its `api` is `1`; otherwise the embedded rescue desktop.
- `/apps/<id>/<rest>`: `/sd/web/apps/<id>/<rest>`.
- A `<file>.gz` twin is sent with `Content-Encoding: gzip`. `ETag` + `304`. HTML adds `X-Frame-Options: SAMEORIGIN`.

## Capabilities

| Cap | Routes | Phase |
|---|---|---|
| `files`, `sd`, `settings`, `time` | as above | P1 |
| `files.flash` | `/flash/...` paths | P1 |
| `screen` | `GET /api/screen` (PNG; `?format=rgb565` raw) | P3 |
| `input` | `POST /api/input` `{type:"tap"\|"swipe"\|"long"\|"button", x, y, x2?, y2?, name?}` | P3 |
| `display` | `POST /api/display` `{on?, brightness?}` | P3 |
| `notify` | `POST /api/notify` `{id?, title, body, level, vibrate?}`; same `id` replaces | P4 |
| `sensors.imu` | `GET /api/sensors` | P4 |
| `gnss` | `GET /api/gnss`, events `gnss.fix` | P4 |
| `lora` | `GET/PATCH /api/lora/config`, `POST /api/lora/send`, events `lora.rx` | P4 |
| `ir` | `POST /api/ir/send`, `POST /api/ir/learn`, events `ir.rx` | P4 |
| `nfc` | `POST /api/nfc/scan`, events `nfc.tag` | P4 |
| `recorder`, `audio.play` | `POST /api/recorder/start\|stop`, `POST /api/audio/play` `{path}` | P4 |
| `wifi.scan`, `wifi.manage` | `GET /api/wifi/scan`, `GET/POST/DELETE /api/wifi/networks` | P4 |
| `ble.scan` | `GET /api/ble/scan` | P4 |
| `link.espnow`, `usb.hid`, `ota`, `wifi.attack` | defined in P5 | P5 |
