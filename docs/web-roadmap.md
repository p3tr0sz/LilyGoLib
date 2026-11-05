# T-Watch Ultra: web desktop roadmap

Goal: open the watch's address in a browser and get a **desktop for the watch**: files, every hardware tool, the watch screen, settings and notifications, on a laptop or a phone. The watch keeps its own LVGL UI (see `watch-ui-roadmap.md`); the browser is the big-screen console for it.

Everything is ours: the API (`web-api.md`), the firmware side and the desktop. The desktop is built for what this watch can do, not a copy of another device's.

## Patterns we follow

- **The watch decides, the browser shows.** State lives on the watch; the desktop renders it and sends commands. Heavy work (maps, charts, parsing, AI calls, big file previews) runs in the browser, never on the watch.
- **Pair with a PIN shown on the watch.** Proof of being next to the watch. The browser gets an HttpOnly session cookie, so every later request and the event socket are authenticated without extra code.
- **Capabilities, not guesses.** `GET /api/caps` lists what this firmware can do. The desktop hides what is missing instead of probing routes and handling 404s.
- **One event stream.** One WebSocket carries everything the watch has to say: battery, files changed, LoRa packets, GNSS fixes, IR codes, notifications. Every message is `{t, seq, d}`.
- **One desktop at a time.** A new desktop takes the event stream; the old one is told and stops writing.
- **Files are the shared ground.** SD card and internal flash through one file API; tools save captures (IR codes, GNSS tracks, recordings, NFC dumps) as files the desktop can open.
- **Cheap to serve.** Static files gzipped, cached by ETag and a service worker. After the first visit the watch serves almost nothing but the API.
- **Host first, flash last.** A mock watch (`web_mock_device.mjs`) implements the same API on the PC. The desktop is built against it; the firmware is checked against the same test (`web_api_test.mjs`).
- **Fail safe on the watch.** The server and Wi-Fi run only when switched on, stop after an idle timeout, and never block the LVGL loop.

## Architecture

```
browser ── HTTP /api/*  ─┐
        ── WS /api/events ┤  esp_http_server (FACTORY_WEB)
        ── GET /, /apps/* ┘     ├─ web_auth     PIN, sessions (NVS)
                                ├─ web_fs       /sd, /flash
                                ├─ web_events   one socket, {t, seq, d}
                                ├─ web_tools    IR, LoRa, GNSS, NFC, sensors, recorder, Wi-Fi, BLE
                                ├─ static       desktop from /sd/web, else the embedded rescue page
                                └─ web_rescue.h rescue page built into the firmware (gzip)
```

- **The desktop lives on the SD card** (`/sd/web/`), gzipped. No firmware space limit, updatable without flashing, room for apps and their data.
- **A small rescue page is built into the firmware** (`web-rescue/index.html`, 3.4 KB gzipped, budget 60 KB): pairing, SD status, and installing or updating the desktop on the card from a folder the browser uploads. It is always at `/rescue`, and at `/` when there is no card, no desktop on it, or a desktop built for another API version. The released image uses about 3.06 MB of the 4 MB app slot, and the internal FFat is already full of audio, so the full desktop does not go in flash.
- **Desktop source:** `examples/factory/web/`. Plain HTML, CSS and ES modules, no framework and no build step while developing. `tools/web_build.py` gzips it into an SD bundle (`web/` + `web/manifest.json` with version, API level and file hashes) and `--rescue` regenerates `web_rescue.h`, the rescue page as a gzipped C array (commit it with the page).

## SD card support

The SD card is a first-class part of the system, not an optional extra:

- **Card detection.** The XL9555 card-detect line (`EXPANDS_SD_DET`) is watched. Insert mounts the card, removal unmounts it; both publish `sd.changed` so the desktop updates at once. Without the detect line, mount state is checked on each file request.
- **Clear status.** `GET /api/device` and `GET /api/sd` report present, mounted, FAT type, total, free, card type and the last error (for example "not FAT32"). The watch only supports FAT32 cards up to 32 GB; the desktop and the watch both say so when a card is refused.
- **Safe writes.** Every write goes to `<file>.tmp` and is renamed into place, so a removed card or a dead battery never leaves half a file. A write when the card is gone fails cleanly with `503 sd_missing`.
- **Shared bus.** The SD shares the SPI bus with the LoRa radio. File transfers use the bus lock that the factory code already uses for the radio, in chunks, so LoRa receive keeps working during a download.
- **Layout.** `/sd/web/` desktop, `/sd/web/apps/<id>/` extra apps, `/sd/data/` user files and tool captures (`ir/`, `gnss/`, `nfc/`, `recordings/`, `lora/`), `/sd/system/` desktop settings.
- **Desktop install and update.** The rescue page uploads the built folder to `/sd/web.new` through the file API, checks every file's size against `manifest.json`, then moves `/sd/web.new` onto `/sd/web`. A failed or interrupted upload leaves the old desktop in place. Settings › Web desktop shows the installed version and links to the rescue page. From a PC, `web_build.py --install <card>` does the same.
- **Card tools on the desktop.** SD status in the status bar, free space, "safe to remove" (unmount), and a check that scans the card and reports errors.
- **Apps are ES modules** loaded by the shell (same origin, no iframes): `web/apps/<id>/app.js` exporting `{ id, title, icon, caps, mount(el, api) }`. An app whose `caps` are not all present is not shown.
- **Responsive:** windows on a laptop, one full-screen app at a time on a phone.

## What the desktop has

| App | Uses | Phase |
|---|---|---|
| Pairing and status bar (battery, Wi-Fi, SD, time) | `auth`, `device`, events | P2 |
| Files: browse, upload, download, rename, delete, preview (text, images, audio, JSON) | `files` | P2 |
| Settings: brightness, screen timeout, time zone, time from the browser's clock, Wi-Fi networks | `settings`, `time`, `wifi.manage` | P2 |
| Screen: live watch screen, tap and swipe on it, screenshots | `screen`, `input` | P3 |
| Monitor: battery and memory graphs, uptime, logs | `device`, events | P4 |
| Sensors: IMU, steps, compass | `sensors.imu` | P4 |
| GNSS: position on a map, satellites, tracks from the SD | `gnss` | P4 |
| LoRa: messenger and packet log, radio settings | `lora` | P4 |
| IR: learn, name, send, remotes saved as files | `ir` | P4 |
| NFC: read tags, save dumps | `nfc` | P4 |
| Recorder and player: record on the watch, play in the browser or on the watch | `recorder`, `audio.play` | P4 |
| Wi-Fi and BLE scanners | `wifi.scan`, `ble.scan` | P4 |
| Notifications: send one to the watch (with vibration), history | `notify` | P4 |
| Link: ESP-NOW file transfer between watches | `link.espnow` | P5 |
| Assistant: voice from the watch mic, LLM called by the browser, answer spoken on the watch | `recorder`, `audio.play` | P5 |
| USB keyboard scripts (BadUSB editor) | `usb.hid` | P5 |
| Updates: firmware upload with rollback | `ota` | P5 |
| Wi-Fi security tools (authorized testing only) | `wifi.attack` | P5 |

## Phases

| Phase | What | Estimate | State |
|---|---|---|---|
| P0 | API design (`web-api.md`), mock watch, API test, desktop load probe | days | Done (`c40370f`) |
| P1 | Firmware API core: server, mDNS (`watch.local`), PIN pairing, `caps`, `device`, `sd`, `files`, `events`, card detection, safe writes, static files from the SD and the built-in rescue page | 2 weeks | Done in the simulator (`4571697`): `web_api_test.mjs` 50/50 (flash 47/47). The watch build compiles the web code but stops at the unrelated `demo_ce.h` NFC issue, so it is not yet run on the watch. Left: `not_fat32` detection (a card that fails to mount is reported as `mount_failed`) |
| P2 | Desktop shell v1: pairing, status bar, launcher, windows, Files, Settings, SD tools, desktop install and update | 1–2 weeks | Done in the simulator: `web_ui_test.mjs` 29/29 on the mock and the simulator, starting from an empty card |
| P3 | Screen and input; "Web" page on the watch (on/off, QR of the address, PIN, connected desktop); access-point fallback when no Wi-Fi is known (its name and password need the watch screen) | 1 week | |
| P4 | Tool apps wave 1 (table above) | 3–4 weeks | |
| P5 | Tool apps wave 2 (table above) | 3–4 weeks | |
| P6 | Battery and robustness: idle shutdown, light sleep, OTA rollback, crash recovery | 1–2 weeks | |
| P7 | `web_api_test.mjs` and `web_ui_test.mjs` on every firmware or desktop change | ongoing | |

The first usable milestone is P1–P3 (about 4–5 weeks): pair, browse files, change settings and drive the watch screen from the browser.

## Testing

Two tests in `examples/factory/tools/`: `web_api_test.mjs` checks the API, `web_ui_test.mjs` clicks through the desktop in headless Chrome (rescue install from an empty card, pairing, Files, SD removal, Settings, phone layout). The API test runs against three targets, the UI test against the first two (it needs the `/api/_dev` hooks):

| Target | What runs | How |
|---|---|---|
| PC, no simulator | `web_mock_device.mjs --dev` (serves `web/` live, no build step) | `node web_api_test.mjs --dev-pin`, `node web_ui_test.mjs` |
| LVGL simulator (`emulator_watch_ultra`) | the real web code, built for the desktop | `pio run -e emulator_watch_ultra -t execute`, then `node web_api_test.mjs --url http://localhost:8080 --dev-pin` and `node web_ui_test.mjs --url http://localhost:8080`. Put the desktop on the simulated card with `python3 web_build.py --install <dir>/sim_sd`, or install it through `/rescue` |
| Watch | the same code on `esp_http_server` | `node web_api_test.mjs --url http://<watch-ip> --pin <pin>` |

How the simulator runs the real web code:

- **One core, two transports.**
  - Shared files (`web_core.cpp`, `web_fs.cpp`, `web_json.cpp`) hold the routing, pairing, files, SD and events. They use no ESP calls and only POSIX file calls, which work on the watch's `/sd` and `/ffat` mounts as well as on the PC.
  - They talk to a thin transport (`web_internal.h`): read the request, send status, headers and body chunks, send a WebSocket message.
  - The watch uses `web_transport_esp.cpp` (`esp_http_server`).
  - The simulator uses `web_transport_posix.cpp`, a small HTTP/1.1 + WebSocket server on `localhost:8080`, written here with no extra library and limited to the same number of connections as the watch.
- **Storage on the PC.** `/sd` is a folder (`sim_sd/` in the working directory, or `TWATCH_SIM_SD`) and `/flash` another (`sim_flash/`, or `TWATCH_SIM_FLASH`). F3 in the simulator window inserts or removes the card (F2 is already the power button); the web server mounts and unmounts it and the browser gets `sd.changed`. The watch's own SD pages are not built for the simulator yet, so they don't see this card.
- **Same threading as the watch.** The server runs in its own task (watch) or thread (simulator). Anything that touches LVGL or the hardware goes through a queue that an `lv_timer` drains in the UI loop.
- **Test hooks only in the simulator.** `/api/_dev/pin` and `/api/_dev/sd` are compiled only without `ARDUINO`; the watch never has them. The PIN is also printed to the console.
- **Fake hardware.** GNSS, LoRa, IR and NFC answer with fixed fake data in the simulator, as the desktop build already does for audio and network radio.

## Where the code goes

- Firmware: `examples/factory/web_server.cpp/.h`, `web_auth.cpp`, `web_fs.cpp`, `web_events.cpp`, `web_static.cpp`, `web_tools.cpp`, generated `web_assets.h`. All behind `FACTORY_WEB`, on for `ARDUINO_T_WATCH_S3_ULTRA`, the same way `FACTORY_WATCH_UI` works. Replaces `screen_web_server.cpp`.
- Watch UI: a "Web" page in Settings (P3).
- Desktop: `examples/factory/web/` (`js/shell.js`, `js/api.js`, `js/ui.js`, `css/desktop.css`, `apps/<id>/app.js`).
- Rescue page: `examples/factory/web-rescue/index.html` → generated `examples/factory/web_rescue.h`.
- Tools in `examples/factory/tools/`: `web_mock_device.mjs` (mock watch), `web_api_test.mjs` (API test), `web_ui_test.mjs` (desktop UI test in headless Chrome), `web_build.py` (SD bundle, install, rescue header), `web_boot_probe.mjs` (request log and screenshots).

## Risks

- **Flash space.** The rescue desktop must stay under its 60 KB budget.
- **Card quality.** Cheap or worn cards fail under load; the card check and safe writes limit the damage, and errors are shown rather than hidden.
- **Battery.** Wi-Fi and the server cost power; they run only on demand (P6).
- **Radio sharing.** LoRa, SD and other SPI devices share buses; tool routes answer `503 busy` instead of blocking when a radio is in use by the watch UI.
- **Watch UI and web at once.** A tool used from the browser while its watch screen is open needs one owner; the watch UI wins and the API answers `503 busy`.
