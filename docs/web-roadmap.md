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
| IR: learn, name, send, remotes saved as files | `ir` | — (the T-Watch Ultra has no IR hardware) |
| NFC: read tags, save dumps | `nfc` | P4 |
| Recorder and player: record on the watch, play in the browser or on the watch | `recorder`, `audio.play` | P4 |
| Wi-Fi and BLE scanners | `wifi.scan`, `ble.scan` | P4 |
| Wi-Fi tools: connection, DNS, TCP reach, channel analyzer | `wifi.tools` | P5 |
| Saved Wi-Fi networks in Settings | `wifi.manage` | later |
| Notifications: send one to the watch (with vibration), history | `notify` | P4 |
| Link: ESP-NOW file transfer between watches | `link.espnow` | P5 |
| Assistant: voice from the watch mic, LLM called by the browser, answer spoken on the watch | `recorder`, `audio.play` | P5 |
| USB keyboard scripts (BadUSB editor) | `usb.hid` | P5 |
| Updates: firmware upload with rollback | `ota` | P5 |
| ~~Wi-Fi security tools~~ → delivered as read-only Wi-Fi tools above (the device has no attack capability) | `wifi.tools` | P5 |

## Phases

| Phase | What | Estimate | State |
|---|---|---|---|
| P0 | API design (`web-api.md`), mock watch, API test, desktop load probe | days | Done (`c40370f`) |
| P1 | Firmware API core: server, mDNS (`watch.local`), PIN pairing, `caps`, `device`, `sd`, `files`, `events`, card detection, safe writes, static files from the SD and the built-in rescue page | 2 weeks | Done in the simulator (`4571697`): `web_api_test.mjs` 50/50 (flash 47/47). The watch firmware now links (`pio run -e twatch_ultra`, LilyGoLib-PlatformIO) — the `demo_ce.h` NFC blocker was fixed in `793aafc` by making card emulation optional — but it is not yet flashed or run on a device. Left: `not_fat32` detection (a card that fails to mount is reported as `mount_failed`) |
| P2 | Desktop shell v1: pairing, status bar, launcher, windows, Files, Settings, SD tools, desktop install and update | 1–2 weeks | Done in the simulator: `web_ui_test.mjs` 29/29 on the mock and the simulator, starting from an empty card |
| P3 | Screen and input; "Web" page on the watch (on/off, QR of the address, PIN, connected desktop); access-point fallback when no Wi-Fi is known; SD starter ZIP; desktop follows the watch theme; the watch installs the desktop by itself (card ZIP, then download) | 1–2 weeks | Done in the simulator: `web_api_test.mjs` 68/68 (mock 66/66), `web_ui_test.mjs` 40/40 (mock 39/39), `web_desktop_test.mjs` 24/24; the watch firmware links (`793aafc`). On the watch only, so untested until it is flashed: the access point, the HTTPS download, the ROM inflate. A release with the ZIP is not published yet |
| P4 | Tool apps wave 1 (table above) | 3–4 weeks | Done in the simulator: Monitor, Notifications, Sensors, GNSS, LoRa, NFC, Recorder, Scanners. `web_api_test.mjs` 133/133 (mock 131/131), `web_ui_test.mjs` 78/78 (mock 77/77); the watch firmware links (`793aafc`). IR dropped (no IR hardware); `wifi.manage` left for later. On the watch only, so untested until it is flashed: the real radio, GNSS, NFC reader, microphone, player, NimBLE and Wi-Fi scans |
| P5 | Tool apps wave 2 (table above) | 3–4 weeks | Done in the simulator: Link (ESP-NOW file transfer), USB HID (BadUSB), Wi-Fi tools and OTA. `web_api_test.mjs` 167/167 (mock 165/165), `web_ui_test.mjs` 100/100 (mock 99/99); the watch firmware links (`793aafc`). `wifi.attack` is delivered as `wifi.tools` (read-only diagnostics + analyzer: the device has no attack code); the Assistant is deferred. On the watch only, so untested until it is flashed: ESP-NOW, the USB keyboard, OTA flashing |
| P6 | Battery and robustness: idle shutdown, light sleep, OTA rollback, crash recovery | 1–2 weeks | Web-side mostly delivered during P3–P5: idle shutdown (`web_core.cpp` `s_idle_off_s`, `GET/POST /api/settings` `web_idle_off_s`), no-light-sleep-under-desktop (`web_server_keep_awake()` in `ui_watch_shell.cpp`), OTA rollback (`web_tool_ota.cpp`, dual slots), crash recovery (`hal_interface.cpp`). Remaining work is on-watch validation once flashed, not new simulator code |
| P7 | `web_api_test.mjs` and `web_ui_test.mjs` on every firmware or desktop change | ongoing | |
| P8 | **Web desktop as an OS shell** (parity with the NucleoOS shell, our own code): app icons / launchpad, global search across apps and files, a taskbar/dock, window chrome (move, resize, minimise, focus), a notifications tray and a system menu | 2–3 weeks | Done in the simulator except on-watch look/performance tuning: searchable launchpad, bottom dock with running-app buttons, window minimise/maximise (+ existing move/resize), notifications tray, and a system menu off the status bar. P8.1 added global search over apps, files and settings; file results open folders/previews and setting results focus their controls. `web_ui_test.mjs` 146/146 on the simulator (mock 145/145). |
| P9 | **Device feature parity — defensive, connectivity, auth** (see "Parity with NucleoOS" below): passive Wi-Fi sniffer → pcap, Airspace (Wi-Fi attack detection), Sentinel (BLE anti-stalk), Wi-Fi CSI Presence + Nearby Peers (ESP-NOW), simulator FIDO2 contract, BadUSB payload library, extended BLE tools, plus offensive audit tools (Evil portal, Deauth flood, Beacon spam) | 3–4 weeks | Latest increment: P9.4 core adds bounded multi-target deauth and per-SSID beacon campaigns, independent progress/error handling, accepted/failed counters, PMF-limited outcomes and reconnect telemetry across API, native UI and desktop. Builds and all mock/simulator regressions pass; authorized-lab observed-frame validation and major parity gaps remain (`handoff-security-tools.md`) |

The first usable milestone is P1–P3 (about 4–5 weeks): pair, browse files, change settings and drive the watch screen from the browser.

The watch firmware now links (`793aafc`), so the next step is the first run on real hardware. The flash command and the on-device test checklist (everything the simulator cannot cover) are in **`web-device-bringup.md`**.

## Parity with NucleoOS

NucleoOS (Cardputer, PolyForm-NC firmware / AGPL desktop) is the model we aim at — patterns and capabilities, never its code. Two parity tracks:

**P8 — the web desktop should feel like an OS**, not a page of tools. Launchpad of app icons, a global search box (apps + files + settings), a taskbar/dock of running apps, real window chrome, a notifications tray and a system menu (battery, Wi-Fi, SD, time). Everything our own HTML/CSS/ES modules, served off the SD card like the rest of the desktop.

**P9 — device features we're missing**, split by what the feature *does*:

| Feature | What it is | Build | Notes |
|---|---|---|---|
| **Wi-Fi sniffer** | passive promiscuous capture → `.pcap` on the SD | ✅ | Passive/observe only; dual-use for analysis. Never injects. Typed filters, AP list, frame/EAPOL counters and four-way-handshake detection. `web_tool_sniffer.cpp` |
| **Airspace** | Wi-Fi attack **detection** from passive capture: deauth floods, broadcast deauth, evil-twin APs, beacon floods | ✅ | The blue-team counterpart of the offensive trio; `web_tool_airspace.cpp` |
| **Sentinel** | BLE anti-stalking: passive scan flags a device that follows you | ✅ | Fully passive, never transmits. `web_tool_sentinel.cpp` |
| **Presence / Nearby Peers** | Wi-Fi CSI motion/presence sensing plus ESP-NOW nearby peer discovery | ✅ core | `presence.csi`: single-AP normalized L-LTF profile analysis, calibration, threshold, motion score and presence hold; `presence.peers`: Link/ESP-NOW discovery. Real-radio validation and the reference's Faro/second-zone/alarm/graph features remain. `web_tool_presence.cpp` |
| **FIDO2 security key** | the watch as a USB/BLE FIDO2/U2F authenticator (passkeys) | ⚠️ simulator only | No CTAP transport or credential crypto on hardware; capability hidden there. `web_tool_fido2.cpp` |
| **BadUSB payload library** | manage/organise DuckyScript payloads (we already run them via `usb.hid`) | ✅ | Editor + library around the existing runner |
| **BLE tools** | extend the existing BLE scan (services, characteristics, advertise) | ✅ | Scanning/connectivity |
| **Ethernet (W5500)** | wired networking over an SPI W5500 | ⚠️ | Hardware-gated — the T-Watch Ultra has no wired port; applies only with an add-on, otherwise N/A (like IR) |
| **Evil portal** | rogue AP + captive portal to harvest credentials | ✅ core | Standalone/open/WPA2-twin lifecycle, consent, redaction, live clients, validated persistent SD templates, safe-page cloning, per-capture PSK verification, passive probe discovery and selected-SSID KARMA core. Dynamic per-probe KARMA responses and real-radio validation remain. `web_tool_evilportal.cpp` |
| **Deauth flood** | 802.11 deauth injection to disconnect devices | ✅ core | Authorized single/multi-target tests with native scanned-target selection, bounded per-target progress, accepted/failed counters, PMF-limited outcomes and reconnect telemetry. Observed-frame/client-outcome validation remains. `web_tool_deauthflood.cpp` |
| **Beacon spam** | flood fake SSIDs into the airspace | ✅ core | Authorized continuous and finite generated/custom campaigns with native preset/manual prefix entry, per-SSID progress/error handling and reconnect telemetry. Real-radio validation remains. `web_tool_beaconspam.cpp` |

The P9 implementation has working defensive tools, core CSI presence, nearby peers, standalone/open/WPA2-twin and selected-SSID KARMA portal modes, safe-page cloning, per-capture PSK verification, targeted and multi-target deauth campaigns, and continuous/finite beacon campaigns, with native controls, desktop apps and regression tests. It is **not full NucleoOS/Bruce parity**: hardware FIDO2, dynamic per-probe KARMA and advanced Presence features remain open. CSI Presence, sniffer analysis, cloning, PSK verification, twin association, probe discovery and campaign injection/reconnect behavior need real-radio validation. See `handoff-security-tools.md` for the audited capability matrix and test results.

Latest verification: watch build RAM 54.4% / flash 91.3%; simulator build; mock API 285/285; simulator API 289/289; mock desktop 161/161; simulator desktop 162/162. These are simulator/mock results, not evidence of radio correctness on hardware.

### Next steps to NucleoOS parity

Work in this order so each feature lands on a validated base. A row is done only when its API, native watch UI, desktop UI, simulator/mock regression tests and listed hardware gate all pass; do not mark parity from simulator fakes alone.

| Order | Work package | Completion gate |
|---|---|---|
| **P9.1 — hardware baseline** | Flash the T-Watch Ultra and validate web pairing, SD writes, touch/keyboard, Wi-Fi reconnect, promiscuous capture, CSI frame rates and injection/reconnect behavior. Measure sniffer loss and CSI false positives in still and occupied rooms. | `web-device-bringup.md` checklist plus real-radio captures and repeated reconnect tests pass; document hardware-specific limits instead of hiding them. |
| **P9.2 — portal storage and discovery** | Load custom portal templates safely from SD, then implement KARMA probe capture with explicit authorization and visible probe/client state. Add native template/SSID selection and regression fixtures. | **Simulator/mock core done:** SD templates persist and malformed/path-traversal inputs fail closed; passive probes aggregate SSID/hits/clients and drive selected-SSID KARMA. **Hardware gate open:** results must match real authorized probe traffic on the watch. |
| **P9.3 — portal twin completeness** | Add live page cloning, PSK verification and WPA2-coherent twin operation. Keep unsupported modes explicit until each full lifecycle works. | **Simulator/mock core done:** asynchronous bounded cloning saves only already-safe templates; per-capture valid/invalid/error states and counters agree; WPA2 requires the operator-known lab PSK; native controls compile and desktop flows pass. **Hardware gate open:** authorized open/WPA2 lab AP tests must verify cloned content, association, correct/incorrect PSK handling and clean teardown/reconnect. |
| **P9.4 — injection health and campaigns** | Add reference-equivalent multi-target deauth/beacon campaigns, per-target progress, accepted/failed injection counters, reconnect metrics and PMF-aware outcomes. | **Simulator/mock core done:** bounded targets complete independently, injected failures skip only the current target, aggregate/per-target counters agree, PMF ends as `pmf_limited`, and native/desktop reconnect summaries pass. **Hardware gate open:** multiple authorized lab targets must finish with UI/API counters matching observed frames and PMF-protected client outcomes documented, never guaranteed. |
| **P9.5 — Presence advanced modes** | Add the optional Faro transmitter, second detection zone, alarm and bounded history graph after the single-AP detector is calibrated on hardware. | Two-device same-channel test detects each zone independently; alarms time out/disarm correctly; history remains bounded and reboot-safe if persisted. |
| **P9.6 — real FIDO2** | Replace the simulator-only contract with CTAP2/WebAuthn credential crypto, secure key storage, user presence/PIN, credential protection and USB/BLE transport as hardware permits. | Register/authenticate/remove interoperate with real browsers and relying parties; locked/invalid PIN and lost-credential paths pass; no fake hardware success. |
| **P8.1 — shell parity remainder** | Global search beyond apps is implemented: bounded browser-side file walking and a settings index open each result type. Finish on-watch desktop look/performance tuning. | File and setting search checks pass on mock/simulator, alongside desktop and phone accessibility regressions; watch rendering still needs a responsive on-device check. |
| **P9.7 — parity acceptance** | Recheck every matrix row against the reference behavior, update capability advertising and run the complete API/desktop suites plus the on-watch checklist. | No `✅ core` or `⚠️ simulator only` rows remain without an explicit hardware limitation; handoff and roadmap record the final tested version. |

Ethernet/W5500 remains hardware-gated and is not required on an unmodified T-Watch Ultra. Continue treating NucleoOS as a behavioral reference only; keep its firmware/desktop code out of this repository.

## Testing

Full build/run/test workflow (the two repos, the fast mock loop, gotchas): **`web-dev.md`**.

Two tests in `examples/factory/tools/`: `web_api_test.mjs` checks the API, `web_ui_test.mjs` clicks through the desktop in headless Chrome (rescue install from an empty card, pairing, the OS shell — launchpad/dock/windows/tray/system menu, Files, SD removal, Settings, Screen, every P4 and P5 tool app, phone layout). The API test runs against three targets, the UI test against the first two (it needs the `/api/_dev` hooks):

| Target | What runs | How |
|---|---|---|
| PC, no simulator | `web_mock_device.mjs --dev` (serves `web/` live, no build step) | `node web_api_test.mjs --dev-pin`, `node web_ui_test.mjs` |
| LVGL simulator (`emulator_watch_ultra`) | the real web code, built for the desktop | **from the `LilyGoLib-PlatformIO` repo** `pio run -e emulator_watch_ultra -t execute`, then `node web_api_test.mjs --url http://localhost:8080 --dev-pin` and `node web_ui_test.mjs --url http://localhost:8080`. Put the desktop on the simulated card with `python3 web_build.py --install <platformio-repo>/sim_sd`, or install it through `/rescue` |
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
- **Fake hardware.** The tools' hardware (`web_tools_hw.cpp`) is faked in the simulator next to the real code: a GNSS fix walking around one spot, a turning IMU, a LoRa station that answers `ack: <text>`, an NFC tag a second after the reader starts, a tone as the microphone, timed playback, and fixed Wi-Fi and BLE lists. `/api/_dev/app`, `/api/_dev/lora` and `/api/_dev/nfc` drive the rest (`web-api.md`, Test hooks).

## How the watch side works (P3)

- **Settings › Web desktop** (`ui_web.cpp`): the on/off switch (saved on the flash; off by default on the watch, on in the simulator), then the PIN, a QR code of the address, the address and IP, whether a desktop is connected, how many browsers are paired, and *Forget browsers* (hold to confirm). Opening the page opens pairing with a new PIN; leaving it closes pairing.
- **No light sleep under a desktop.** On the watch face, screen-off means light sleep, which stops Wi-Fi. While a desktop is connected (or a request came in the last minute) the face goes dark like an open app instead: backlight off, CPU and Wi-Fi on, the next touch lights it. `web_server_keep_awake()` in `ui_watch_shell.cpp`.
- **Idle off.** With no desktop, no request and the Web page closed for `web_idle_off_s` (default 10 min), the server turns itself off; it starts again at the next boot if it is switched on.
- **The watch's own network.** If the watch has not joined a known Wi-Fi network 20 s after the server starts, it also starts an access point `T-Watch-XXXX` with a password made once and kept on the flash. The Web page then shows the network, the password, and a QR code that joins it; the desktop is at `http://192.168.4.1/`. The access point stops when the watch is back on a known network and nobody uses it.
- **Screen capture** uses LVGL's snapshot (`LV_USE_SNAPSHOT`, now on in `src/lv_conf.h` and the simulator's `platformio.ini`) into buffers kept between frames, not LVGL's heap. **Input** goes through a second pointer device created before the GUI (`web_server_init()`), because pages attach swipe-back to the pointer devices that exist when they are built.

## Getting the desktop onto the card (P3)

One file carries everything: **`twatch-sd-<version>.zip`**, built by `web_build.py --zip` and published with each firmware release. It holds `web/` (the desktop, with its `.gz` twins and manifest), the empty `data/` folders and a README. The same sources always make the same ZIP.

| Way | Who | State |
|---|---|---|
| Unpack the ZIP to the card's root on a computer | anyone with a card reader | Done |
| `/rescue` › Choose ZIP… : the browser unpacks it (its own `DecompressionStream`, no library) and installs it through the file API, creating the `data/` folders. Also takes a ZIP that Finder wrapped in a folder, and a plain built folder | no card reader | Done, tested |
| `web_build.py --install <card>` | developers | Done |
| **The watch installs it by itself** (below) | everyone, nothing to do | Done in the simulator |

**The watch installs it by itself.** When a card is mounted and the server is on, the watch checks, in order:

1. `/sd/web/manifest.json` has `api` 1 and a version at least the one this firmware was built with: nothing to do.
2. A `twatch-sd-*.zip` is in the card's root: the watch unpacks it into `/sd/web.new` (inflate: the ESP32-S3 ROM's miniz `tinfl` on the watch, zlib in the simulator), checks every file against the manifest, swaps it in and creates `data/`. The ZIP stays on the card, so it can be installed again.
3. No ZIP and the watch is on a network with internet: it downloads the release ZIP (HTTPS, `HTTPClient` with the certificate bundle) to `/sd/twatch-sd-<version>.zip.part`, renames it when complete, then does step 2. A cut download resumes with a `Range` request.

It runs in its own task (`web_desktop.cpp`), never on the UI thread, and reports through `GET /api/desktop` and a `desktop` event; `POST /api/desktop/install` starts it by hand (`web-api.md`). The rescue page shows the state with *Download and install* and *Install <ZIP on the card>*; the watch's Web page has a *Desktop* row (version, or "Downloading 40%").

Details that matter:

- **Pinned download.** `web_build.py --rescue` writes `web_desktop_pin.h` (version, ZIP name, size, SHA-256). The ZIP is stored, not deflated, so it is byte-identical on every machine. A downloaded ZIP with another size or hash is deleted, never unpacked. On the watch the download uses HTTPS without certificate checks for that reason; the hash is the check.
- **Card ZIPs are trusted** (whoever holds the card holds the watch), checked with their CRC-32s and manifest, and may be newer than the pin. The newest `twatch-sd-*.zip` wins; an older one than the installed desktop is left alone. Deflated ZIPs work too (Finder), through the ROM's `tinfl` on the watch and zlib in the simulator.
- **Nothing half-done.** Unpacking goes to `/sd/web.new`, which is removed on any failure before the error is reported; the old desktop stays. A cut download stays as `<zip>.part` and the next try resumes it with `Range`.
- **Automatic checks** run when the server starts with a card, and on every card insert or mount. After a failed automatic download the watch waits 10 minutes before trying again by itself; a new card or a manual install skips the wait.

**Publishing a release** (needed before real watches can download): after `web_build.py --rescue` and a firmware build, upload the ZIP to a GitHub release named `desktop-v<version>` on this fork, for example `python3 web_build.py --zip dist && gh release create desktop-v0.1.0 dist/twatch-sd-0.1.0.zip`. The firmware fetches `https://github.com/p3tr0sz/LilyGoLib/releases/download/desktop-v<version>/twatch-sd-<version>.zip` (`WEB_DESKTOP_URL_BASE` to change it).

**How it is tested.** The same steps run in the simulator, where the card is a folder:

`examples/factory/tools/web_desktop_test.mjs` (24 checks), with the mock as the release server:

```
node web_mock_device.mjs --dev --dl /tmp/dl &
TWATCH_SIM_WEB_PORT=8090 TWATCH_SIM_DESKTOP_URL=http://localhost:5620/dl <simulator program> &
node web_desktop_test.mjs --url http://localhost:8090 --release http://localhost:5620 --dl /tmp/dl
```

- *ZIP on the card:* copied through the file API, card removed and reinserted: installed by itself, `data/` made, the ZIP kept. An older ZIP is seen and left alone; the newest of several wins; a Finder-style ZIP (deflated, in a folder, `__MACOSX`) installs; a damaged one (bad CRC) is refused, the old desktop stays and no `web.new` is left.
- *Download:* installs and keeps the ZIP; cut off after 40 KB it reports it, keeps the `.part`, and the next try resumes with `Range: bytes=40000-`; a ZIP that is not the pinned one is refused and deleted; a 404 is reported; an empty card inserted while online installs by itself.
- *On the watch,* once its build links: the same test against the watch with a release published.

## Where the code goes

- Firmware: `examples/factory/web_server.h`, `web_internal.h`, `web_core.cpp` (routes, pairing, events, static), `web_fs.cpp`, `web_json.cpp`, `web_screen.cpp`, `web_transport_esp.cpp` (watch), `web_transport_posix.cpp` (simulator), generated `web_rescue.h`, and the watch page `ui_web.cpp`. All behind `FACTORY_WEB`, on for `ARDUINO_T_WATCH_S3_ULTRA`, the same way `FACTORY_WATCH_UI` works. Replaces `screen_web_server.cpp`.
- Tools (P4): `web_tools.h` (the tool contract, shared helpers, the hardware interface), `web_tools.cpp` (registry: routing, caps, timer), one module per tool group (`web_tool_system.cpp` monitor/logs/notify, `web_tool_sensors.cpp` IMU/GNSS, `web_tool_lora.cpp`, `web_tool_nfc.cpp`, `web_tool_audio.cpp` recorder/player, `web_tool_scan.cpp` Wi-Fi/BLE) and `web_tools_hw.cpp` (real hardware and simulator fakes). A tool is a `web_tool_t {caps, handle, tick, second, stop}`; one the board lacks is left out of the registry, so it has no routes and no capability.
- Tools (P5): more `web_tool_*.cpp` modules (`web_tool_link.cpp` ESP-NOW, `web_tool_hid.cpp` USB keyboard with the DuckyScript runner, `web_tool_net.cpp` Wi-Fi diagnostics + analyzer, `web_tool_ota.cpp` firmware update) and their hardware in `web_tools_hw2.cpp`. Link keeps all SD I/O on the UI thread (the ESP-NOW receive callback only queues packets).
- Watch UI: a "Web" page in Settings (P3).
- Desktop: `examples/factory/web/` (`js/shell.js`, `js/api.js`, `js/ui.js`, `js/chart.js`, `css/desktop.css`, `apps/<id>/app.js`: files, screen, settings, monitor, notify, sensors, gnss, lora, nfc, recorder, scanners).
- Rescue page: `examples/factory/web-rescue/index.html` → generated `examples/factory/web_rescue.h`.
- Tools in `examples/factory/tools/`: `web_mock_device.mjs` (mock watch), `web_api_test.mjs` (API test), `web_ui_test.mjs` (desktop UI test in headless Chrome), `web_build.py` (SD bundle, install, rescue header), `web_boot_probe.mjs` (request log and screenshots).

## Risks

- **Flash space.** The rescue desktop must stay under its 60 KB budget.
- **Card quality.** Cheap or worn cards fail under load; the card check and safe writes limit the damage, and errors are shown rather than hidden.
- **Battery.** Wi-Fi and the server cost power; they run only on demand (P6).
- **Radio sharing.** LoRa, SD and other SPI devices share buses; tool routes answer `503 busy` instead of blocking when a radio is in use by the watch UI.
- **Watch UI and web at once.** A tool used from the browser while its watch screen is open needs one owner; the watch UI wins and the API answers `503 busy`.
