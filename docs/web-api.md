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

## Tools wave 2 (P5)

The same rules as P4: one capability per tool, `503 busy` with `"app"` while the matching watch app is open. These reach hardware the simulator cannot fully stand in for (USB, ESP-NOW, a second watch, the flash), so the simulator fakes them (Test hooks) and they are first proven on a real watch.

### Link: ESP-NOW file transfer (`link.espnow`)

Send a file to another T-Watch over ESP-NOW, with no Wi-Fi network between them. Watches running this announce themselves; the desktop lists the ones in range and sends a file to one.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/link` | paired | `{enabled, mac, name, peers: [{mac, name, rssi, last_seen}], transfer}`. `transfer` is `null` or the one in flight (below) |
| `POST /api/link/enable` | paired | `{on}` → `{ok, enabled}`. Turns ESP-NOW and the 2 s "hello" announcement on or off. Off by default |
| `POST /api/link/send` | paired | `{path, to_mac}` → `202 {ok}`. Sends a file on `/sd` or `/flash` to a peer's MAC (`"ff:ff:ff:ff:ff:ff"` broadcasts to every watch in range). One transfer at a time: a second → `409`. A missing peer → `404`. No card → `503 sd_missing` |
| `POST /api/link/cancel` | paired | → `{ok}`. Stops the transfer in flight |

A transfer: `{dir: "tx"|"rx", peer, name, size, done, state: "sending"|"receiving"|"done"|"error", error}`. Received files are written to `/sd/data/link/<name>` (a clash gets ` (2)` and so on). Chunks are sent stop-and-wait with a CRC and retries; a lost peer ends the transfer with `error`. Events: `link` on every state, peer-list and progress change, and `files.changed` when a file arrives.

### USB HID: keyboard scripts (`usb.hid`)

The watch is a USB keyboard to the computer it is plugged into. It types text and runs DuckyScript — the same subset the watch's BadUSB app runs. For the computer it is plugged into, with permission: a convenience for setup and testing, not for use against machines that are not yours.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/hid` | paired | `{available, state: "idle"|"armed"|"running", script, line, lines}`. `available` is false until the USB cable is in and the host enumerates the keyboard |
| `POST /api/hid/type` | paired | `{text}` 1–1024 chars → `{ok}`. Types it once, now. `503 hid_unavailable` when no host |
| `POST /api/hid/run` | paired | `{path}` (a `.txt`/`.duck`/`.ducky` on `/sd`) or `{text}` (the script itself, up to 16 KB) → `202 {ok}`. Arms for 2 s, then runs. Already running → `409` |
| `POST /api/hid/stop` | paired | → `{ok}`. Stops the running script |

DuckyScript subset: `STRING`, `DELAY ms`, `DEFAULT_DELAY ms`, `ENTER`/`TAB`/`ESC`/`GUI`/arrows and the function keys, and modifier combos such as `CTRL ALT DEL` or `GUI r`. Scripts are ordinary files, edited through the file API; the desktop keeps them in `/sd/data/badusb/`. Events: `hid` on each state or line change. `503 busy` while the watch's BadUSB app is open.

### Wi-Fi tools (`wifi.tools`)

The watch's own network diagnostics and RF overview — the Network Tools and Wi-Fi Analyzer apps, from the browser. Read-only: it looks at the air and reaches out from the watch; it does not transmit management frames or attack anything.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/net` | paired | `{connected, ssid, ip, gateway, netmask, dns, rssi, channel, mac}` for the watch's own connection |
| `POST /api/net/resolve` | paired | `{host}` → `{ok, ip, ms}`; a failure is `{ok:false, ms}`. Needs Wi-Fi: `409 offline` |
| `POST /api/net/tcp` | paired | `{host, port}` (port 1–65535) → `{ok, ms}` or `{ok:false, ms}`: can a TCP connection be opened. `409 offline` |
| `GET /api/net/channels` | paired | the 2.4 GHz RF overview from a scan: `{best, channels: [{channel, count, score}], aps: [{ssid, bssid, channel, rssi}]}`, `best` the least-busy channel to use |

`503 busy` while the watch's Network Tools or Wi-Fi Analyzer app is open.

### Firmware update (`ota`)

Replace the firmware over the air, into the spare app slot, with a roll-back to the running one if the new build does not come up. **Irreversible-ish and able to brick the watch: the desktop confirms twice and checks the file before it starts.**

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/ota` | paired | `{slot, other_slot, state: "idle"|"writing"|"ready"|"error", progress:{done,total}, error, can_rollback}`. `slot` the running partition (`"app0"`/`"app1"`), `can_rollback` whether the other slot holds a known-good build |
| `POST /api/ota/upload` | paired | body = the firmware `.bin` (raw bytes, `Content-Length` required) → `{ok, size}`. Streams into the spare slot and verifies the image; a bad or too-large image → `400 bad_image`/`413 too_large` and the slot is left untouched. Publishes `ota` as it writes |
| `POST /api/ota/commit` | paired | → `{ok}`, then the watch reboots into the new slot after about a second. Only after a good upload, else `409` |
| `POST /api/ota/rollback` | paired | → `{ok}`, reboots into the other slot. `409` when there is nothing good to roll back to |

After a reboot into a new build, the watch marks it good once it reaches the UI; a build that crashes first is rolled back by the bootloader on the next power-up. Events: `ota` on each state and progress change.

### Assistant (`assistant`) — later

Voice to the watch, an answer spoken back: record on the watch (`recorder`), the browser does speech-to-text, the language model and text-to-speech, the reply plays on the watch (`audio.play`). It needs a model endpoint and a key, so it waits until the desktop has a settings place for those. No firmware routes of its own; it is built on the P4 recorder and player.

## Tools wave 3: P9 NucleoOS parity

### Wi-Fi sniffer (`wifi.sniffer`)

Passive promiscuous Wi-Fi capture, writing 802.11 frames to a `.pcap` file on the SD card. The resulting file can be opened in Wireshark or any pcap-aware tool. The watch never transmits while sniffing.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/sniffer` | paired | `{active, channel, filter, packets, drops, bytes, seen, management, control, data, beacons, probes, deauth, eapol, handshakes, aps: [{bssid, ssid, channel, rssi, beacons}], file, error}`. `channel` is `-1` (all), `1-13`, or `null`. `seen` counts observed frames before filtering; `packets` counts frames saved to the PCAP. `handshakes` counts complete WPA/WPA2 four-way handshakes inferred from EAPOL-Key messages |
| `POST /api/sniffer/start` | paired | `{channel?, filter?, max_bytes?, path?}` → `{ok}`. `channel` -1 (all, default), 1-13; `filter` `all` (default), `management`, `data`, `beacons`, `probes` or `eapol` filters what is written to the PCAP (frame counters and the AP list still observe every frame); `max_bytes` 0 (unlimited, default); `path` to the .pcap file, default `/sd/data/sniffer/capture-<timestamp>.pcap` |
| `POST /api/sniffer/stop` | paired | → `{ok}` |

When the capture fills `max_bytes` it stops automatically and publishes a `sniffer` event with `{active:false, reason:"max_bytes"}`. A path on a missing card answers `503 sd_missing`.

### Airspace (`airspace`)

Wi-Fi attack detection from passive capture. The blue-team counterpart of the offensive Wi-Fi tools — it listens for deauth floods, broadcast deauth, evil-twin APs and beacon floods, and publishes alerts when it detects them. Fully passive, never transmits.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/airspace` | paired | `{active, deauth_total, deauth_floods, beacon_total, beacon_floods, evil_twins, started}` |
| `POST /api/airspace/start` | paired | → `{ok}`. Starts passive monitoring |
| `POST /api/airspace/stop` | paired | → `{ok}` |

Alerts are published as `airspace` events: `{type:"deauth_flood"|"beacon_flood"|"evil_twin", detail, bssid, rssi}`. `503 busy` while the watch's Wi-Fi Sniffer app is open (they share the promiscuous mode).

### Sentinel (`sentinel`)

BLE anti-stalking: a passive scan that flags BLE devices appearing persistently across multiple scan cycles in different locations. Fully passive, never transmits.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/sentinel` | paired | `{active, started, device_count, devices: [{addr, name, rssi, first_seen, last_seen, sightings, stalking}]}` |
| `POST /api/sentinel/start` | paired | → `{ok}`. Starts monitoring |
| `POST /api/sentinel/stop` | paired | → `{ok}` |

A device flagged as stalking triggers a `sentinel` event: `{type:"stalking", addr, name, rssi, sightings}`. Devices older than 1 hour are automatically forgotten.

### Nearby peers (`presence.peers`)

ESP-NOW nearby device discovery — extends the existing Link/ESP-NOW tool with persistent peer tracking, arrival/departure events, and a history log. **This is not NucleoOS's Wi-Fi CSI human-motion Presence detector.**

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/presence` | paired | `{active, started, peers_ever, peers: [{mac, name, rssi, first_seen, last_seen, present}], history: [{ts, mac, name, event}]}` |
| `POST /api/presence/start` | paired | → `{ok}`. Starts peer discovery with ESP-NOW |
| `POST /api/presence/stop` | paired | → `{ok}` |

`event` is `"appeared"` or `"disappeared"`. Peers not seen for 5 minutes are marked as gone. A `presence` event is published on arrival and departure and includes `event`, `mac`, `name` and `rssi`.

### FIDO2 security key (`fido2`, simulator only)

**The current watch firmware does not include a CTAP/FIDO transport or credential crypto. It therefore does not advertise this capability and returns `503 fido_unavailable` for mutations. The routes below exist only for desktop development against the simulator/mock; they are not a hardware authenticator.**

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/fido2` | paired | `{available, state, credentials, error}` |
| `POST /api/fido2/register` | paired | `{rp, user}` → `{ok}`. Register a new credential |
| `POST /api/fido2/authenticate` | paired | `{rp}` → `{ok}`. Authenticate with an existing credential. `404 not_found` when no credential exists for this RP |
| `GET /api/fido2/credentials` | paired | `{credentials: [{index, rp, user}]}` |
| `POST /api/fido2/remove` | paired | `{index}` → `{ok}` |

`503 hid_unavailable` when the USB HID is not enumerated by a host.

### Evil portal (`evil.portal`)

Rogue AP + captive portal for credential harvesting. Security audit tool — only for testing networks you own or have explicit permission to test. Creates a rogue access point with a configurable SSID, serves a captive portal login form, and captures credentials entered by connecting devices.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/evilportal` | paired | `{active, ssid, clients, attempts, started, error}` |
| `POST /api/evilportal/start` | paired | `{ssid?, templateId?, gateway?, authorized:true}` → `{ok, ssid}`. Default SSID: "Free Wi-Fi". Explicit authorization is required |
| `POST /api/evilportal/stop` | paired | → `{ok}`. Stops the AP and DNS server |
| `GET /api/evilportal/captures` | paired | `{captures: [{mac, hostname, url, username, password, ts}]}` |
| `POST /api/evilportal/clear` | paired | → `{ok}`. Clears captured credentials |

Whilst the portal is active, all HTTP requests from connected clients are intercepted and served the captive portal. DNS resolves every domain to the watch's IP.

### Deauth flood (`deauth.flood`)

802.11 deauthentication frame injection to disconnect devices from a target network. Security audit tool — only for testing networks you own or have explicit permission to test. Injects deauth frames at a configurable rate against a specific BSSID and optional station.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/deauthflood` | paired | `{active, target_bssid, target_station, channel, sent, rate_kbps, started}` |
| `POST /api/deauthflood/start` | paired | `{bssid, station?, channel?, rate?, authorized:true}` → `{ok}`. `station` defaults to broadcast (`ff:ff:ff:ff:ff:ff`), `channel` 1-13, `rate` 1-100 pkts/s (default 10). Explicit authorization is required |
| `POST /api/deauthflood/stop` | paired | → `{ok}` |

Requires the Wi-Fi interface to disconnect from any network during the flood.

### Beacon spam (`beacon.spam`)

Fake SSID beacon flooding. Security audit tool — only for use in controlled environments you own. Injects fake 802.11 beacon frames with sequentially named SSIDs into the airspace.

| Route | Auth | Body → answer |
|---|---|---|
| `GET /api/beaconspam` | paired | `{active, channel, beacons, ssids, prefix, started}` |
| `POST /api/beaconspam/start` | paired | `{prefix?, count?, channel?, mode?, authorized:true}` → `{ok}`. `prefix` (default "FreeNet"), `count` of unique SSIDs (default 20), `channel` 0 (hop) or 1-13, `mode` `generated` or `custom`. Explicit authorization is required |
| `POST /api/beaconspam/stop` | paired | → `{ok}` |

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
| `link` | as `GET /api/link`, on peer, state and transfer changes | P5 |
| `hid` | as `GET /api/hid`, on state or line changes | P5 |
| `ota` | as `GET /api/ota`, while writing and on state changes | P5 |
| `sniffer` | `{active, reason?}`, capture state changes | P9 |
| `airspace` | `{type, detail, bssid, rssi}`, alerts for detected attacks | P9 |
| `sentinel` | `{type:"stalking", addr, name, rssi, sightings}`, a device flagged | P9 |
| `presence` | peer arrival/departure | P9 |

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
| `link.espnow` | `GET /api/link`, `POST /api/link/enable\|send\|cancel`, events `link` | P5 |
| `usb.hid` | `GET /api/hid`, `POST /api/hid/type\|run\|stop`, events `hid` | P5 |
| `wifi.tools` | `GET /api/net`, `POST /api/net/resolve\|tcp`, `GET /api/net/channels` | P5 |
| `ota` | `GET /api/ota`, `POST /api/ota/upload|commit|rollback`, events `ota` | P5 |
| `assistant` | no routes of its own: the P4 recorder and player | later |
| `wifi.sniffer` | `GET /api/sniffer`, `POST /api/sniffer/start|stop`, events `sniffer` | P9 |
| `airspace` | `GET /api/airspace`, `POST /api/airspace/start|stop`, events `airspace` | P9 |
| `sentinel` | `GET /api/sentinel`, `POST /api/sentinel/start|stop`, events `sentinel` | P9 |
| `presence.peers` | `GET /api/presence`, `POST /api/presence/start|stop`, events `presence` | P9 |
| `fido2` | simulator/mock only: `GET /api/fido2`, `POST /api/fido2/register|authenticate|remove`, `GET /api/fido2/credentials`; not advertised on current watch hardware | P9 |
| `evil.portal` | `GET /api/evilportal`, `POST /api/evilportal/start|stop|clear`, `GET /api/evilportal/captures` | P9 |
| `deauth.flood` | `GET /api/deauthflood`, `POST /api/deauthflood/start|stop` | P9 |
| `beacon.spam` | `GET /api/beaconspam`, `POST /api/beaconspam/start|stop` | P9 |

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
| `POST /api/_dev/link` | `{peer?}`: a fake watch appears in range (default one), or `{rx:{name,size}}`: a file is sent to this watch |
| `POST /api/_dev/hid` | `{available}`: pretend the USB keyboard is or is not enumerated by a host |
| `POST /api/_dev/fido2` | `{available, reset?}`: enable the fake transport or clear fake credentials |
| `POST /api/_dev/sniffer` | `{count?}` injects fake beacon frames while capturing; `{kind:"beacon"\|"probe"\|"deauth"\|"management"\|"data"\|"eapol", msg?:1-4, count?}` injects typed frames for the filter and EAPOL handshake checks |

The simulator and the mock also fake the hardware: a GNSS fix walking slowly around one spot, a slowly turning IMU, Wi-Fi and BLE devices, a LoRa station that answers each packet with `ack: <text>`, a recording of a tone, playback that lasts as long as the file, a fake watch in range for the link, a USB host for the keyboard, and an OTA write that progresses but never reboots.

The mock can also act as the release server for the simulator (`--dl <dir>`): it serves `<dir>/<file>` at `/dl/<file>` with `Range`, and with `--dev` adds `POST /api/_dev/dl {cut_after?, status?}` (cut the next download off, or answer a status) and `GET /api/_dev/dl-log`. The simulator downloads only when `TWATCH_SIM_DESKTOP_URL` is set (plain HTTP).
