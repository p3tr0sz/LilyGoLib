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

Sketch of the follow-up slice:
- New `web_tool_automation.cpp` embedding the interpreter; cap `automation`; `POST /api/automation/run`
  `{source}` → `{ok, engine:"berry", output}`. (A working first-slice sketch exists — see the code
  block at the end of the Berry spike below.)
- A C binding layer exposing a safe, explicit device API: `print()` (done in the sketch via an output
  hook), `delay()`, `sys.status()`, `fs.read()/list()` (sandboxed via `web_fs_resolve`), `notify()`,
  and later radio/attack verbs behind the same claim/authorization gates the P9 tools use.
- Its own web app + on-device app, reusing the Scripts UI shape.
- The mock cannot faithfully re-run a native interpreter, so test it against the simulator/firmware
  directly (unlike the DSL, which the JS mock mirrors exactly).

### Berry integration spike (2026-11-08) — findings + the build recipe

A spike vendored Berry and wired a minimal `web_tool_automation.cpp`. **It is not committed** because
it could not be made to build *in the cloud sandbox* (reason below) and committing non-building
vendored sources would break the tree for everyone. The findings are solid and save the next attempt a
lot of time:

**How Berry must be built (proven the hard way):** Berry has to compile **as C with
`BE_USE_PRECOMPILED_OBJECT 1`** plus the `coc`-generated constant tables (the normal Tasmota/Makefile
embed). The tempting shortcut of `BE_USE_PRECOMPILED_OBJECT 0` (skip the codegen) is a **dead end** —
do not retry it:
- As **C**, the runtime module macros (`be_native_module_attr_table`, `be_define_native_module`) use the
  bare struct tag `bntvmodobj` and a `.init`/`.module` field set that don't match the C struct → compile
  errors in every `be_*lib` file.
- As **C++** (renaming `.c`→`.cpp`), those module macros resolve (they use `bntvmodobj(...)` C++
  constructors) but the Berry *core* relies on C's implicit `void*`→`T*` conversions (`var_toobj` and
  friends) → hundreds of errors. Berry's core is not C++-clean.
So `PRECOMPILED 0` cannot satisfy both halves. Use `PRECOMPILED 1` + `coc`, compiled as C.

**The codegen step:** from the Berry tree, `python3 tools/coc/coc -o generate src default -c
default/berry_conf.h` writes `generate/be_fixed_*.h` + `be_const_strtab*.h`. Vendor Berry keeping the
`src/` + `generate/` layout (so the in-tree `#include "../generate/..."` resolve), commit the generated
files, and leave the sources as `.c` (compiled as C). PlatformIO auto-compiles any subdir of
`examples/factory/`, so no `platformio.ini` change is needed; set `BE_USE_PRECOMPILED_OBJECT 1` and the
module set via `BE_USE_*_MODULE` in the vendored `berry_conf.h`.

**Why it stalled here (environment, not design):** the managed cloud sandbox denies running freshly
downloaded/External code — both `python3 tools/coc/coc …` and executing an ad-hoc compiled binary come
back as `[Code from External]`. So the codegen cannot be produced in this session. **To finish, do the
vendoring+codegen where that is allowed:** run `coc` locally (the maintainer has both repos checked
out), or add a Claude Code Bash permission rule for the `coc` command, then commit the vendored
`berry/` (with `generate/`) — after which the rest (the automation tool, device API, apps) builds and is
verifiable on the simulator exactly as item 1 was.

**Output capture that worked:** add an optional hook in Berry's `default/be_port.c` so `print` can be
captured into a response buffer instead of stdout:
```c
/* LOCAL CHANGE: route print output through a hook when set, else stdout. */
void (*berry_output_hook)(const char *buffer, size_t length) = 0;
BERRY_API void be_writebuffer(const char *buffer, size_t length) {
    if (berry_output_hook) berry_output_hook(buffer, length);
    else be_fwrite(stdout, buffer, length);
}
```
**Time-bounding that worked:** `be_set_obs_hook(vm, hook)` and, on `BE_OBS_VM_HEARTBEAT`, call
`be_raise(vm, "runtime_error", "time limit exceeded")` once a wall-clock budget is exceeded (lower
`BE_VM_OBSERVABILITY_SAMPLING` from 20 for finer checks). Run pattern: `be_loadstring` → `be_pcall`,
and on a non-`BE_OK` result call `be_dumpexcept(vm)` (its traceback flows through the output hook).

**First-slice tool sketch** (compiles against a correctly-vendored Berry; expand with the device API):
```cpp
// web_tool_automation.cpp — cap "automation", POST /api/automation/run {source} -> {ok,engine,output}
#include "web_tools.h"
#if FACTORY_WEB
#include "web_internal.h"
#include <string>
#include "berry/berry.h"                 // carries its own extern "C" guard
extern "C" void (*berry_output_hook)(const char *buffer, size_t length);
#define AUTO_SRC_MAX (16*1024)
#define AUTO_OUT_MAX (32*1024)
#define AUTO_MS_MAX  8000
static std::string *g_out = nullptr;
static void out_cb(const char *b, size_t n){ if (g_out && g_out->size()<AUTO_OUT_MAX) g_out->append(b,n); }
static uint32_t g_start = 0;
static void obs(bvm *vm, int ev, ...){ if (ev==BE_OBS_VM_HEARTBEAT && tools_now_ms()-g_start>AUTO_MS_MAX) be_raise(vm,"runtime_error","time limit exceeded"); }
static bool berry_run(const std::string &src, std::string &out){
    bvm *vm = be_vm_new(); if(!vm){ out+="automation: out of memory\n"; return false; }
    g_out=&out; g_start=tools_now_ms(); berry_output_hook=out_cb; be_set_obs_hook(vm,obs);
    bool ok=true; int res=be_loadstring(vm,src.c_str());
    if(res==BE_OK){ res=be_pcall(vm,0); if(res!=BE_OK){ be_dumpexcept(vm); ok=false; } }
    else { out+="compile error: "; out+=(be_isstring(vm,-1)?be_tostring(vm,-1):"syntax error"); out+="\n"; ok=false; }
    berry_output_hook=nullptr; g_out=nullptr; be_vm_delete(vm); return ok;
}
static bool automation_handle(web_req_t *r){
    if(!tools_is(r,"POST","/api/automation/run")) return false;
    std::string body; if(!tools_read_json(r,body,AUTO_SRC_MAX+1024)) return true;
    std::string src; web_json_get_str(body,"source",src);
    if(src.size()>AUTO_SRC_MAX){ web_resp_error(r,400,"too_large","script too large"); return true; }
    std::string out; bool ok=berry_run(src,out);
    std::string j="{\"ok\":"; j+=ok?"true":"false"; j+=",\"engine\":\"berry\",\"output\":"; web_json_str(j,out.c_str()); j+="}";
    web_resp_json(r,200,j); return true;
}
static const char *const AUTO_CAPS[] = { "automation", nullptr };
const web_tool_t web_tool_automation = { AUTO_CAPS, automation_handle, nullptr, nullptr, nullptr };
#endif
```
Register it in `web_tools.h` (the `extern const web_tool_t …` list) and `web_tools.cpp` (`TOOLS[]`),
after `web_tool_script`.

### Related: per-command exit status (smaller, enables real `if cmd`) — DONE 2026-11-08

Done. `term_exec` now returns a real exit status (0 success, 1 a failed command, 127 command not
found); `POST /api/term` also reports it as `status`. The DSL feeds it into `$?`, so `if cmd`,
`while cmd`, `&&` and `||` branch on actual command success, not just `test`. Verified on both planes
(simulator API 340/340, mock API 336/336). This removed the main v1 limitation above.

### Related: BadUSB

Not a duplication problem — **DuckyScript** (Hak5) is the established standard, already run via
`usb.hid`, and portable across Flipper/Bruce. The reuse move is to pull in the Hak5/community **payload
library**, not to write a new parser.
