# Handoff — automation reversal + execution-model study (2026-11-08, session 2)

Branch `dev` (repo `p3tr0sz/LilyGoLib`; firmware build sibling `p3tr0sz/LilyGoLib-PlatformIO`).
Follows `handoff-2026-11-08-scripting-automation.md` (session 1, which landed Berry). This session **reversed
the Berry decision** after settling the execution model, and specced the replacement.

## What shipped (3 commits on `dev`, newest last)

| Commit | What |
|---|---|
| `a7541d7` | **chore:** disconnect Berry from the build — unregistered from `web_tools.cpp` `TOOLS[]`, TU gated behind `FACTORY_AUTOMATION`; `--gc-sections` drops it. Berry source kept in-tree. |
| `fb53663` | **docs:** `automation-rule-engine.md` (the replacement design) + `eval-loadable-native-apps.md` (the `.nap` evaluation); roadmap note marks the reversal. |
| `bb79adb` | **docs:** roadmap feature-work item #8 = loadable native apps (gated). |

Verified: `twatch_ultra` flash **96.2% → 93.0%** (full ~133 KB reclaimed, pre-Berry baseline); sim API
**340/340**, mock **336/336**. Working tree clean.

## The decision, in one paragraph

Watch "automation" needs **trigger → condition → action running headless** (nothing connected), not a
Turing-complete language. An embedded interpreter (Berry) delivered headless programmability but cost ~133 KB
of a tight flash budget. We studied NucleoOS's execution model to settle it (see below) and concluded: drop
the interpreter, build a small **rule engine** instead, and keep *full programmability* out of scope (the
Scripts DSL already covers imperative sequences headless, and a rule action can run a `.scr`).

## Execution-model findings (NucleoOS, `../NucleoOs`, AGPL — model only, no code copied)

- **On-device apps are native C/C++**, not JS. Two tiers: **resident** built-ins compiled into firmware, and
  **loadable** native apps (`.nap`) streamed off SD on demand into a shared **flash scratch region** (one at
  a time; switching unloads the previous — this is the "it installs the app on switch" behaviour, with per-app
  data under `/sd/apps/<id>/`). `.nap` = PIE ELF32 Xtensa shared object, imports firmware by name from an ABI
  export table, gated by ABI-level + layout-fingerprint discipline.
- **`www` + manifest bundles** are the companion/web plane; a `service`-runtime app = www front-end + firmware
  service + event bus. That is exactly our automation shape (author in web JS, execute in firmware).
- **We already have the event bus:** `web_events_publish` (`web_core.cpp:734`) emits the right topics but
  **drops events when no WebSocket client holds the seat**. Closing that drop is the whole headless gap.

Three efficiency levers fell out (cheapest first): **A** drop the interpreter (done, −133 KB); **B** the
headless rule engine (next); **C** loadable `.nap` apps (big, gated on a flash map).

## START HERE next session — B step 1

Implement the in-process event tap and prove the headless path *before* any matcher:

1. In `web_core.cpp` add `web_events_subscribe(web_event_sink_t, void*)` + `web_events_dispatch_local(topic,
   d_json)`, and call the latter at the end of `web_events_publish` **regardless of `s_seat`**. The sink runs
   on the publish thread under `s_lock`, so it must only enqueue (no work inline).
2. Register a no-op/count sink from the (redesigned, un-gated) automation module.
3. **Sim check:** publish an event with **no WebSocket client connected** and assert the sink received it.
   This is the one thing the JS mock can't prove (it has no firmware bus).

Then, in order (roadmap item #1): the JSON rule matcher drained on the UI `tick`; `/api/automation/{list,
read,save,delete,test}` (mirror `/api/script/*`); web Automation app (JS authoring) + on-device LVGL
list/enable. The matcher is pure logic, so **mirror it in the JS mock** (dual-plane testing is back — unlike
Berry). Full spec: `automation-rule-engine.md`.

## Don't relearn (carried from session 1, still true)

- **Firmware sim:** build + run from the PlatformIO repo (`pio run -e emulator_watch_ultra`, then
  `./.pio/build/emulator_watch_ultra/program` with the **default SDL driver** — `SDL_VIDEODRIVER=dummy`
  exits immediately). Kill a stale sim on `:8080` before testing. API tests:
  `node examples/factory/tools/web_api_test.mjs --url http://localhost:8080 --dev-pin`.
- **Flash is the constraint.** Keep interpreter-class footprints out; the rule engine must stay kilobytes.
- **Licensing:** NucleoOS AGPL, Bruce GPL — study patterns, copy no code. Only permissive engines may be
  vendored (and we've decided not to vendor one for automation).
- **Berry removal:** `examples/factory/berry/` can be deleted once the rule engine lands and we're sure we
  don't want the interpreter back (separate commit).

## Commit conventions

One-line Conventional-Commits messages; **no `Co-Authored-By` trailer**. Develop on `dev`.
