# Security Tools — Audit Remediation Handoff

## Status

The original P9 commits were **not at parity and did not build for T-Watch Ultra**. The follow-up audit repaired the implementation, contracts, native watch controls, desktop apps, simulator fakes and tests.

Current state: **builds pass, simulator and mock regression suites pass. Real-radio and on-wrist behaviour still needs hardware validation.**

## What the audit found

- P9 apps were absent from the web launchpad registry and had no P9 UI tests.
- Every P9 `/api/_dev/*` hook was unreachable behind its route guard.
- All native Start/Stop buttons except the portal merely changed labels; no tool started.
- Promiscuous callback and raw-TX ABIs were wrong; one API no longer existed; `<mutex>` was missing.
- Static capture queues overflowed DRAM once compilation was fixed.
- Sniffer could dereference a null request at its byte limit, used the wrong filesystem path and wrote malformed radiotap headers.
- Airspace discarded simulator alerts and used different JSON names from its desktop.
- Sentinel treated any nearby BLE device as a stalking tag after a few seconds and used active scans while claiming to be passive.
- Presence implemented ESP-NOW peer discovery, not NucleoOS's Wi-Fi CSI human-motion detector.
- FIDO2 returned success on hardware without CTAP, credentials, crypto or a FIDO transport.
- Portal numeric/boolean JSON settings were read as strings; KARMA, cloning, PSK verification and coherent twin returned fake success.
- Captive interception ran after static routing and could not serve the portal to unpaired clients.
- Custom beacon SSIDs were unused; channel hopping changed tags but not the radio channel; deauth rate/validation was wrong.
- Desktop list rendering passed arrays directly to `replaceChildren`, and initial-load/action-load races left stale state.

## Remediation delivered

### Shared/backend

- Added one Wi-Fi owner for Sniffer, Airspace, Evil Portal, Deauth Flood and Beacon Spam. Conflicting starts return `503 busy` naming the tool to stop.
- Corrected ESP-IDF callback and injection ABIs. Injection counters increment only on accepted frames; failures are reported.
- Reduced static queue memory; the watch links at about **48.4% RAM / 90.0% flash**.
- Sniffer now resolves `/sd` paths through `web_fs_resolve`, creates parent directories, writes valid PCAP/radiotap data, reports drops/errors, and stops safely at `max_bytes`. It now also offers typed PCAP filters (`all`, `management`, `data`, `beacons`, `probes`, `eapol`), observed-vs-saved counters, an AP table, and EAPOL/four-way-handshake detection in the API, desktop and watch UI.
- Airspace reports deauth/beacon/flood/twin/broadcast counters and emits detailed events.
- Sentinel uses passive scans, classifies Find My/SmartTag/Tile/Chipolo, and requires a persistence window. “Persistent” is explicitly described as a heuristic, not proof of stalking.
- Presence now combines Wi-Fi CSI motion/presence sensing (`presence.csi`) with the existing ESP-NOW Nearby Peers (`presence.peers`). CSI uses AP-sourced L-LTF amplitudes, AGC-independent profile correlation, 32-frame calibration, a normalized motion score and a 10-second presence hold. It includes radio ownership, gateway traffic generation, power-save restoration, reconnect/recalibration handling, native/desktop controls, state events and deterministic synthetic-profile tests.
- FIDO2 is simulator-only. Hardware does not advertise `fido2` and returns `503 fido_unavailable`; it never reports fake authentication success.
- Portal has working standalone and open-twin lifecycles, typed validation, gateway/DNS/DHCP checks, live clients, safe JSON/CSV output, hidden-password default, working redaction and clear-capture consent.
- P9.2 core adds persistent custom templates from `/sd/data/evilportal/portals/`: safe filenames, 64 KiB limit, required POST `/login` fields, no scripts/external resources, revalidation on selection and explicit malformed-file reporting. Passive KARMA discovery hops channels 1–13, aggregates directed-probe SSIDs/hits/unique clients/RSSI/channel and starts an open AP for a selected or most-seen SSID. Dynamic per-probe responses are not claimed.
- P9.3 core adds asynchronous live cloning for authorized open lab APs. Responses are bounded to 64 KiB, accepted only if they already pass the safe static-template rules, written atomically as `cloned.html`, and reloaded into the SD catalog. WPA2-coherent twins require the operator-known lab PSK and never silently fall back to open mode. Captured PSKs are checked one at a time against only the selected BSSID/channel with per-capture `queued/checking/valid/invalid/error` states, timeout handling, redaction preserved, and saved station configuration restored after checks and teardown.
- Offensive starts require `authorized:true` after UI confirmation. Normal Wi-Fi disconnection and reconnect behaviour is stated to the operator.
- Deauth scans are asynchronous. Beacon generated/custom modes and real channel hopping work.
- P9.4 core adds finite campaigns: up to 8 scanned deauth targets and 64 generated/custom beacon SSIDs, each with independent accepted/failed counters, goal progress and elapsed time. Repeated injection failures skip only the current target. Deauth WPA3/WPA2-transition targets end as `pmf_limited`, explicitly not guaranteed disconnects. Both tools auto-stop at campaign completion and expose reconnect attempts, duration and outcome; hardware retries three times over 20 seconds.
- P9.5 core adds an optional 100 Hz ESP-NOW Faro transmitter (`TWF1` + counter), source-MAC discovery and a second independent CSI detector. Router/Faro zones calibrate and hold presence separately; silent Faro links expire after 3 seconds. The alarm has exit delay, 1.5-second settle grace, per-zone motion trigger, vibration/beeps, manual disarm and automatic timeout; timing settings persist. A 120 × 500 ms motion graph is bounded to 60 seconds and intentionally runtime-only. Native and desktop controls expose all modes, including accepted/error transmitter counters.

### Watch UI (`examples/factory/ui_nucleo_tools.cpp`)

- Replaced fake macro pages with real calls into the same tool lifecycle used by HTTP.
- Added channel/prefix/rate/template/target pickers, live counters and short lists, and readable error text.
- Added on-watch text editing: Evil Portal accepts a manual SSID and Beacon Spam accepts a manual generated-SSID prefix; either overrides its preset when non-empty.
- Evil Portal lists built-in and validated SD templates, clones safe lab pages, configures open/WPA2 twins with target BSSID/channel and optional PSK verification, runs a 10-second passive probe scan, shows discovered SSIDs/hits, and starts KARMA core using the most-seen result.
- Added consent screens for Evil Portal, Deauth Flood and Beacon Spam.
- Added native finite-campaign actions, campaign progress, accepted/failed counters and reconnect summaries. Deauth explicitly warns that every one of the first 8 scanned campaign targets must be authorized.
- Closing a radio test stops it and requests Wi-Fi reconnect.
- FIDO2 shows an explicit unavailable explanation and no misleading controls.
- Long consent/error text wraps instead of being truncated into card rows.

### Web desktop/accessibility

- Registered all supported P9 apps in `web/js/shell.js`.
- Fixed API field names, list rendering and latest-request-wins loading in all P9 apps and Link.
- Added consent dialogs, destructive-action confirmation, visible errors, target buttons and unavailable states.
- Errors are dismissible instead of disappearing on a timer.
- Removed whole-app live-region chatter, added toast live region metadata, dialog labels/focus trapping and reduced-motion CSS.
- Desktop status uses text as well as colour.

## Verification

Final successful runs after P9.5 core:

- `pio run -e twatch_ultra` — **pass** (RAM 55.0%, flash 91.8%)
- `pio run -e emulator_watch_ultra` — **pass**
- `node web_api_test.mjs --dev-pin` — **310/310**
- `node web_api_test.mjs --url http://localhost:8080 --dev-pin` — **314/314**
- `node web_ui_test.mjs` — **170/170**
- `node web_ui_test.mjs --url http://localhost:8080` — **171/171**

The API tests cover PCAP bytes/radiotap fields, max-byte shutdown, sniffer filters/AP/EAPOL/handshake analysis, CSI calibration/motion/presence hold/recalibration/events/radio conflicts, independent Faro calibration/motion/holds and TTL, alarm validation/exit/grace/zone trigger/manual disarm/timeout/events, bounded 60-second history, Faro transmitter validation/lifecycle/counters, alerts, tracker persistence, FIDO unavailable behaviour, authorization, custom SD template validation/serving, safe clone success/failure, WPA2 PSK validation, per-capture valid/invalid verification with redaction, passive probe aggregation, selected-SSID KARMA startup, captive POST, deauth scans, independent campaign failure/completion, aggregate/per-target counter consistency, PMF-limited outcomes, reconnect metrics and custom beacon lists. Desktop tests click every P9 app, including sniffer live analysis, CSI stable/motion/present states, independent Faro motion, graph rendering, alarm arm/trigger/timeout/disarm, Faro transmitter start/stop, custom template selection, asynchronous cloning, WPA2 twin startup, live PSK verification, probe-discovery consent/results/KARMA startup, multi-target campaign progress and reconnect health.

## Parity with `/Development/firmware` and `/Development/NucleoOs`

**Still not full parity.** Remaining material gaps:

- **Presence:** dual-zone CSI, optional Faro transmitter, alarm and bounded history are implemented and simulator-tested. Real-radio accuracy, actual 100 Hz Faro delivery/CSI frame rates, two-device same-channel operation, ESP-NOW coexistence, reconnects, alarm audibility and environmental false positives remain unvalidated.
- **FIDO2:** NucleoOS has a real authenticator with user presence/PIN and credential protection. Here it is only a simulator fake; hardware capability is deliberately hidden.
- **Sniffer:** filtering, AP discovery and basic WPA four-way-handshake/EAPOL breakdown are implemented and simulator-tested. Real capture correctness, loss rates, channel hopping and encrypted/high-throughput traffic still need radio validation; no specialised reference firmware modes are claimed.
- **Evil Portal:** custom SD templates, safe-page cloning, per-capture PSK verification, open/WPA2 twins and passive probe discovery/selected-SSID KARMA core are implemented and simulator-tested. Real authorized clone traffic, correct/incorrect PSK association, WPA2 client association, reconnect behavior and probe traffic still need validation. Dynamic per-probe KARMA responses remain unavailable. Cloning deliberately rejects pages that are not already safe self-contained templates; it is not a general active-site copier.
- **Offensive tools:** bounded multi-target campaigns, injection-health/reconnect metrics and PMF-limited reporting are implemented and simulator-tested. Real accepted-versus-observed frame rates, independent multi-target behavior, reconnect timing and PMF client outcomes still require authorized lab captures.
- **Radio/on-wrist validation:** actual injection, promiscuous capture, DHCP, reconnect timing, touch layout and outdoor readability remain unverified on hardware.

Do not restore “full parity” wording until those gaps are closed and tested on a real T-Watch Ultra.

## Important API notes

- Capabilities are `presence.csi` for router/Faro motion/presence, `presence.faro` for the optional second-watch transmitter, `presence.alarm` for the alarm lifecycle, `presence.history` for the bounded runtime graph and `presence.peers` for ESP-NOW discovery. The root peer-discovery fields and routes remain compatible; CSI status, zones and graph are nested under `csi`, transmitter state under `faro`, and alarm state under `alarm` in `GET /api/presence`.
- Hardware omits `fido2`. `evil.portal.clone`, `evil.portal.verify` and `evil.portal.coherentTwin` advertise implemented contracts, not completed radio validation. `evil.portal.karma` advertises passive discovery + selected-SSID AP startup only; dynamic per-probe response is not implied.
- Portal start routes, Deauth/Beacon continuous and campaign starts require `authorized:true`.
- Deauth status uses `targetBssid`, `targetStation`, `targetSsid` and `ratePps`. Campaign capabilities are `deauth.campaign`, `deauth.health`, `beacon.campaign` and `beacon.health`; these advertise implemented contracts, not observed-frame validation.
- Sniffer/Airspace include `drops`; sniffer adds `filter`, `seen`, frame-type/EAPOL/handshake counters and `aps`; Sentinel includes `kind`, `separated`, `scanning` and `error`.
- Captive non-API requests are intercepted before static routing; API routes remain authenticated.
