# Handoff — scripting → automation session (2026-11-08)

Branch `dev` (repo `p3tr0sz/LilyGoLib`; firmware build sibling `p3tr0sz/LilyGoLib-PlatformIO`).
This note is the session summary; the deep detail lives in `handoff-scripting.md` and `web-roadmap.md`.

## What shipped (5 commits on `dev`, newest last)

| Commit | What |
|---|---|
| `abaa4d9` | **feat:** `term_exec` returns a real exit status (0 / 1 / 127); `POST /api/term` reports it as `status`. The Scripts DSL feeds it into `$?`, so `if cmd` / `while cmd` / `&&` / `||` branch on real command success, not just `test`. |
| `da1c97f` | Verified that on the firmware simulator (API 340/340). |
| `2092d20` | **Licensing correction:** mJS is **GPLv2**-or-commercial (the roadmap wrongly said Apache-2.0) → **disqualified**. Berry (MIT) is the interpreter. |
| `66ed4ca` | Berry integration recipe + findings written up. |
| `d0c4e38` | **feat:** Berry (MIT) embedded as the **automation app** — cap `automation`, `POST /api/automation/run`. First slice: full language + `print`. |

All verified; working tree clean; nothing half-built committed.

## Current state of the scripting area

Two distinct planes, both shared by a web app and an on-device app:

1. **Scripts (DSL)** — `web_tool_script.cpp`, cap `script`, `/api/script/*`, `.scr` files in `/sd/scripts`.
   A small sh-like shell language over `term_exec`. Exit-status work is **done** — conditionals now work
   on real command success. The JS mock mirrors it line-for-line.
2. **Automation (Berry)** — `web_tool_automation.cpp`, cap `automation`, `POST /api/automation/run`.
   A real language (functions, maps, lists, stdlib) via vendored **Berry** (MIT) under
   `examples/factory/berry/`. **First slice only: language + `print`.** The JS mock does **not**
   implement it (can't re-run a native interpreter), so it is verified on the firmware simulator.

Verification this session: simulator API **345/345**, mock API **336/336**, simulator firmware builds
clean (`emulator_watch_ultra` and `twatch_ultra`).

## Flash budget — now the main constraint

`twatch_ultra` flash went **93.0% → 96.2%** (~157 KB free). Berry's first slice cost ~3.2% / ~133 KB.
**Before the next automation slice, reclaim flash** by trimming unused Berry modules in
`berry/src/berry_conf.h` (`BE_USE_JSON_MODULE`, `BE_USE_OS_MODULE`, `BE_USE_DEBUG_MODULE`,
`BE_USE_SOLIDIFY_MODULE`, `BE_USE_INTROSPECT_MODULE` → 0 as appropriate) and re-running `coc`.

## Next steps (automation item, in order)

1. Trim Berry modules → re-measure flash.
2. Bind a safe device API into Berry: `delay()`, `sys.status()`, sandboxed `fs.read()/list()` (via
   `web_fs_resolve`), `notify()`, then radio/attack verbs behind the same P9 claim/authorization gates
   the security tools use.
3. Front-ends: a web Automation app + an on-device LVGL app (reuse the Scripts UI shape), and a
   `/sd/automation/*.be` script library (`list/read/save/delete`, mirroring `/api/script/*`).
4. Then resume the `web-roadmap.md` "Next feature work" list (unified file manager, wardriving→WiGLE,
   dynamic Karma, TOTP, notepad, per-app help).

## Gotchas that cost time this session (don't relearn them)

- **Berry build:** compile **as C with `BE_USE_PRECOMPILED_OBJECT 1` + `coc`-generated tables**.
  `PRECOMPILED 0` is a dead end (C++-only module macros vs. a C-only core). Everything is flattened into
  `berry/src/` so includes resolve same-dir — **no `platformio.ini` change**. Only local edit to Berry:
  the `berry_output_hook` in `src/be_port.c`. Re-run `coc` after any `berry_conf.h` module change. Full
  recipe: `handoff-scripting.md` → "Berry landed".
- **Running `coc`** needs a Bash permission rule (`Bash(python3 tools/coc/coc:*)`, in
  `~/.claude/settings.json`). A cloud session's sandbox otherwise denies it and any ad-hoc external
  binary as `[Code from External]`, and Claude cannot edit its own settings (`[Self-Modification]`), so
  the **user** must add such rules.
- **Firmware simulator** (details in the `drive-native-sim-screen` memory): build + run from the
  PlatformIO repo (`pio run -e emulator_watch_ultra`, then `./.pio/build/emulator_watch_ultra/program`).
  Launch with the **default SDL driver** — `SDL_VIDEODRIVER=dummy` makes it exit immediately. A **stale
  simulator from a previous session holding `:8080` serves old code**, so `pkill -f
  emulator_watch_ultra/program` before testing and check the binary mtime. API tests:
  `node web_api_test.mjs --url http://localhost:8080 --dev-pin`.
- **Licensing:** only permissively-licensed engines may be vendored. mJS = GPLv2 (out); Berry/Duktape/
  Lua = MIT. Never copy Bruce (GPL) or NucleoOS (AGPL) code.

## Commit conventions (this repo)

One-line Conventional-Commits messages; **no `Co-Authored-By` trailer**. Develop on `dev`, push to `dev`.
