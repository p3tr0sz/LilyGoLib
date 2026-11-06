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
- ESP-NOW Presence is now exposed honestly as `presence.peers` / **Nearby Peers**, with identified arrival/departure events.
- FIDO2 is simulator-only. Hardware does not advertise `fido2` and returns `503 fido_unavailable`; it never reports fake authentication success.
- Portal has working standalone and open-twin lifecycles, typed validation, gateway/DNS/DHCP checks, live clients, safe JSON/CSV output, hidden-password default, working redaction and clear-capture consent.
- Offensive starts require `authorized:true` after UI confirmation. Normal Wi-Fi disconnection and reconnect behaviour is stated to the operator.
- Deauth scans are asynchronous. Beacon generated/custom modes and real channel hopping work.

### Watch UI (`examples/factory/ui_nucleo_tools.cpp`)

- Replaced fake macro pages with real calls into the same tool lifecycle used by HTTP.
- Added channel/prefix/rate/template/target pickers, live counters and short lists, and readable error text.
- Added consent screens for Evil Portal, Deauth Flood and Beacon Spam.
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

Final successful runs:

- `pio run -e twatch_ultra` — **pass**
- `pio run -e emulator_watch_ultra` — **pass**
- `node web_api_test.mjs --dev-pin` — **231/231**
- `node web_api_test.mjs --url http://localhost:8080 --dev-pin` — **234/234**
- `node web_ui_test.mjs` — **138/138**
- `node web_ui_test.mjs --url http://localhost:8080` — **139/139**

The API tests cover PCAP bytes/radiotap fields, max-byte shutdown, sniffer filters/AP/EAPOL/handshake analysis, validation, radio conflicts, alerts, tracker persistence, FIDO unavailable behaviour, authorization, captive POST, redaction, deauth scans and custom beacon lists. Desktop tests click every P9 app, including sniffer live analysis, consent and credential deletion.

## Parity with `/Development/firmware` and `/Development/NucleoOs`

**Still not full parity.** Remaining material gaps:

- **Presence:** NucleoOS uses Wi-Fi CSI motion/presence sensing. This firmware offers ESP-NOW peer discovery only.
- **FIDO2:** NucleoOS has a real authenticator with user presence/PIN and credential protection. Here it is only a simulator fake; hardware capability is deliberately hidden.
- **Sniffer:** filtering, AP discovery and basic WPA four-way-handshake/EAPOL breakdown are implemented and simulator-tested. Real capture correctness, loss rates, channel hopping and encrypted/high-throughput traffic still need radio validation; no specialised reference firmware modes are claimed.
- **Evil Portal:** no KARMA probe capture, live page cloning, PSK verification, WPA2-coherent twin or custom SD template loading. These routes fail explicitly instead of pretending to work.
- **Offensive tools:** no reference-equivalent multi-target campaigns, injection-health/reconnect metrics or PMF-aware behaviour.
- **Radio/on-wrist validation:** actual injection, promiscuous capture, DHCP, reconnect timing, touch layout and outdoor readability remain unverified on hardware.

Do not restore “full parity” wording until those gaps are closed and tested on a real T-Watch Ultra.

## Important API notes

- Capability is `presence.peers`, not `presence`.
- Hardware omits `fido2` and all KARMA/clone capabilities.
- Portal start routes, Deauth start and Beacon start require `authorized:true`.
- Deauth status uses `targetBssid`, `targetStation`, `targetSsid` and `ratePps`.
- Sniffer/Airspace include `drops`; sniffer adds `filter`, `seen`, frame-type/EAPOL/handshake counters and `aps`; Sentinel includes `kind`, `separated`, `scanning` and `error`.
- Captive non-API requests are intercepted before static routing; API routes remain authenticated.
