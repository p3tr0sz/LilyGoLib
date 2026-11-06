/**
 * @file      web_tool_beaconspam.cpp
 * @brief     Beacon spam (NucleoOS parity): flood fake SSIDs into the airspace.
 *            Security audit tool — for testing network resilience with permission.
 *
 *            Injects fake 802.11 beacon frames with generated SSIDs to flood the
 *            airspace with visible networks. Each beacon announces a unique SSID
 *            based on a configurable prefix.
 *
 *            The implementation uses esp_wifi 802.11 raw frame injection to broadcast
 *            beacon frames with randomized BSSIDs.
 *
 *            WARNING: Only use in controlled environments you own.
 *            Flooding the airspace with beacons is disruptive and may be illegal
 *            in some jurisdictions without authorization.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <mutex>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

/* ---------------------------------------------------------------- state */

static bool s_active = false;
static int s_channel = 1;
static int s_mode = 0;              /* 0=random prefix, 1=from list, 2=custom */
static uint32_t s_beacon_count = 0;
static uint32_t s_ssid_count = 0;
static char s_prefix_buf[24] = "FreeNet";
static const char *s_prefix = s_prefix_buf;
static long long s_started = 0;

static uint32_t s_last_send_ms = 0;
static uint32_t s_packets_per_tick = 2;
static uint32_t s_ssid_index = 0;

/* Custom SSID list */
static char s_custom_ssids[64][33];
static int s_custom_count = 0;
static std::mutex s_custom_mx;

/* ---------------------------------------------------------------- beacon frame builder */

/*
 * 802.11 beacon frame (type 0, subtype 8)
 * Bytes:
 *   0-1: Frame Control (0x80 0x00 = beacon)
 *   2-3: Duration
 *   4-9: Destination (broadcast: FF:FF:FF:FF:FF:FF)
 *  10-15: Source address (random BSSID)
 *  16-21: BSSID
 *  22-23: Sequence control
 *  24-31: Timestamp (8 bytes)
 *  32-33: Beacon interval (typically 100 = ~102.4ms)
 *  34-35: Capabilities
 *  36+: Tagged parameters
 *    tag 0: SSID
 *    tag 1: Supported rates
 *    tag 3: Channel
 */

#define BEACON_MIN_SIZE 50
#define MAX_SSID_LEN 32

#ifdef ARDUINO
static uint8_t s_seq = 0;

static void build_beacon(uint8_t *frame, size_t *len, const char *ssid, const uint8_t *bssid, int channel)
{
    memset(frame, 0, BEACON_MIN_SIZE);
    frame[0] = 0x80;   /* type=0 (management), subtype=8 (beacon) */
    frame[1] = 0x00;
    frame[2] = 0x00;   /* duration */
    frame[3] = 0x00;
    /* Destination: broadcast */
    frame[4] = 0xFF; frame[5] = 0xFF; frame[6] = 0xFF;
    frame[7] = 0xFF; frame[8] = 0xFF; frame[9] = 0xFF;
    /* Source (BSSID) */
    memcpy(frame + 10, bssid, 6);
    /* BSSID */
    memcpy(frame + 16, bssid, 6);
    /* Sequence */
    frame[22] = s_seq++; frame[23] = 0;
    /* Timestamp (8 bytes — just use 0 for spam) */
    memset(frame + 24, 0, 8);
    /* Beacon interval: 100 (0x64, 0x00) = ~102.4ms */
    frame[32] = 0x64; frame[33] = 0x00;
    /* Capabilities: ESS (bit 0) */
    frame[34] = 0x01; frame[35] = 0x00;

    /* Tagged parameters start at offset 36 */
    size_t off = 36;

    /* Tag 0: SSID */
    int ssid_len = strlen(ssid);
    if (ssid_len > MAX_SSID_LEN) ssid_len = MAX_SSID_LEN;
    frame[off++] = 0;                           /* tag: SSID */
    frame[off++] = (uint8_t)ssid_len;           /* length */
    memcpy(frame + off, ssid, ssid_len);
    off += ssid_len;

    /* Tag 1: Supported rates */
    {
        frame[off++] = 1;                       /* tag: Supported Rates */
        frame[off++] = 4;                       /* length */
        frame[off++] = 0x82;                    /* 12 Mbps (b) */
        frame[off++] = 0x84;                    /* 24 Mbps (b) */
        frame[off++] = 0x8B;                    /* 48 Mbps (b) */
        frame[off++] = 0x96;                    /* 108 Mbps (b) */
    }

    /* Tag 3: Channel (for non-802.11b/g) */
    if (channel > 0) {
        frame[off++] = 3;                       /* tag: Channel */
        frame[off++] = 1;                       /* length */
        frame[off++] = (uint8_t)channel;
    }

    *len = off;
}
#endif

/* Generate a fake BSSID from an index */
static void bssid_from_index(uint8_t *bssid, uint32_t idx)
{
    bssid[0] = 0x02;
    bssid[1] = 0x00;
    bssid[2] = 0x00;
    bssid[3] = (uint8_t)((idx >> 16) & 0xFF);
    bssid[4] = (uint8_t)((idx >> 8) & 0xFF);
    bssid[5] = (uint8_t)(idx & 0xFF);
}

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Beacon Spam", NULL };

static void status_json(web_req_t *r)
{
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false");
    if (s_active) {
        j += ",\"channel\":" + tools_int(s_channel) +
            ",\"beacons\":" + tools_int(s_beacon_count) +
            ",\"ssids\":" + tools_int(s_ssid_count) +
            ",\"prefix\":"; web_json_str(j, s_prefix);
        j += ",\"mode\":" + tools_int(s_mode) +
            ",\"started\":" + tools_int(s_started);
    }
    j += ",\"customSsids\":[";
    { std::lock_guard<std::mutex> lk(s_custom_mx);
      for (int i = 0; i < s_custom_count; i++) {
        if (i) j += ",";
        web_json_str(j, s_custom_ssids[i]);
      }
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void start_spam(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, prefix;
    double ch = 0, count = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "prefix", prefix);
    web_json_get_num(body, "channel", &ch);
    web_json_get_num(body, "count", &count);

    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }
    if (prefix.empty()) prefix = "FreeNet";
    if (prefix.size() > 20) { web_resp_error(r, 400, "bad_request", "prefix too long (max 20)"); return; }

    s_active = true;
    s_channel = (int)ch > 0 ? (int)ch : 0;  /* 0 = current channel */
    s_ssid_count = (uint32_t)count > 0 ? (uint32_t)count : 20;
    snprintf(s_prefix_buf, sizeof(s_prefix_buf), "%s", prefix.c_str());
    s_beacon_count = 0;
    s_ssid_index = 0;
    s_started = tools_epoch();
    s_last_send_ms = 0;

#ifdef ARDUINO
    /* Disconnect from any AP to free the Wi-Fi interface */
    WiFi.disconnect(false);
    if (s_channel > 0) esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_enable(0);  /* Can't recall if needed, but doesn't hurt */
#endif

    web_resp_ok(r);
}

static void stop_spam(web_req_t *r)
{
    if (!s_active) { web_resp_ok(r); return; }
    s_active = false;

#ifdef ARDUINO
    /* Reconnection handled by web core */
#endif

    web_resp_ok(r);
}

/* ── Custom SSID management ── */

static void set_custom_ssids(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;
    std::lock_guard<std::mutex> lk(s_custom_mx);
    s_custom_count = 0;
    /* Parse JSON array of strings. Simple parser for ["a","b","c"] */
    const char *p = body.c_str();
    while (*p && *p != '[') p++;
    if (!*p) { web_resp_ok(r); return; }
    p++; /* skip '[' */
    while (*p && s_custom_count < 64) {
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++; /* skip opening '"' */
        char *dst = s_custom_ssids[s_custom_count];
        int n = 0;
        while (*p && *p != '"' && n < 32) { *dst++ = *p++; n++; }
        *dst = 0;
        if (n > 0) s_custom_count++;
        if (*p == '"') p++;
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == ']') {
            if (*p == ']') { p = ""; break; }
            p++;
        }
    }
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/beaconspam") && strncmp(r->path, "/api/beaconspam/", 16)) return false;
    if (tools_is(r, "GET", "/api/beaconspam")) status_json(r);
    else if (tools_is(r, "POST", "/api/beaconspam/start")) start_spam(r);
    else if (tools_is(r, "POST", "/api/beaconspam/stop")) stop_spam(r);
    else if (tools_is(r, "PUT", "/api/beaconspam/ssids")) set_custom_ssids(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/beaconspam")) {
        s_active = true;
        s_beacon_count = 0;
        std::string body;
        web_read_body(r, body, 256);
        (void)body;
        web_resp_ok(r);
        return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void beacon_tick(uint32_t now_ms)
{
    if (!s_active) return;

#ifdef ARDUINO
    if (now_ms - s_last_send_ms < 100) return;  /* ~10 Hz */
    s_last_send_ms = now_ms;

    uint8_t frame[128];
    size_t len = 0;

    for (uint32_t i = 0; i < s_packets_per_tick; i++) {
        /* Generate SSID */
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "%s%04u", s_prefix, s_ssid_index);

        /* Generate BSSID */
        uint8_t bssid[6];
        bssid_from_index(bssid, s_ssid_index);

        /* Set channel for this beacon */
        int ch = s_channel > 0 ? s_channel : (int)(s_ssid_index % 11) + 1;

        build_beacon(frame, &len, ssid, bssid, ch);
        if (len > 0) {
            esp_wifi_80211_tx(frame, len, 0);
            s_beacon_count++;
        }

        s_ssid_index++;
        if (s_ssid_index >= s_ssid_count) {
            s_ssid_index = 0;  /* Cycle through SSIDs */
        }
    }
#else
    /* Simulator: just count */
    (void)now_ms;
    s_beacon_count += s_packets_per_tick;
#endif
}

static void beacon_stop(void)
{
    s_active = false;
}

static const char *const CAPS[] = { "beacon.spam", NULL };
const web_tool_t web_tool_beaconspam = { CAPS, handle, beacon_tick, NULL, beacon_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_beaconspam = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */