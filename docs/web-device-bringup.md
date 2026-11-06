# T-Watch Ultra: on-device bring-up and test checklist

First hardware run of the web desktop stack. Everything below has passed in the simulator
(`web-roadmap.md`); this is the list of what the simulator **cannot** cover and must be checked on a
real watch. The firmware links as of `793aafc` (the `demo_ce.h` NFC blocker fix).

Context you may not have in front of you:

- The firmware (Arduino + LVGL factory build) lives in the sibling PlatformIO project
  **`../LilyGoLib-PlatformIO`**, env **`twatch_ultra`** (board `lilygo-t-watch-ultra`, 16 MB flash).
- The Node test tools live here in **`examples/factory/tools/`** and run from a PC against the watch's IP.
- Partitions (`examples/factory/partitions.csv`): two 4 MB OTA app slots (`app0`/`app1`), 7.9 MB FFat,
  a 64 KB `coredump` partition — so OTA rollback and coredump decoding both work on device.

## 0. Prerequisites

- [ ] T-Watch Ultra + USB-C cable.
- [ ] A **FAT32** microSD card, **≤ 32 GB** (the only format the watch mounts). The desktop lives on it.
- [ ] On the PC: Node 22+ and Google Chrome (for the test tools), PlatformIO.
- [ ] The watch and the PC on the **same Wi-Fi network** (or use the watch's AP fallback, step 4).

## 1. Build and flash

From the PlatformIO project (not this repo):

```bash
cd ../LilyGoLib-PlatformIO

# Build + flash over USB (upload 921600 baud). Port is auto-detected;
# add --upload-port /dev/cu.usbmodemXXXX if several devices are attached.
pio run -e twatch_ultra -t upload

# First flash after partition changes, or if it misbehaves, erase first:
pio run -e twatch_ultra -t erase -t upload

# Serial log with the ESP32 exception decoder (115200 baud):
pio device monitor -e twatch_ultra          # or: pio run -e twatch_ultra -t monitor
```

- [ ] Flash completes, watch reboots into the factory UI.
- [ ] Serial shows boot with no panic/backtrace. Keep the monitor open for the rest of the run — the
  web server prints the PIN and request logs, and `esp32_exception_decoder` symbolicates any crash.

## 2. Put the desktop on the SD card

The watch serves the desktop from `/sd/web/`. Three ways (any one):

```bash
# From this repo. Build the starter ZIP and unpack it to the card root on the PC:
cd examples/factory/tools
python3 web_build.py --zip dist        # -> dist/twatch-sd-<version>.zip  (web/ + data/ + README)
#   ...unzip dist/twatch-sd-*.zip to the card root with a reader, OR
python3 web_build.py --install /Volumes/<SDCARD>    # build straight onto a mounted card
```

- [ ] Card has `/sd/web/manifest.json` and the desktop files. (Or skip this and use `/rescue` in step 5
  to install from the browser — that path is worth testing too.)
- [ ] Insert the card into the watch.

## 3. Turn the web server on and read the PIN

The server is **off by default on the watch** (on only in the simulator). On the watch:

- [ ] Open **Settings › Web desktop** and toggle the **Web desktop** switch on.
- [ ] The page shows: a **PIN** (large), a **QR** of the address, **Address** `watch.local`, **IP**,
  and a **Browser / Desktop / Paired** card. Note the **IP** and **PIN**.
- Opening this page opens pairing with a fresh PIN; leaving it closes pairing.

## 4. (If no known Wi-Fi) the access-point fallback — simulator-untested

If the watch has not joined a known network ~20 s after the server starts, it raises an AP.

- [ ] The page's **Wi-Fi** and **Password** rows show an SSID `T-Watch-XXXX` and a password.
- [ ] Join that network from the PC/phone; the desktop is at **http://192.168.4.1/**.
- [ ] The QR on the page joins the network when scanned.
- [ ] The AP stops once the watch is back on a known network and nobody is using it.

## 5. First browser connection

- [ ] Open **http://<watch-ip>/** (or **http://watch.local/** to also check mDNS) in Chrome.
- [ ] Pair by entering the PIN from the watch screen. The **Paired** count goes to 1; **Browser** shows
  the connection.
- [ ] If the card had no desktop, **`/`** serves the embedded **rescue** page — use **Choose ZIP…** to
  install `dist/twatch-sd-<version>.zip` from the browser, then reload. (Tests the browser-side
  `DecompressionStream` install path on device.)

## 6. Automated API test against the watch

The UI test (`web_ui_test.mjs`) needs the `/api/_dev` hooks, which the watch does **not** have, so it is
PC/simulator only. The **API** test runs against the watch with the on-screen PIN:

```bash
cd examples/factory/tools
node web_api_test.mjs --url http://<watch-ip> --pin <pin>              # file routes on the SD card
node web_api_test.mjs --url http://<watch-ip> --pin <pin> --root flash # file routes on internal FFat
node web_boot_probe.mjs --url http://<watch-ip> --pin <pin> --png-dir /tmp/probe   # request log + shots
```

- [ ] `web_api_test.mjs` passes on `--root sd`.
- [ ] `web_api_test.mjs` passes on `--root flash`.
- [ ] `web_boot_probe.mjs` shows the desktop loading with no unexpected 404s/500s.
- [ ] Check `/api/caps` lists what this board really has (and omits what it doesn't).
- [ ] `not_fat32` detection: insert a non-FAT32 card — `GET /api/sd` should report it clearly rather than
  a bare `mount_failed` (the one P1 item still noted as weak).

## 7. Per-tool manual checks (all simulator-untested — real hardware only)

Open each app in the desktop and confirm it drives the real hardware. Captures should land as files on
the card (`/sd/data/...`) that the Files app can open.

- [ ] **Screen + input**: live watch screen mirrors; tap/swipe on it reaches the watch; screenshot works.
- [ ] **Monitor**: battery/memory graphs and uptime move; logs stream.
- [ ] **Sensors**: real IMU motion and compass; step count.
- [ ] **GNSS**: a real fix (outdoors), satellites, track saved to the card.
- [ ] **LoRa**: send/receive a packet with a second radio; RSSI/SNR real; packet log saved. Confirm the
  watch's LoRa/Walkie/Sub-G/LoRaWAN apps take the radio back (desktop gets `503 busy` while one is open).
- [ ] **NFC (reader)**: read a real tag; dump saved to the card. *(NFC Card **emulation** is disabled in
  this build — see note at the end; the reader is unaffected.)*
- [ ] **Recorder / Player**: record from the real mic; play back in the browser and on the watch.
- [ ] **Scanners**: real Wi-Fi and BLE device lists (NimBLE + Wi-Fi scan; async on device).
- [ ] **Wi-Fi tools**: connection info, DNS, TCP reach, channel analyzer against the real network.
- [ ] **Notifications**: a notification reaches the watch and vibrates.
- [ ] **Link (ESP-NOW)**: transfer a file to a second watch; receiver keeps SD I/O on the UI thread.
- [ ] **USB HID / BadUSB**: plug the watch into a computer; type text and run a DuckyScript (2 s arming).
      *(Note: the **native** BadUSB app is compiled out on the Ultra by `ARDUINO_USB_MODE=1`; this is the
      browser-driven HID path, which uses a different code path and should work.)*

## 8. OTA update + rollback — simulator-untested

Two real 4 MB slots make this a genuine test.

- [ ] Build a firmware image to flash over the air: `.pio/build/twatch_ultra/firmware.bin`.
- [ ] In the desktop **Updates** app, upload that `.bin`; it streams into the spare slot and verifies
  (the desktop confirms twice and checks the file first — it can brick the watch).
- [ ] Commit → the watch reboots into the other slot and runs.
- [ ] **Roll back** → the watch reboots into the previous slot. Confirm `GET /api/ota` `can_rollback`
  reflects state correctly before and after.

## 9. Desktop self-install over the internet — blocked on a release

The watch can download and install the desktop ZIP itself over HTTPS. This needs the GitHub release
published first (still deferred — awaiting go-ahead):

```bash
cd examples/factory/tools
python3 web_build.py --rescue                       # regenerate web_desktop_pin.h (commit it)
python3 web_build.py --zip dist
gh release create desktop-v0.1.0 dist/twatch-sd-0.1.0.zip
```

Then, with the card empty and the watch online:

- [ ] The watch downloads `twatch-sd-<version>.zip` (HTTPS, pinned size + SHA-256), unpacks it with the
  ROM inflate, swaps `/sd/web.new` → `/sd/web`, and creates `data/`.
- [ ] A cut download resumes with a `Range` request (`<zip>.part`).
- [ ] A ZIP already on the card installs without any network (newest wins; a bad-CRC ZIP is refused and
  the old desktop stays).

## 10. Battery and robustness (P6) — on-device validation

The code exists (idle shutdown, light sleep, OTA rollback, crash recovery); only hardware behaviour is
left to confirm.

- [ ] **No light sleep under a desktop**: while a browser is connected, screen-off goes *dark* (backlight
  off, CPU + Wi-Fi on) instead of light sleep; the next touch lights it. Wi-Fi stays up.
- [ ] **Idle shutdown**: with no desktop, no request, and the Web page closed for `web_idle_off_s`
  (default 600 s), the server turns itself off; it comes back at the next boot if still switched on.
- [ ] **Crash recovery**: if a crash occurs, the coredump partition captures it; the serial monitor's
  exception decoder symbolicates the backtrace; the watch recovers on reboot.
- [ ] Battery drain over an hour with a desktop connected is reasonable.

## 11. P9.5 Presence advanced modes — two real watches

Use two T-Watch Ultras and a fixed 2.4 GHz AP. Keep all devices fixed during calibration. These checks are
required before marking the Faro/alarm core hardware-validated; synthetic simulator frames do not count.

- [ ] **Router baseline:** calibrate in an empty still room; record actual CSI frames/s, RSSI and false-positive
      rate. Walk across the router/watch radio path and confirm motion, then confirm the 10-second presence hold.
- [ ] **Faro setup:** stop sensing on watch B, start its Faro transmitter, and verify channel 1–13 matches watch A's
      connected AP. Watch A discovers B's real MAC and independently completes 32-frame calibration.
- [ ] **Independent zones:** cross only the router/A path, then only the B/A path. API, native chart and desktop must
      identify the corresponding zone without forcing motion in the other. Stop B for over 3 seconds: Faro becomes
      `absent`; restart it: the link recovers without restarting router sensing.
- [ ] **Rates/coexistence:** compare Faro's accepted/error counters with observed frames and receiver CSI rate. Check
      Link/ESP-NOW and normal browser traffic still work. `sent` is driver acceptance, not over-air delivery proof.
- [ ] **Alarm:** exit delay and 1.5-second grace suppress premature triggers; motion in each zone triggers the correct
      label, watch vibration and audible beeps. Manual disarm works while arming/armed/triggered. Siren duration
      automatically returns to `off`; stopping CSI disarms and restores the previous Wi-Fi power-save mode.
- [ ] **History/restart:** verify 120 buckets / 60 seconds on native and desktop. Reboot: saved timing settings remain,
      but alarm state and graph are clear (history is intentionally not persisted).
- [ ] **Reconnect:** drop and restore Wi-Fi during sensing and while triggered. Router recalibrates on reconnect, alarm
      timeout still expires, no stale Faro remains linked, and no radio-owner/power-save leak follows teardown.

---

**NFC card emulation is disabled in this build.** `demo_ce.h` (ST's RFAL card-emulation demo glue) is not
in the `ST25R3916-fork`/`NFC-RFAL-fork` libraries, so `beginEmulation()` returns "Card emulation not
available in this build". The NFC **reader** and the web NFC tool are unaffected. To enable emulation
later, drop ST's `demo_ce.{h,c}` onto the include path — the code re-activates automatically via
`#if __has_include(<demo_ce.h>)` (`LILYGO_NFC_HAS_CE`) in `src/nfc/LilyGoNfcService.cpp`.
