<div align="center" markdown="1">
  <img src=".github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGoLib🌟</h1>

<p align="center">
  <a href="https://github.com/Xinyuan-LilyGO/LilyGoLib/releases"><img alt="Latest version" src="https://img.shields.io/github/v/tag/Xinyuan-LilyGO/LilyGoLib?sort=semver&amp;label=version"/></a>
  <a href="https://www.arduinolibraries.info/libraries/lily-go-lib"><img alt="Arduino Library Manager" src="https://img.shields.io/badge/Arduino-Library%20Manager-00878F?logo=arduino&amp;logoColor=white"/></a>
  <a href="https://github.com/espressif/arduino-esp32"><img alt="ESP32 platform" src="https://img.shields.io/badge/platform-ESP32-E7352C?logo=espressif&amp;logoColor=white"/></a>
  <a href="./LICENSE"><img alt="MIT license" src="https://img.shields.io/github/license/Xinyuan-LilyGO/LilyGoLib"/></a>
</p>

<p align="center">
  <a href="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/base_examples_ci.yml"><img alt="Base examples build" src="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/base_examples_ci.yml/badge.svg"/></a>
  <a href="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/lvgl_examples_ci.yml"><img alt="LVGL examples build" src="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/lvgl_examples_ci.yml/badge.svg"/></a>
  <a href="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/radio_examples_ci.yml"><img alt="Radio examples build" src="https://github.com/Xinyuan-LilyGO/LilyGoLib/actions/workflows/radio_examples_ci.yml/badge.svg"/></a>
</p>

### Factory UI Preview

<table>
  <tr>
    <td width="50%" align="center">
      <a href="./docs/factory-ui.md#t-deck">
        <img src=".github/assets/factory-ui/tdeck-preview.png" alt="T-Deck Factory UI preview" width="100%">
      </a>
      <br><strong>T-Deck or T-Vibe-Dev</strong>
    </td>
    <td width="50%" align="center">
      <a href="./docs/factory-ui.md#t-lora-pager">
        <img src=".github/assets/factory-ui/tlora-pager-preview.png" alt="T-LoRa Pager Factory UI preview" width="100%">
      </a>
      <br><strong>T-LoRa Pager or T-Vibe-Pager</strong>
    </td>
  </tr>
  <tr>
    <td width="50%" align="center">
      <a href="./docs/factory-ui.md#t-watch-s3">
        <img src=".github/assets/factory-ui/twatch-s3-preview.png" alt="T-Watch S3 Factory UI preview" width="100%">
      </a>
      <br><strong>T-Watch S3</strong>
    </td>
    <td width="50%" align="center">
      <a href="./docs/factory-ui.md#t-watch-ultra">
        <img src=".github/assets/factory-ui/twatch-ultra-preview.png" alt="T-Watch Ultra Factory UI preview" width="100%">
      </a>
      <br><strong>T-Watch Ultra</strong>
    </td>
  </tr>
</table>

<p align="center">
  <a href="./docs/factory-ui.md"><strong>Explore the Factory UI gallery</strong></a>
</p>

### `1` Get Started

- 🔧 **[T-Deck Quick Start](./docs/lilygo-t-deck.md)**
- 🔧 **[T-LoRa-Pager Quick Start](./docs/lilygo-t-lora-pager.md)**
- 🔧 **[T-Watch-Ultra Quick Start](./docs/lilygo-t-watch-ultra.md)**
- 🔧 **[T-Watch-S3 IDE Quick Start](./docs/lilygo-t-watch-s3.md)**
- 🔧 **[T-Watch-S3-Plus Quick Start](./docs/lilygo-t-watch-s3-plus.md)**
- 🔧 **[T-Watch-2020-V3 Quick Start](./docs/lilygo-t-watch-2020-v3.md)**
- 🔧 **[T-Watch (2019) Quick Start](./docs/lilygo-t-watch.md)**
- 🔧 **[T-Vibe Dev Quick Start](./docs/lilygo-t-vibe-dev.md)**
- 🔧 **[T-Vibe Pager Quick Start](./docs/lilygo-t-vibe-pager.md)**

### `2` Build, Flash & SD Card (T-Watch Ultra factory firmware)

The T-Watch Ultra factory firmware — the browser **web desktop** plus the on-device apps — is built
with PlatformIO. The firmware project is the sibling repo **`LilyGoLib-PlatformIO`** (env
`twatch_ultra`); its sources live here under **`examples/factory/`**. A desktop **simulator**
(`emulator_watch_ultra`) runs the same UI on your computer. For the full on-device checklist see
**[docs/web-device-bringup.md](./docs/web-device-bringup.md)**; for the generic flashing guide (web
flashers, download mode) see **[firmware/README.md](./firmware/README.md)**.

**Build**

```bash
cd ../LilyGoLib-PlatformIO
pio run -e twatch_ultra                 # firmware for the watch
pio run -e emulator_watch_ultra         # desktop simulator -> ./.pio/build/emulator_watch_ultra/program
```

**Back up the factory flash first** (one-time, on a brand-new watch — the generic factory `.bin`
cannot restore your unit's own calibration/pairing/NVS):

```bash
tools/backup_firmware.sh  --port /dev/cu.usbmodemXXXX     # -> firmware-backup-*.bin (+ .sha256)
tools/restore_firmware.sh --in firmware-backup-*.bin      # write it back later
```

**Flash the watch**

```bash
cd ../LilyGoLib-PlatformIO
pio run -e twatch_ultra -t upload               # build + flash over USB (port auto-detected)
pio run -e twatch_ultra -t erase -t upload      # erase first (after partition changes)
pio device monitor -e twatch_ultra              # serial log (115200)
```

If the USB port keeps dropping, put the watch in
[download mode](./docs/lilygo-t-watch-ultra.md#t-watch-ultra-enter-download-mode) (hold **BOOT**,
tap **RST**, release **BOOT**), then tap **RST** when done.

**Put the web desktop + content on the SD card** (FAT32, ≤ 32 GB):

```bash
cd examples/factory/tools
python3 web_build.py --sync /Volumes/<SDCARD>   # desktop + seed content (radio presets, portal
                                                # templates) + all data/ folders; keeps your captures
python3 web_build.py --zip  dist                # starter ZIP (install from the watch's /rescue page)
python3 web_build.py --rescue                   # regenerate web_rescue.h + web_desktop_pin.h
```

`--sync` mirrors the version-controlled seed tree (`examples/factory/sdcard/`) onto the card, copying
each seed file only when it is absent — re-running never clobbers captures or edits; pass `--force` to
overwrite the seeds with the repo's copies.

> ⚠️ After changing anything under `examples/factory/web/`, run `python3 web_build.py --rescue` and
> commit the regenerated headers, **then rebuild the firmware** — the firmware pins the desktop ZIP's
> SHA-256 at compile time, so a stale pin makes the watch reject the over-the-air desktop install.

**Cut a release** — push a `desktop-v*` tag (or run **Actions → release**): it verifies the pinned
headers, builds the deterministic SD ZIP, checks it against `web_desktop_pin.h`, optionally builds the
firmware, and publishes both. See **[`.github/workflows/release.yml`](./.github/workflows/release.yml)**.

**Scripts reference**

| Script | What it does |
| --- | --- |
| `tools/backup_firmware.sh` | Dump the full flash image to a file (back up before flashing) |
| `tools/restore_firmware.sh` | Write a backup image back onto the board |
| `examples/factory/tools/web_build.py` | Build / sync the SD web desktop + content (`--sync`, `--zip`, `--install`, `--rescue`) |
| `.github/workflows/release.yml` | Tagged release: SD ZIP + firmware `.bin` |

### `3` Hardware Information

- 🔧 **[T-Deck Hardware Information](./docs/hardware/lilygo-t-deck.md)**
- 🔧 **[T-LoRa-Pager Hardware Information](./docs/hardware/lilygo-t-lora-pager.md)**
- 🔧 **[T-Watch-Ultra Hardware Information](./docs/hardware/lilygo-t-watch-ultra.md)**
- 🔧 **[T-Watch-S3 Hardware Information](./docs/hardware/lilygo-t-watch-s3.md)**
- 🔧 **[T-Watch-S3-Plus Hardware Information](./docs/hardware/lilygo-t-watch-s3-plus.md)**
- 🔧 **[T-Watch-2020-V3 Hardware Information](./docs/hardware/lilygo-t-watch-2020-v3.md)**
- 🔧 **[T-Watch (2019) Hardware Information](./docs/hardware/lilygo-t-watch.md)**
- 🔧 **[T-Vibe-Dev Hardware Information](./docs/hardware/lilygo-t-vibe-dev.md)**
- 🔧 **[T-Vibe-Pager Hardware Information](./docs/hardware/lilygo-t-vibe-pager.md)**

### `4` GPS AssistNow Usage Guide

- 🔧 **[GPS AssistNow Usage Guide](./docs/assistNow/assistNow.md)**

### `5` Unable to flash again

- ⚠️ **[Flash Steps Guide](./firmware/README.md)**
