# T-Watch Ultra: web API v1

The HTTP and WebSocket API the watch serves to its web desktop (`web-roadmap.md`). The mock watch `examples/factory/tools/web_mock_device.mjs` implements it on the PC; `examples/factory/tools/web_api_test.mjs` checks any server (mock or watch) against it.

## Conventions

- JSON in and out, UTF-8. File contents are raw bytes.
- Errors: HTTP status + `{"error":"<code>","message":"..."}`. Codes: `bad_request` 400, `unpaired` 401, `pairing_closed` 403, `not_found` 404, `conflict` 409, `too_large` 413, `locked` 429, `sd_missing` 503, `busy` 503 (a radio or tool is in use on the watch).
- **public** routes work before pairing; all others need the session cookie, else `401 unpaired`.
- A route listed for a later phase answers `404 not_found` until its capability is in `/api/caps`.

## Pairing

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/auth` | public | → `{paired, method:"pin"}` |
| `POST /api/auth/pair` | public | `{pin}` → `{ok:true}` + `Set-Cookie: session=<token>; Path=/; Max-Age=7776000; HttpOnly; SameSite=Strict`. Wrong PIN: `401 bad_pin`. 5 misses: `429 locked` with `retry_ms`, for 30 s. Pairing not open on the watch: `403 pairing_closed` |
| `POST /api/auth/logout` | paired | `{all?:bool}` → `{ok, revoked}`. `all` ends every session, including this one |

**Pairing window.** Pairing works only while the watch shows **Settings › Web desktop**: opening that page opens pairing with a **new** 6-digit PIN, leaving it closes pairing. Whoever pairs must be holding the watch. The PIN is never sent over HTTP (the watch also logs it to its serial port). Sessions are kept on the watch's flash (`/ffat/.web/state.txt`) and expire after 90 days unused; Settings › Web desktop › Forget browsers ends them all.

## System

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/caps` | public | → `{api:1, board:"twatch-ultra", fw:"<version>", caps:[...]}` |
| `GET /api/device` | public | → see below. Without pairing only `board`, `fw`, `battery`, `theme`, `sd.present` |
| `POST /api/time` | paired | `{epoch, offset_min?, tz?}` → `{ok}`. `epoch` is UTC seconds; `offset_min` the UTC offset the watch shows (it has no DST rules, so the desktop sends the current one); `tz` an IANA name kept for display. The desktop sends the browser's clock when the watch's is not synced |
| `GET /api/settings` | paired | → `{brightness, screen_timeout_s, web_idle_off_s}`. `brightness` 0–100 (mapped to the display's levels, so a value may come back ±1), `screen_timeout_s` 0–255, `web_idle_off_s` 0–86400 |
| `PATCH /api/settings` | paired | any subset of those fields → the full settings. Out of range: `400`. Publishes `settings.changed` |
| `POST /api/system/reboot` | paired | → `{ok}` |

`GET /api/device`:

```json
{ "board": "twatch-ultra", "fw": "1.4.0+web.1", "uptime_s": 812,
  "battery": { "percent": 76, "charging": false, "mv": 3968 },
  "sd": { "present": true, "mounted": true },
  "flash": { "total": 8257536, "free": 102400 },
  "net": { "mode": "sta", "ssid": "home", "ip": "192.168.1.50", "rssi": -58, "host": "watch.local" },
  "time": { "epoch": 1762369200, "offset_min": 60, "synced": true, "tz": "Europe/Warsaw" },
  "mem": { "heap_free": 182000, "psram_free": 6100000 },
  "theme": { "accent": "#00d4aa", "accent_dim": "#009977", "card": "#1a1a1a", "track": "#333333", "name": "Mint" } }
```

`theme` is the watch's colour preset; the desktop and the rescue page use its colours, and follow a change live through the `theme` event.

## Screen, input and display (P3)

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/screen?format=rle565\|rgb565` | paired | the screen as it is now (screen and top layer, from an LVGL snapshot). Headers `X-Width`, `X-Height`, `ETag` (a hash of the frame); `If-None-Match` with the same frame → `304`. `rle565` (default): runs of `[count u16 LE][colour u16 LE]` row by row, count 1–65535 (a watch face is about 20–45 KB instead of 400 KB). `rgb565`: raw little-endian pixels. No PNG: the browser makes images itself |
| `POST /api/input` | paired | `{type:"tap", x, y}`, `{type:"long", x, y}` (held 0.9 s), `{type:"swipe", x, y, x2, y2}` (about 0.2 s), or `{type:"button", name:"wake"\|"power_menu"}` → `{ok}`. Coordinates in screen pixels; outside the screen → `400`. Played through a second LVGL pointer device, so the watch UI treats it like a finger |
| `POST /api/display` | paired | `{on?, brightness?}` → `{ok}`. `on:false` turns the backlight off (the watch stays awake); `on:true` wakes the screen; `brightness` 0–100 changes it live without saving it |

## Desktop install (P3)

The watch keeps its desktop on the card up to date by itself (`web-roadmap.md`, "Getting the desktop onto the card"); these routes show and start it. Capability `desktop.install`.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/desktop` | paired | `{installed, wanted, zip_on_card, state, source, run, progress: {done, total}, error}`. `installed`: version in `/sd/web` or `null`; `wanted`: the version this firmware pins; `zip_on_card`: the newest `twatch-sd-*.zip` in the card's root or `null`; `state`: `idle`, `checking`, `downloading`, `unpacking` or `error`; `run` goes up by one each time a check starts |
| `POST /api/desktop/install` | paired | `{source: "auto" \| "card" \| "download"}` → `202 {ok}`, the work goes on in the background. `card`: the newest ZIP on the card; `download`: the pinned ZIP from the release; `auto`: what the watch does by itself. No card: `503 sd_missing` |

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

## Tools (P4)

Each tool has its capability; the desktop shows an app only when the watch lists it. Times (`ts`) are UTC epoch seconds from the watch clock.

**The watch UI wins.** A tool used by the app open on the watch (the LoRa app holds the radio, the Recorder the microphone, the NFC app the reader) answers `503 busy` with `"app": "<name>"`. If the watch opens such an app while the desktop uses the tool, the desktop's use stops and its state event says so.

**Polled sensors turn themselves off.** The IMU stream and the GNSS reading run while the desktop asks for them: the first `GET` starts them, and they stop 10 s after the last one.

### Monitor (`monitor`)

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/monitor` | paired | see below |
| `GET /api/logs?after=<n>` | paired | `{lines: [{n, ms, text}], next}`: the last 200 lines the watch logged (web server and tools), only those with `n > after`. `ms` is the watch's uptime; `next` is the `after` for the next call |

```json
{ "uptime_s": 812, "cpu_mhz": 240, "app": "GNSS",
  "battery": { "percent": 76, "charging": false, "mv": 3968, "ma": -85, "temp_c": 27.5, "vbus_mv": 0 },
  "mem": { "heap_free": 182000, "heap_min": 150000, "heap_total": 320000, "psram_free": 6100000, "psram_total": 8388608 } }
```

`app` is the app open on the watch, `""` on the face or the app list. A value the watch cannot measure is `null`.

### Notifications (`notify`)

| Route | Auth | Body → answer |
|---|---|---|
| `POST /api/notify` | paired | `{id?, title, body?, level?, vibrate?}` → `{ok, id}`. `title` 1–64 bytes, `body` up to 512, `level` `info` (default), `warn` or `alert`, `vibrate` (default true). Shows a pop-up on the watch and wakes its screen; the same `id` (up to 32 bytes) replaces the earlier one. Publishes `notify` |
| `GET /api/notify` | paired | `{items: [{id, title, body, level, ts}]}`, newest first, the last 20 since the watch started |

### Sensors (`sensors.imu`)

| Route | Auth | Answer |
|---|---|---|
| `GET /api/sensors` | paired | `{imu: {roll, pitch, heading, accel: {x, y, z}}, steps, seq}`. Angles in degrees, `accel` in m/s² (`null` until the first sample), `steps` today's count or `null`, `seq` goes up with each new sample. Poll it (up to 10 times a second) |

### GNSS (`gnss`)

| Route | Auth | Answer |
|---|---|---|
| `GET /api/gnss` | paired | see below |

```json
{ "model": "MIA-M10Q", "fix": true, "lat": 52.2297, "lon": 21.0122, "alt": 112.4, "speed_kmh": 0.4,
  "sats": 9, "time": 1762369200, "ttff_ms": 31000, "antenna": "ok",
  "satellites": [ { "sys": "GPS", "prn": 12, "el": 54, "az": 120, "cn0": 38, "used": true } ] }
```

Without a fix `lat`, `lon`, `alt` and `speed_kmh` are `null`. `time` is the GNSS time or `null`; `antenna` is `ok`, `open`, `short` or `unknown`; `sys` is `GPS`, `GLONASS`, `BeiDou`, `Galileo` or `QZSS`. While the desktop polls it, `gnss.fix` comes once a second. Recorded tracks are GPX files in `/sd/tracks/` (the watch's Track Log app), read through the file API.

### LoRa (`lora`)

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/lora/config` | paired | `{freq, bw, sf, cr, power, sync_word, listen}` |
| `PATCH /api/lora/config` | paired | any subset → the full config, applied to the radio at once. `freq` 150–960 MHz, `bw` one of 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500 kHz, `sf` 5–12, `cr` 5–8 (4/5–4/8), `power` −9–22 dBm, `sync_word` 0–255, `listen` receive between sends. Out of range: `400`. Kept on the watch's flash |
| `POST /api/lora/send` | paired | `{text}` or `{data_b64}`, 1–255 bytes → `{ok, len}`. Queued and sent in order (up to 8 wait while one is on air; more: `409`); back to receiving afterwards when `listen` is on |
| `GET /api/lora/log` | paired | `{packets: [{n, dir, ts, rssi, snr, len, data_b64}]}`: the last 50 packets sent (`dir: "tx"`, `rssi` and `snr` `null`) and received (`"rx"`), oldest first |

Every route answers `503 busy` while the watch's LoRa, Walkie, Sub-G or LoRaWAN app is open. A received packet is published as `lora.rx`.

### NFC (`nfc`)

| Route | Auth | Body → answer |
|---|---|---|
| `POST /api/nfc/scan` | paired | `{seconds?}` 1–60, default 20 → `{ok}`. Turns the reader on; it turns off after that time or `POST /api/nfc/stop`. Publishes `nfc.state` |
| `POST /api/nfc/stop` | paired | → `{ok}` |
| `GET /api/nfc` | paired | `{scanning, last}`. `last` is the last tag read, or `null`: `{uid, type, ts, ndef: [{kind, value}]}`, `uid` hex with colons, `type` the card type the reader reports, `kind` `text`, `uri`, `wifi` (value: the SSID) or `other` |

A tag read while scanning is published as `nfc.tag`. `503 busy` while the watch's NFC, NFC Card or PN532 app is open.

### Recorder (`recorder`) and player (`audio.play`)

| Route | Auth | Body → answer |
|---|---|---|
| `POST /api/recorder/start` | paired | `{path?, max_s?}` → `{ok, path}`. Records the watch microphone as 16 kHz 16-bit mono WAV straight to the file (default `/sd/data/recordings/rec-YYYYMMDD-HHMMSS.wav`; a given `path` must end in `.wav`), for up to `max_s` (1–600, default 60) seconds. Already recording: `409`. No card: `503 sd_missing` |
| `POST /api/recorder/stop` | paired | → `{ok, path, duration_ms, size}`. Not recording: `409` |
| `GET /api/recorder` | paired | `{state: "idle"\|"recording", path, elapsed_ms, max_s, level}`, `level` 0–100 |
| `POST /api/audio/play` | paired | `{path}` → `{ok}`: a WAV or MP3 on `/sd` or `/flash`, played on the watch speaker. Missing: `404`; another format: `400` |
| `POST /api/audio/stop` | paired | → `{ok}` |
| `GET /api/audio` | paired | `{playing, path, volume}`, `volume` 0–100 |
| `PATCH /api/audio` | paired | `{volume}` → as `GET` |

The finished recording publishes `files.changed`. `503 busy` while the watch's Recorder, Player, Net Radio or Walkie app is open.

### Scanners (`wifi.scan`, `ble.scan`)

A scan runs in the background (the watch's server answers one request at a time, so it never waits for the radio): `POST` starts it, the result comes as an event and stays readable with `GET`.

| Route | Auth | Body → answer |
|---|---|---|
| `POST /api/wifi/scan` | paired | → `202 {ok}`. A scan takes about 3–6 s; the watch stays on its network. A scan already running is joined |
| `GET /api/wifi/scan` | paired | `{scanning, ts, networks: [{ssid, bssid, rssi, channel, auth}]}`: the last result, strongest first, `ts` `null` before the first scan. `auth`: `open`, `wep`, `wpa`, `wpa2`, `wpa/wpa2`, `wpa2-ent`, `wpa3`, `wpa2/wpa3` or `other` |
| `POST /api/ble/scan` | paired | `{seconds?}` 1–15, default 5 → `202 {ok}`. `503 busy` while the watch's BLE Scan or BLE HID app is open |
| `GET /api/ble/scan` | paired | `{scanning, ts, devices: [{addr, name, rssi, connectable, mfr, services}]}`, strongest first. `name` and `mfr` (manufacturer data, hex) may be `""`; `services` is a list of UUIDs |

When a scan ends, `wifi.scan` or `ble.scan` carries the same object as the `GET`.

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
| `theme` | as `device.theme`, when the watch's colour preset changes | P3 |
| `desktop` | as `GET /api/desktop`, on each state change and about 3 times a second while it downloads or unpacks | P3 |
| `notify` | `{id, title, body, level, ts}`, each notification shown on the watch | P4 |
| `lora.rx` | a received packet, as in `GET /api/lora/log` | P4 |
| `gnss.fix` | `{fix, lat, lon, alt, speed_kmh, sats, time}`, once a second while the desktop watches the GNSS | P4 |
| `nfc.tag` | a tag, as `GET /api/nfc` `last` | P4 |
| `nfc.state` | `{scanning}`, when the reader starts or stops | P4 |
| `recorder` | as `GET /api/recorder`, on start and stop and 4 times a second while recording | P4 |
| `audio` | as `GET /api/audio`, on change | P4 |
| `wifi.scan`, `ble.scan` | as `GET /api/wifi/scan` and `GET /api/ble/scan`, when a scan ends | P4 |

## Static files

- `/rescue`: the rescue page built into the firmware (pairing, SD status, desktop install). Always available.
- `/` and `/<asset>`: `/sd/web/<asset>` if `/sd/web/manifest.json` exists and its `api` is `1`; otherwise `/` is the rescue page and other paths are `404`.
- `/apps/<id>/<rest>`: `/sd/web/apps/<id>/<rest>`.
- A `<file>.gz` twin is sent with `Content-Encoding: gzip`. `ETag` + `304`. HTML adds `X-Frame-Options: SAMEORIGIN`.

## Capabilities

| Cap | Routes | Phase |
|---|---|---|
| `files`, `sd`, `settings`, `time` | as above | P1 |
| `files.flash` | `/flash/...` paths | P1 |
| `screen` | `GET /api/screen` | P3 |
| `input` | `POST /api/input` | P3 |
| `display` | `POST /api/display` | P3 |
| `monitor` | `GET /api/monitor`, `GET /api/logs` | P4 |
| `notify` | `POST /api/notify`, `GET /api/notify`, events `notify` | P4 |
| `sensors.imu` | `GET /api/sensors` | P4 |
| `gnss` | `GET /api/gnss`, events `gnss.fix` | P4 |
| `lora` | `GET/PATCH /api/lora/config`, `POST /api/lora/send`, `GET /api/lora/log`, events `lora.rx` | P4 |
| `nfc` | `POST /api/nfc/scan\|stop`, `GET /api/nfc`, events `nfc.tag`, `nfc.state` | P4 |
| `recorder` | `POST /api/recorder/start\|stop`, `GET /api/recorder`, events `recorder` | P4 |
| `audio.play` | `POST /api/audio/play\|stop`, `GET/PATCH /api/audio`, events `audio` | P4 |
| `wifi.scan` | `POST/GET /api/wifi/scan`, events `wifi.scan` | P4 |
| `ble.scan` | `POST/GET /api/ble/scan`, events `ble.scan` | P4 |
| `ir` | infrared send and learn: reserved, the T-Watch Ultra has no IR hardware | — |
| `wifi.manage` | saved Wi-Fi networks: later | — |
| `desktop.install` | `GET /api/desktop`, `POST /api/desktop/install` | P3 |
| `link.espnow`, `usb.hid`, `ota`, `wifi.attack` | defined in P5 | P5 |

## Test hooks (simulator and mock only)

Compiled only without `ARDUINO`; the watch never has them.

| Route | What it does |
|---|---|
| `GET /api/_dev/pin` | opens pairing like the Web page does, and returns the PIN |
| `POST /api/_dev/pairing` | `{open}`: open or close pairing |
| `POST /api/_dev/sd` | `{present}`: insert or remove the simulated card (F3 in the simulator window does the same) |
| `GET /api/_dev/input` | the last `/api/input` body |
| `POST /api/_dev/theme` | `{preset}` 0–7: switch the watch's colour preset |
| `POST /api/_dev/app` | `{name}`: act as if that app were open on the watch (`""` for none), for the `busy` answers |
| `POST /api/_dev/lora` | `{text}` or `{data_b64}`, `rssi?`, `snr?`: a packet arrives (when `listen` is on) |
| `POST /api/_dev/nfc` | `{uid?, type?, text?}`: a tag is held to the reader (while it scans) |

The simulator and the mock also fake the hardware: a GNSS fix walking slowly around one spot, a slowly turning IMU, Wi-Fi and BLE devices, a LoRa station that answers each packet with `ack: <text>`, a recording of a tone, and playback that lasts as long as the file.

The mock can also act as the release server for the simulator (`--dl <dir>`): it serves `<dir>/<file>` at `/dl/<file>` with `Range`, and with `--dev` adds `POST /api/_dev/dl {cut_after?, status?}` (cut the next download off, or answer a status) and `GET /api/_dev/dl-log`. The simulator downloads only when `TWATCH_SIM_DESKTOP_URL` is set (plain HTTP).
