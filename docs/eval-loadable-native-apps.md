# Evaluation — loadable native apps (the NucleoOS `.nap` model)

**DECISION (2026-11-08): NO-GO — do not build the loader.** The gate was measured (see the spike branch
`feat/native-apps-spike`: `docs/native-apps-flash-map.md`): the movable app-tail is **~175 KB**, well under
the ~300–400 KB threshold below, and ~147 KB of that is a single MP3/FLAC decoder; we have no resident
games/emulators. The LVGL-opaque ABI + a 3-segment `.nap` compile were spiked and *work*
(`docs/native-apps-abi-scope.md`, `native/`), so the subsystem is buildable — it just would not pay for
itself. Cheaper flash levers if ever needed: dedupe the ~346 KB of near-identical launcher icons, and
lazy-load/drop the ~147 KB audio decoder. Revisit only if a large offloadable app ecosystem (games,
emulators, third-party apps) becomes a real goal. The evaluation below stands as the rationale.

**Status: evaluation only — not a commitment to build.** This scopes what it would take for us to move a
*tail* of apps out of the firmware binary onto SD and load them on demand, the way NucleoOS does. Read
alongside `automation-rule-engine.md` (the cheap, near-term lever) — this is the big, systemic one.

## The model is a hybrid, not "everything on SD"

Core apps stay **resident** (compiled into firmware, instant, native speed): stopwatch, clock, settings, the
radio/security tools, terminal, scripts. Only a **tail** is **loadable** from SD — large and/or occasional
apps (games, emulators, one-off tools). This is exactly NucleoOS's split: built-in apps live in firmware;
store apps live in `/sd/apps/<id>/` and are loaded on demand. Their own rationale: *"the games alone are
~480 KB of [the firmware]"* — loadable apps are how that 480 KB leaves the binary and how new apps ship
without a firmware release.

So the question C answers is narrow: **is there enough offloadable tail in *our* flash to justify a dynamic
loader?** (See "Does it pay off for us" — this is the gate.)

## How NucleoOS does it (verified from their source)

A `.nap` is an **ELF32 Xtensa shared object**, `-fPIC`, linked by `sdk/native/nap.ld` into exactly three
PT_LOAD segments and nothing else:

| Segment | Flags | Holds | At run time |
|---|---|---|---|
| text | R+X | code, PLT stubs, literals, dynamic tables | a **flash scratch region**, on the instruction bus (XIP) |
| rodata | R | constants, strings | the same flash pages, data bus |
| data | R+W | GOT, init/fini arrays, `.data`, `.bss` | a **heap block** allocated at load |

Lifecycle (`nucleo_native/nap_loader.c` portable half + `nucleo_native.cpp` platform half, driven from
`store_apps.cpp`):

1. App switch → loader reads the `.nap` off SD, **validates everything** (ELF shape, every offset/size/table/
   reloc/import range-checked — it trusts nothing), relocates it (`R_XTENSA_RELATIVE/JMP_SLOT/GLOB_DAT`
   against a GOT), writes text+rodata to the flash scratch region and maps it, allocates the data block.
2. Imports are resolved **by name** against the firmware's **ABI export table** (`nap_exports.S`: a sorted
   `{name, addr}` array, binary-searched). The module imports libc/libm/libgcc, the app framework, drawing,
   input, fs, etc. The ABI table *is* the sandbox — a module can only call what the table exports.
3. The module exports `nucleo_native_info` (a `nap_info_t`: magic, ABI level, RAM needed, store id) and
   `nucleo_native_main` (called once after relocation; registers the app).
4. **One scratch region → one loadable app at a time.** Switching unloads the current module (free the
   heap block, re-point the launcher) before loading the next. Resident built-ins are re-attached, never
   reloaded. This is the "it looks like it installs the app on switch" behaviour observed on-device.

The discipline that keeps installed apps working across firmware updates — and it is substantial:

- **ABI levels only grow.** A name, once exported, keeps its signature forever; additions bump the level;
  the generator refuses a level that drops/reroutes a name; a module built for a higher level than the
  firmware is refused at install and load (`NAP_E_ABI`). An unknown name in the table is a firmware *build*
  error, never a user-device failure.
- **Layout fingerprint.** Inline code in a module bakes in the *shape* of shared firmware types (the GFX
  classes, the app-def struct, menu types) and the GFX library version. The module and firmware each hash
  these; a mismatch is refused at load (`NAP_E_LAYOUT`). Crucially: **when a firmware update changes the
  fingerprint, the store rebuilds every native app from its recipe and republishes them with the firmware.**

## What C would cost us

### 1. A toolchain + SDK (new, ongoing)
- Compile apps as `-fPIC` ELF32 Xtensa shared objects with a custom linker script (their `nap.ld` analog):
  3 segments, no more.
- A generator that turns an ABI spec into an assembly export table the firmware links (unknown symbol =
  firmware build error), plus module-side headers.
- A per-app build recipe so the catalog can be rebuilt on a layout bump.

### 2. A stable exported-symbol ABI — **the hard part for us, and worse than for NucleoOS**
- NucleoOS draws with **LovyanGFX** (concrete classes, inline methods). We draw with **LVGL**. LVGL's public
  surface is large, macro/inline-heavy, and its object structs evolve between versions. Every inline helper
  and struct field a module touches gets baked in and folded into the layout fingerprint.
- Consequence: either (a) export a **narrow, hand-picked, pointer-opaque** drawing/UI ABI (modules never see
  LVGL struct internals — only `lv_*` function pointers through the table), which limits what a loadable app
  can do but keeps the fingerprint stable; or (b) accept frequent fingerprint churn and a store that rebuilds
  the catalog on most LVGL bumps. (a) is the only sane choice and is real design work.

### 3. A flash scratch region + partition change
- A dedicated flash partition sized to the **largest** loadable app (only one resident at a time). Edit
  `partitions.csv`; verify ESP32-S3 XIP mmap of that partition to the instruction/data bus. RAM budget for
  the per-app data/.bss block.

### 4. A store / distribution path
- `/sd/apps/<id>/` layout, an install/uninstall/update engine, per-app data mounts, and a catalog host
  (NucleoOS has a whole separate store repo). Without this, "loadable" just means "manually copy ELFs to SD".

### 5. A trust model for running code off SD
- The loader hard-validates *structure*, and the ABI table bounds *what* a module can call — but a loaded
  module is still native code with the privileges of whatever the ABI exports. For store distribution we'd
  want **signing** (we already have `fwsig`-style firmware signing to build on). Loading unsigned native code
  from a user's SD is a trust decision to make explicitly, not inherit by accident.

## Does it pay off for us? (the gate — measure before committing)

NucleoOS's win was concrete: ~480 KB of **games** left the binary. **Our flash bulk is not games.** Before
any of the above is worth it we must know our resident-flash breakdown, because the offloadable set is only
the apps that are (a) not radio/system/realtime and (b) individually large:

- **Not offloadable (must stay resident):** Wi-Fi/BLE/LoRa stacks and the radio/security tools, the web
  server + desktop assets, LVGL core, drivers. If these dominate our 3.9 MB, C's ceiling is low.
- **Offloadable tail (candidates):** large optional UI apps / games / emulators / occasional tools — if we
  have enough of them to matter.

**Action before deciding:** generate a size map for `twatch_ultra`
(`.pio/build/twatch_ultra/firmware.elf`, e.g. `xtensa-esp32s3-elf-nm --size-sort --print-size` +
`idf_size` / the map file) and bucket it into {radio+system (fixed), framework (fixed), app tail
(movable)}. If the movable tail is, say, <300–400 KB, the dynamic loader + ABI + store + signing is not worth
it — keep everything resident. If a large, growing app tail is on the roadmap (games, emulators, a third-party
app ecosystem), C becomes the right systemic answer.

## Recommendation

1. **Do B first** regardless (`automation-rule-engine.md`) — cheap, headless, unblocks automation now.
2. **Gate C on a flash map.** Only pursue the `.nap` model if the measured movable tail justifies it *and*
   we actually want a third-party/offloadable app ecosystem. If it's just "trim our own flash once," cheaper
   non-loader options (prune features, `-Os`, drop unused assets, lazy-mount data) likely beat a whole
   dynamic-linker subsystem.
3. If pursued, the make-or-break design item is the **LVGL-opaque ABI** (narrow, pointer-only, versioned like
   their level/fingerprint scheme) — scope that before writing a loader.

## Licence note

NucleoOS is AGPL (firmware PolyForm-NC). This document is understanding of their *model* from reading their
source; any implementation here is clean-room, our own code — no NucleoOS files, names or structures copied,
consistent with the rest of the tree (`docs/web-dev.md`).
