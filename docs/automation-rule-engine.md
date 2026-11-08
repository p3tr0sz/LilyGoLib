# Automation — headless rule engine (design, 2026-11-08)

Supersedes the Berry automation app (disconnected in `a7541d7`; Berry source kept in-tree, its TU gated
behind `FACTORY_AUTOMATION`). Decision trail: an embedded interpreter buys *full programmability* but costs
~133 KB of flash and only runs headless at that price. What "automation" actually needs on a watch is
**trigger → condition → action**, running **with no client connected** — and we already have almost all the
pieces. This replaces the interpreter with a small rule engine that reuses them.

## Why not Berry / pure browser JS

- **Pure browser JS (NucleoOS `www`-app model):** zero firmware flash, full language — but the JS engine is
  the *browser's*, so logic only runs while a desktop/companion client holds the WebSocket seat. A watch
  automation ("buzz when the battery is low", "launch an app on a button hold") must fire with nothing
  connected. Tethered execution disqualifies it as the *executor*. It is the right place for *authoring*.
- **Embedded interpreter (Berry):** headless and fully programmable, but ~133 KB and a native-interpreter
  footprint that the JS mock cannot mirror (so it could only ever be tested against the simulator).
- **Rule engine (this):** headless, kilobytes, and — because the matcher is pure logic — the JS mock *can*
  mirror it line-for-line, restoring dual-plane testing.

The one thing we give up vs. Berry is Turing-complete *computation* inside an automation (loops, data
structures). The Scripts DSL already covers imperative sequences headless; a rule's action can *run* a
`.scr` script, so anything the DSL can do is still reachable from a rule.

## The key realisation: we already have the event bus

`web_events_publish(const char *topic, const char *d_json)` (`web_core.cpp:734`, decl `web_server.h:35`) is
already the firmware's publish choke point, and it is called everywhere with exactly the trigger vocabulary a
rule engine wants:

```
notify   battery   net   settings.changed   sd.changed   theme
wifi.scan   ble.scan   nfc.tag   nfc.state   lora.rx   gnss.fix
recorder   audio   hid   ota   files.changed   link
presence / presence.csi / presence.alarm / presence.faro   airspace   sentinel   sniffer
```

Today it fans a `{"t":topic,"seq":n,"d":<json>}` frame out to the single WebSocket seat (`s_seat`). **When no
client holds the seat (`seat < 0`) the frame is built and dropped — nothing in firmware consumes it.** That
dropped path is the only gap between us and headless automation.

So we do **not** build a `nucleo_eventbus` clone. We add one in-process tap at that choke point.

## Firmware interface

### 1. In-process event tap (new — `web_events.*` or inside `web_core.cpp`)

```c
/* An in-process subscriber. Called from web_events_publish() on whatever thread published —
 * MUST be cheap and MUST NOT block or re-enter web_events_publish. */
typedef void (*web_event_sink_t)(const char *topic, const char *d_json, void *user);
void web_events_subscribe(web_event_sink_t cb, void *user);
```

At the end of `web_events_publish()`, after composing the frame, dispatch to the registered sink(s)
regardless of `s_seat`:

```c
    if (seat >= 0) web_transport_ws_send(seat, msg.data(), msg.size());
    web_events_dispatch_local(topic, d_json && *d_json ? d_json : "{}");   /* <-- added */
```

`web_events_publish` runs under `s_lock` and from several threads, so the sink must not do real work inline.

### 2. Automation module (redesigned `web_tool_automation.cpp` — no Berry)

- **Subscribes** one sink. The sink only **enqueues** `(topic, d_json)` into a small fixed ring buffer
  (drop-oldest on overflow; events are advisory). No matching, no I/O on the publish thread.
- **Drains** the ring in the existing `web_tool_t` `tick(now_ms)` / `second(now_ms)` callbacks, which run on
  the UI thread — the same thread the tools already use to touch hardware via `web_ui_run()`. Matching and
  action execution happen here, never on the publish thread.
- Cap `automation` re-registered in `web_tools.cpp` `TOOLS[]` (restore `&web_tool_automation`); the
  `FACTORY_AUTOMATION` gate in the TU is removed when the redesign replaces the Berry body.

### 3. Rule format (`/sd/automation/*.json`)

```jsonc
{
  "id": "low-batt-buzz",
  "enabled": true,
  "on": "battery",                 // event topic; "presence.*" prefix match allowed
  "if": { "level": { "lt": 20 }, "charging": false },   // optional predicates over the event's "d"
  "do": [
    { "notify": { "title": "Battery low", "body": "$d.level%%" } },
    { "term": "logs add battery-low" }
  ],
  "min_interval_ms": 300000        // per-rule debounce
}
```

- **`on`** — event topic (exact, or a `prefix.*` match).
- **`if`** — field predicates over the event's `d` object: `eq/ne/lt/le/gt/ge`, substring, exists. Absent =
  fire on every occurrence of the topic.
- **`do`** — ordered action list; the chain stops on the first failing action (reuse the DSL's exit-status
  idea). Action kinds, all reusing existing verbs:
  | kind | effect | reuses |
  |---|---|---|
  | `{"term": "..."}` | run one shell line, sandboxed to `/sd` + `/flash` | `term_exec` (cwd, exit status) |
  | `{"script": "name.scr"}` | run a Scripts DSL file | `script_run` (`web_tool_script.cpp`) |
  | `{"app": "<id>"}` | launch a native LVGL app | the app-open path (see `drive-native-sim-screen`) |
  | `{"notify": {...}}` | raise a notification | the notify tool |
  | `{"api": {"method","path","body"}}` | internal cap-gated call | the tool router + **P9 claim/auth gates** |
- **Budget** — each `do` chain is bounded by wall-clock and step count (reuse `SCRIPT_MS_MAX` shape from
  `web_tool_script.cpp`), so a rule cannot hang the UI thread.

### 4. Capability / authorization model

- *Editing / enabling* a rule is gated by the `automation` cap (session auth), like `/api/script/*`.
- *Dangerous actions* are gated by the **same P9 claim/authorization** the radio/attack tools already
  enforce — a rule's `{"api":...}` action goes through the normal router, so a rule can never do what an
  authenticated API call could not. No new privilege path.

### 5. API (mirror `/api/script/*`, `web_tool_script.cpp:770+`)

```
GET  /api/automation/list                 -> [{id, enabled, on, ...}]
GET  /api/automation/read?id=...          -> the rule JSON
POST /api/automation/save   {id, rule}    -> validate + persist to /sd/automation/<id>.json
POST /api/automation/delete {id}
POST /api/automation/test   {rule, event} -> dry-run a rule against a synthetic event (no side effects)
```

### 6. Front-ends (both planes, per house style)

- **Web Automation app** (JS, zero firmware flash — this is where the NucleoOS `www`/authoring idea lives):
  CRUD over `/api/automation/*`, a rule builder (pick topic → predicates → actions), enable/disable.
- **On-device LVGL app**: list rules, enable/disable, last-fired time. Reuse the Scripts app shape
  (`ui_script.cpp`). Authoring on-device can stay minimal (toggle + delete); creation is the web app's job.

## Testing (dual-plane is back)

Unlike Berry, the matcher is pure logic, so the **JS mock mirrors it** — mirror the ring/match/action
dispatch in the mock the way it already mirrors `script_run`/`term_exec`, and keep them in lock-step. Add:

- mock + sim parity on `/api/automation/{list,read,save,delete,test}`;
- a sim-only check that a rule fires **with no WebSocket client connected** (the whole point): publish a
  synthetic event via the tap and assert the action ran. This is the one behaviour the mock can't prove,
  because it has no firmware event bus.

## Build back-out already in place

`web_tool_automation.cpp` is gated by `#if FACTORY_WEB && defined(FACTORY_AUTOMATION)` and unregistered in
`TOOLS[]`. The redesign: replace the gated Berry body with the rule engine, drop the gate, restore
`&web_tool_automation`. Berry's `examples/factory/berry/` can then be deleted (separate commit) once the rule
engine lands and we are sure we don't want the interpreter back.

## Status

Design only. Next implementation step: the event tap (`web_events_subscribe` + `web_events_dispatch_local`)
+ a no-op sink, verified by a sim check that a disconnected event reaches the sink. Then the matcher, then
the API, then the two front-ends.
