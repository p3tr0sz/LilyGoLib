# Scripting engine — handoff

The watch's scripting layer. Delivered **2026-11-08**: a small sh-like engine shared by a web app and
an on-device app, built on the Terminal shell. This doc is the language reference, the architecture,
and the deferred plan for a full interpreter-backed automation app.

## What shipped (the sh-like DSL)

One engine, `script_run(cwd, src) -> output`, in `examples/factory/web_tool_script.cpp`. Two thin
front-ends, exactly like the Terminal (`web_tool_term.cpp`):

- **Web** — `web/apps/scripts/app.js` (edit + run), `POST /api/script/run`, and a `/sd/scripts`
  library over `GET/POST /api/script/{list,read,save,delete}`. Cap `script`.
- **Device** — `ui_script.cpp`, a Scripts app under **Tools** that lists `/sd/scripts/*.scr` and runs
  the one you tap. Editing is web-only (an on-screen keyboard is a poor editor); the watch is the runner.
- **Mock parity** — `tools/web_mock_device.mjs` mirrors `script_run` line-for-line and serves the same
  routes, so both test planes agree (as for the Terminal).

### Language

Deliberately a *subset*, not bash. It is a thin control layer over the Terminal command set.

| Feature | Form |
|---|---|
| Comment | `# to end of line` (at a word boundary) |
| Separators | newline and `;` |
| Variables | `NAME=value`; expand `$NAME`, `${NAME}`, `$?` (last exit status) |
| Quoting | `'literal'` (no expansion), `"expands $vars"` |
| Arithmetic | `$(( expr ))` — integer, `+ - * / %` and `( )`, bare names read variables |
| Builtins | `echo`, `set` (list vars), `sleep N` (seconds), `test` / `[ … ]`, `true`, `false`, `:`, `exit [N]`, `help` |
| `test` / `[ ]` | `-z` `-n` `-e` `-f` `-d`; `=` `!=`; `-eq` `-ne` `-lt` `-le` `-gt` `-ge` |
| Conditionals | `if LIST; then … elif LIST; then … else … fi` |
| Loops | `for NAME in WORDS; do … done`, `while LIST; do … done` |
| Operators | `LIST && LIST`, `LIST || LIST` |

Everything that is not a builtin is dispatched to `term_exec` (the Terminal shell): `ls`, `cat`, `cd`,
`status`, `logs`, `reboot`, … behave identically and stay sandboxed to `/sd` and `/flash` through
`web_fs_resolve`. Run `help` as a one-line script for the in-engine reference. A seed demo lives at
`examples/factory/sdcard/scripts/hello.scr` (mirrored to a card by `web_build.py --sync`).

### Known limits (v1, by design)

- **External-command exit status is always success.** `term_exec` returns text, not a status, so
  `if ls /missing; then …` takes the `then` branch. Use `test` / `[ … ]` for real conditionals. Giving
  external commands a real status is the natural next slice (see below).
- **No pipes `|`, redirection `>`, subshells, globbing, functions, or job control.** Commands return
  whole strings, so pipes/redirection are a deliberate later slice.
- **Runs synchronously and is bounded.** Caps in `web_tool_script.cpp`: `SCRIPT_STEP_MAX` 20000,
  `SCRIPT_MS_MAX` 8 s, `SCRIPT_OUT_MAX` 32 KB, `SCRIPT_SLEEP_MAX` 5 s/sleep, `SCRIPT_DEPTH_MAX` 24.
  A run blocks its web request (and, on-device, the UI) for its duration — fine within these caps; a
  background/async runner is future work.
- `for … in` does not field-split an expanded variable; each word is one item.

### Tests

- `tools/web_api_test.mjs` — the `scripting` section: expansion, `for`/`while`/`$(( ))`, `if`/`elif`,
  `&&`/`||`, quoting, comments, `help`, parse errors, fall-through to the shell, and the full
  save/list/read/delete lifecycle with bad-name and 404 cases.
- `tools/web_ui_test.mjs` — the `scripts` section: open, run (output shown), and a parse error surfaced.

Verified this session: firmware builds (`emulator_watch_ultra`); mock API 329/329, simulator API
333/333; mock desktop 180/180, simulator desktop 181/181.

## Deferred: a full interpreter-backed "automation" app

Decision **2026-11-08**: keep the DSL as the *shell-scripting* layer and, for genuine programmability,
add a **separate** app around a proven embeddable interpreter rather than growing the DSL into a
half-made language. Research summary:

| Engine | License | Footprint | Precedent | Notes |
|---|---|---|---|---|
| ~~**mJS**~~ | **GPLv2-or-commercial** ✗ | <50 KB flash, ~1–2 KB RAM | **Flipper Zero** (modified mJS) | **disqualified** — its `LICENSE` is GPLv2, which would virally GPL the firmware. Verified 2026-11-08. |
| **Berry** | MIT | small, OO | **Tasmota** (ESP32) | clean license, ESP32-proven — **the lead candidate** |
| **Duktape** | MIT | heavier | **Bruce** | full ES5; ports to ESP32 easily |
| **Lua** | MIT | ~200 KB | NodeMCU | the reference embeddable; largest |

License check done (2026-11-08): mJS's in-repo `LICENSE` is GPLv2-or-commercial, **not** Apache-2.0 as
an earlier draft of this note claimed. Berry, Duktape and Lua are MIT and confirmed permissive.

**Flash budget matters here.** The DSL alone took the watch build from 91.8% → **93.0%** flash
(~49 KB), leaving ~293 KB headroom. Berry should fit (Tasmota ships it on ESP32); **Duktape and Lua
would be tighter.** Measure a real `env:twatch_ultra` build before committing to one.

**Recommended:** **Berry** (MIT, ESP32/Tasmota precedent) — the only permissive engine that is both
small enough and proven on this class of hardware. Do **not** embed mJS (GPLv2); do **not** copy Bruce
(GPL) or NucleoOS (AGPL) code — only permissively-licensed engines may be vendored.

Sketch of the follow-up slice (no code yet):
- New `web_tool_autoscript.cpp` (or similar) embedding the interpreter; cap `automation` (or `js`).
- A C binding layer exposing a safe, explicit device API: `print()`, `delay()`, `sys.status()`,
  `fs.read()/list()` (sandboxed via `web_fs_resolve`), `notify()`, and later radio/attack verbs behind
  the same claim/authorization gates the P9 tools use.
- Its own web app + on-device app, reusing the Scripts UI shape.
- The mock cannot faithfully re-run a native interpreter, so test it against the simulator/firmware
  directly (unlike the DSL, which the JS mock mirrors exactly).

### Related: per-command exit status (smaller, enables real `if cmd`) — DONE 2026-11-08

Done. `term_exec` now returns a real exit status (0 success, 1 a failed command, 127 command not
found); `POST /api/term` also reports it as `status`. The DSL feeds it into `$?`, so `if cmd`,
`while cmd`, `&&` and `||` branch on actual command success, not just `test`. Verified on both planes
(simulator API 340/340, mock API 336/336). This removed the main v1 limitation above.

### Related: BadUSB

Not a duplication problem — **DuckyScript** (Hak5) is the established standard, already run via
`usb.hid`, and portable across Flipper/Bruce. The reuse move is to pull in the Hak5/community **payload
library**, not to write a new parser.
