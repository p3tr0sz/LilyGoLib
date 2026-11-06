# Web desktop: development guide

How to build, run and test the T-Watch Ultra web desktop and its firmware. Read this before changing
anything under `examples/factory/web*` or `src/nfc`. The plan and status live in `web-roadmap.md`; the
API contract in `web-api.md`; the on-hardware checklist in `web-device-bringup.md`.

## Two repositories

| Repo | What it is | You edit here |
|---|---|---|
| **`LilyGoLib`** (this repo) | the Arduino **library**, the `examples/factory` firmware, the web desktop (`examples/factory/web/`), the Node test tools (`examples/factory/tools/`) and all the docs | yes — all the source |
| **`LilyGoLib-PlatformIO`** (sibling: `../LilyGoLib-PlatformIO`) | the buildable PlatformIO **project**. It symlinks this library (`symlink://../LilyGoLib`), and holds `sim_sd/` + `sim_flash/` (the simulator's card and flash) and the build envs | no — only run builds; `sim_sd/` is generated |

The single most common mistake: **`pio run` must run from `LilyGoLib-PlatformIO`, not from `examples/factory`** (that folder has no `platformio.ini`). Build envs: `twatch_ultra` (the watch) and `emulator_watch_ultra` (the LVGL simulator).

## What the pieces are

- **Desktop** — `examples/factory/web/`: plain HTML, CSS and ES modules, **no build step** while developing. `js/shell.js` is the OS shell (pairing, launchpad, dock, windows, tray, system menu); `js/ui.js` DOM/icon/dialog helpers; `js/api.js` the API client; `apps/<id>/app.js` one module per app exporting `mount(el, ctx)`. Served off the SD card at `/sd/web/`.
- **Firmware** — `examples/factory/web_*.cpp`, all behind `FACTORY_WEB`. Shared core (`web_core.cpp`, `web_fs.cpp`, `web_json.cpp`) is pure POSIX + no ESP calls, so it runs on the watch and on the PC; a thin transport (`web_transport_esp.cpp` on the watch, `web_transport_posix.cpp` in the simulator) is the only difference. Tools are `web_tool_*.cpp` behind the `web_tools.h` contract.
- **Naming** — everything is ours and named `web-*` / `FACTORY_WEB`. NucleoOS (`../NucleoOs`) is the model for patterns and capabilities only; none of its code, names or files are used (its firmware is PolyForm-NC, its desktop AGPL).

## The three test targets

Two Node tests in `examples/factory/tools/` (need **Node 22+**, and **Google Chrome** for the UI test):
`web_api_test.mjs` checks the API; `web_ui_test.mjs` drives the desktop in headless Chrome (pairing, OS
shell, Files, SD removal, Settings, Screen, every tool app, phone layout). A third, `web_desktop_test.mjs`,
checks the watch-installs-itself flow.

| Target | Serves | Port | Run it |
|---|---|---|---|
| **Mock** (fastest — no build) | `web/` live from disk | 5620 | `node web_mock_device.mjs --dev &` then `node web_api_test.mjs --dev-pin` / `node web_ui_test.mjs` |
| **Simulator** (real firmware) | the built web code | 8080 | build + run (below), then `node web_api_test.mjs --url http://localhost:8080 --dev-pin` / `node web_ui_test.mjs --url http://localhost:8080` |
| **Watch** (real hardware) | the same code on `esp_http_server` | — | see `web-device-bringup.md` |

The `--dev` mock and the simulator expose `/api/_dev/*` hooks (the PIN, SD insert/removal, fake packets)
that the watch never has; `web_ui_test.mjs` needs them, so it runs against the first two only.

### The fast loop (most desktop/API work)

Use the **mock** — no build, edits to `web/` are picked up on reload:

```bash
cd examples/factory/tools
node web_mock_device.mjs --dev &          # http://localhost:5620
node web_ui_test.mjs                       # desktop UI
node web_api_test.mjs --dev-pin            # API
```

### The simulator loop (firmware, or final check)

```bash
# 1. put the current desktop on the simulator's card
cd examples/factory/tools
python3 web_build.py --install ../../../../LilyGoLib-PlatformIO/sim_sd

# 2. build + run the simulator (serves on :8080; opens an SDL window; PIN printed to its console)
cd ../../../../LilyGoLib-PlatformIO
pio run -e emulator_watch_ultra -t execute        # or run ./.pio/build/emulator_watch_ultra/program

# 3. point the tests at it
cd ../LilyGoLib/examples/factory/tools
node web_ui_test.mjs  --url http://localhost:8080
node web_api_test.mjs --url http://localhost:8080 --dev-pin
```

Simulator storage is folders: `/sd` → `sim_sd/` (or `$TWATCH_SIM_SD`), `/flash` → `sim_flash/` (or
`$TWATCH_SIM_FLASH`), relative to the working directory the program runs in. In the SDL window **F3**
inserts/removes the card and **F2** is the power button.

### The self-install test (watch-installs-the-desktop flow)

Needs the mock acting as the release server and the simulator on a non-default port so the two don't
collide:

```bash
node web_mock_device.mjs --dev --dl /tmp/dl &
TWATCH_SIM_WEB_PORT=8090 TWATCH_SIM_DESKTOP_URL=http://localhost:5620/dl ./.pio/build/emulator_watch_ultra/program &
node web_desktop_test.mjs --url http://localhost:8090 --release http://localhost:5620 --dl /tmp/dl
```

## Before every commit

- Run **both** `web_api_test.mjs` and `web_ui_test.mjs` against the **mock** (fast); run them against the
  **simulator** too for anything touching firmware or the shell. Current baselines: `web_ui_test.mjs`
  **142/142** simulator / **141/141** mock; `web_api_test.mjs` **250/250** simulator / **246/246** mock;
  `web_desktop_test.mjs` **24/24**. A new feature should add checks, not just keep the count.
- When editing the desktop shell, **keep the selectors the UI test anchors on**: `.bar` (must still show
  the battery % and the SD status text), `.launcher .tile`, `.win` + `.win .back`, `.link-dot.on`, and the
  phone full-screen window. The UI test opens apps by hash (`#app=<id>`), not by clicking tiles.
- Confirm the **watch firmware still links**: `pio run -e twatch_ultra` (from `LilyGoLib-PlatformIO`). It
  links as of `793aafc`; the NFC `demo_ce.h` card-emulation dependency is optional via `__has_include`
  (`LILYGO_NFC_HAS_CE` in `src/nfc/LilyGoNfcService.cpp`), so don't re-add a hard `#include` of it.
- Commit style: one-line messages, no `Co-Authored-By` trailer (repo convention).

## Gotchas worth knowing

- `pio run` from the wrong directory → *"Not a PlatformIO project"*. Run it from `LilyGoLib-PlatformIO`.
- `web_ui_test.mjs --png-dir <dir>`: the directory must **exist first** (`mkdir -p` it) — the test writes
  straight into it.
- The UI test runs at **1280×800** (desktop) until the phone section, which overrides to 390×844 and clears
  it afterwards. New desktop-only checks go before the phone section.
- Headless Chrome path is auto-detected at the usual macOS/Linux locations; pass `--chrome <path>` if yours
  differs.
- Background a mock/simulator you start, and stop it when done (`pkill -f web_mock_device`,
  `pkill -f emulator_watch_ultra/program`).
