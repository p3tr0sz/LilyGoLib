# Security Tools — Handoff

## What was done (6 commits on `dev`, pushed to origin)

### Commits
1. `03954b5` — Added "Security" filter chip to app list UI
2. `d7be26c` — Assigned all 8 P9 tools to Security category
3. `a941471` — Evil Portal: 3 modes, 4 templates, SSID presets, consent screen, web app
4. `db7c22a` — Deauth Flood target scan, Beacon Spam custom SSID lists
5. `282fcac` — Rewrote all 8 web desktop apps with proper configuration UIs
6. `3bd7c0d` — Evil-twin deauth injection, User-Agent fingerprinting, SD credential logging

### Files changed
- `examples/factory/ui_main.cpp` — Security category mapping
- `examples/factory/ui_app_list.cpp` — Security chip in filter bar
- `examples/factory/ui_nucleo_tools.cpp` — Evil Portal on-device UI (consent + setup)
- `examples/factory/web_internal.h` — `web_req_t.user_agent` field, `web_resp_html()` declaration
- `examples/factory/web_core.cpp` — `web_resp_html()` implementation
- `examples/factory/web_tools.h` — Extended all P9 hardware interfaces
- `examples/factory/web_transport_esp.cpp` — User-Agent header capture
- `examples/factory/web_transport_posix.cpp` — User-Agent header capture (simulator)
- `examples/factory/web_tool_evilportal.cpp` — Full rewrite: 3 modes, templates, presets, fingerprinting, deauth, SD logging
- `examples/factory/web_tool_deauthflood.cpp` — Target AP scan + list endpoints
- `examples/factory/web_tool_beaconspam.cpp` — Custom SSID list management
- `examples/factory/web/js/api.js` — All new API client methods
- `examples/factory/web/apps/evilportal/app.js` — Full config UI
- `examples/factory/web/apps/deauthflood/app.js` — Scan + picker UI
- `examples/factory/web/apps/beaconspam/app.js` — Custom SSIDs UI
- `examples/factory/web/apps/sniffer/app.js` — Enhanced UI
- `examples/factory/web/apps/airspace/app.js` — Attack detection UI
- `examples/factory/web/apps/sentinel/app.js` — BLE tag list UI
- `examples/factory/web/apps/presence/app.js` — Peer discovery UI
- `examples/factory/web/apps/fido2/app.js` — Credential management UI

### Architecture notes

The codebase does NOT use the `web_hw_*()` hardware abstraction layer declared in `web_tools.h` for P9 tools. Those declarations exist but are never implemented. Instead, each `web_tool_*.cpp` calls ESP-IDF APIs directly (WiFi.softAG(), esp_wifi_80211_tx(), esp_wifi_set_promiscuous(), etc.). This is the existing pattern — follow it for new hardware code.

## What still needs implementing

### 1. KARMA probe sniffing (`web_tool_evilportal.cpp`)
Promiscuous mode callback that parses 802.11 probe request frames (type 0, subtype 4) to discover SSIDs nearby devices are searching for. The `karma_scan()` function currently does nothing.

**File:** `examples/factory/web_tool_evilportal.cpp` — `karma_scan()` function (~line 580)
**Reference:** `web_tool_airspace.cpp` has a working promiscuous callback + frame parser you can copy
**Pattern:**
```cpp
static void karma_promisc_cb(uint8_t *buf, wifi_promiscuous_pkt_t *pkt) {
    if (pkt->payload_length < 26) return;
    uint8_t fc0 = pkt->payload[0];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (type != 0 || subtype != 4) return; // probe request
    // Extract SSID from tagged parameters (offset 24 for probe request body)
    // Store in s_karma_ssids[], increment s_karma_count
}
```

### 2. Live page cloning (`web_tool_evilportal.cpp`)
Connect to an open AP, make an HTTP GET request to any URL (captive portal should intercept), save the response body as a template.

**File:** `examples/factory/web_tool_evilportal.cpp` — `clone_page()` function (~line 610)
**Steps:**
1. `WiFi.begin(open_ssid)` — connect without password
2. Wait for IP with timeout (~10s)
3. Create a TCP connection to any IP on port 80
4. Send `GET / HTTP/1.1\r\nHost: example.com\r\n\r\n`
5. Read response, strip HTTP headers
6. Save body to `/sd/evilportal/portals/cloned_<ssid>.html`
7. `WiFi.disconnect()`, restore previous network

### 3. PSK verification (`web_tool_evilportal.cpp`)
When `s_verify` is true and twin mode is active, connect to the real AP with captured credentials to verify them.

**File:** `examples/factory/web_tool_evilportal.cpp` — add to `handle_captive_request()` after capture
**Steps:**
1. `WiFi.begin(s_ssid, cap.password)` 
2. Wait for connection with timeout (~8s)
3. If connected → `s_verified_ok++`, else `s_verified_bad++`
4. Disconnect and restore twin AP

### 4. DHCP diagnostics (`web_tool_evilportal.cpp`)
Detect whether the softAP DHCP server started correctly. Currently `s_dhcps_ok` is always true.

**File:** `examples/factory/web_tool_evilportal.cpp` — `start_standalone()`, `start_twin()`, `start_karma()`
**Approach:** Check `WiFi.softAPgetStationNum()` after ~3 seconds — if 0 clients connected and 30+ seconds pased, DHCP may have failed. Or check `WiFi.softAPgetDhcpServerStatus()` if available.

### 5. Coherent evil-twin (`web_tool_evilportal.cpp`)
When `coherent=true`, clone the WPA2 security parameters in beacon frames. Currently only OPEN AP mode is supported.

**File:** `examples/factory/web_tool_evilportal.cpp` — `start_twin()`
**Steps:**
1. Pass `authmode` parameter to `start_twin` (currently only ssid/bssid/channel)
2. When coherent, set `wifi_auth_mode_t` on softAP to match real AP
3. This means clients can't connect without the PSK — but the beacon looks identical
4. Add `authmode` parameter to `/api/evilportal/start-twin` and the web app

### 6. Debug logging (`web_tool_evilportal.cpp`)
When `s_debug` is true, write diagnostics to `/sd/evilportal/ep-debug.log` (no credentials, only heap, DHPC state, SSID).

**File:** `examples/factory/web_tool_evilportal.cpp`
**Pattern:** `fprint(f, "timestamp, free_heap: %d, dhcps_ok: %d, ssid: %s\n", ...)` on start and periodically

### 7. On-device UIs for remaining 7 tools (`ui_nucleo_tools.cpp`)
All tools except Evil Portal use the `NUCLEO_APP` macro (just Start/Stop buttons). Each needs:
- Deauth Flood: AP scan + picker (like evil portal's SSID cycler)
- Beacon Spam: prefix input, custom SSID textarea
- Sniffer: channel selector
- Airspace: live deauth/beacon counts
- Sentinel: detected tag list
- Presence: peer list
- FIDO2: credential list

**File:** `examples/factory/ui_nucleo_tools.cpp`
**Pattern:** Follow `enter_evilportal()` / `ep_rebuild_setup()` for the two-phase approach (consent → setup)

### SD card template loading (`web_tool_evilportal.cpp`)
Load custom .html templates from `/sd/evilportal/portals/` and add them to the template list. Currently only built-in templates are in `BUILTIN_TEMPLATES[]`.

**File:** `examples/factory/web_tool_evilportal.cpp` — `status_json()`, `start_*()`
**Steps:** Scan `/sd/evilportal/portals/*.html`, add entries with `builtin: false` to the templates array in status, serve from SD when selected.

## How to compile and test

```bash
# Simulator (native)
cd /Users/piotrbartkiewicz/Development/LilyGoLib-PlatformIO
pio run -e emulator_watch_ultra          # compile
pio run -e emulator_watch_ultra -t exec  # run (interactive, opens GUI)

# ESP32 (T-Watch Ultra)
pio run -e twatch_ultra                   # compile only
pio run -e twatch_ultra -t upload         # flash

# Run API tests against simulator
cd /Users/piotrbartkiewicz/Development/LilyGoLib/examples/factory/tests
node web_api_test.mjs                    # all API tests
node web_ui_test.mjs                     # desktop UI tests
```

## API endpoints added

### Evil Portal
```
GET  /api/evilportal                — full status + templates + presets + settings
POST /api/evilportal/start         — standalone (ssid, templateId, gateway)
POST /api/evilportal/start-twin    — evil-twin (ssid, bssid, channel, coherent, templateId, gateway)
POST /api/evilportal/start-karma   — karma (templateId, gateway)
POST /api/evilportal/stop          — stop portal
GET  /api/evilportal/captures      — captured credentials (redacted per pwdMode)
POST /api/evilportal/clear         — clear captures
PUT  /api/evilportal/settings      — update pwdMode, verify, gateway, debug, autoStopBase/Extended
POST /api/evilportal/karma-can     — scan for probe requests (seconds)
GET  /api/evilportal/karma         — list discovered probes
POST /api/evilportal/clone         — clone page from open AP (ssid)
POST /api/_dev/evilportal          — simulator test hook
```

### Deauth Flood
```
POST /api/deauthflood/scan         — scan for nearby APs
GET  /api/deauthflood/targets      — list discovered targets
```

### Beacon Spam
```
PUT  /api/beacospam/ssids          — set custom SSID list
```

## Key design decisions

1. **No hardware abstraction layer for P9.** The `web_hw_*()` declarations in `web_tools.h` are unused. Tools call ESP-IDF directly. Don't fight this pattern.

2. **Captive portal runs on the web API server.** When evil portal is active, `handle()` intercepts all non-`/api/evilportal` requests and serves captive HTML. The DNS server redirects all domains to the softAP IP. This is simpler than running a separate HTTP server.

3. **Simulator test hooks use `/api/_dev/*`.** All P9 tools have simulator injection endpoints. Use them to test without real hardware.

4. **Web desktop lives on SD card.** Don't add to firmware flash — the firmware only has the rescue page. Desktop is in `examplesefactory/web/` and gets gzipped by `tools/web_build.py`.

5. **Commit granularity.** Stay focused: implement → test-compile → commit one feature at a time. Push after each commit.