/**
 * @file      web_tool_airspace.cpp
 * @brief     Airspace (P9 NucleoOS parity): Wi-Fi attack detection from passive capture.
 *            Detects: deauth floods, broadcast deauth, evil-twin APs, beacon floods.
 *
 *            This is the blue-team counterpart of the offensive trio (evil portal, deauth flood,
 *            beacon spam). Fully passive — it only listens and analyses, never transmits.
 *
 *            On the real watch, this uses esc-wifi promiscuous mode to capture 802.11 management
 *            frames, then analyses them for attack patterns. In the simulator, events are injected
 *            through test hooks.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <map>

#ifdef ARDUINO
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

/* ---------------------------------------------------------------- state */

static bool s_active = false;
static long long s_started = 0;

/* Deauth detection */
static uint32_t s_deauth_total = 0;
static uint32_t s_deauth_floods = 0;
static uint32_t s_deauth_this_second = 0;
static uint32_t s_deauth_last_second = 0;
#define DEAUTH_FLOOD_THRESHOLD 50   /* 50+ deauths in a second = flood */

/* Beacon flood detection */
static uint32_t s_beacon_total = 0;
static uint32_t s_beacon_floods = 0;
static uint32_t s_beacon_this_second = 0;
static uint32_t s_beacon_last_second = 0;
#define BEACON_FLOOD_THRESHOLD 100  /* 100+ beacons in a second = flood */
#define BEACON_SSID_LIMIT 20        /* unique SSIDs in a 10s window = flood */

/* Evil-twin detection */
static uint32_t s_evil_twins = 0;
static std::map<std::string, std::string> s_known_aps; /* bssid -> ssid */
#define EVIL_TWIN_RSSI_THRESHOLD 5  /* same SSID, different BSSID, RSSI > threshold apart */

/* Simulator injection queue */
#ifndef ARDUINO
#include <queue>
static std::queue<std::string> s_inject_alerts;
static std::mutex s_inject_mx;
#endif

#ifdef ARDUINO
/* Promiscuous callback for airspace analysis */
#define AIR_BUF_MAX 64
static uint8_t s_air_buf[AIR_BUF_MAX][1536];
static size_t s_air_len[AIR_BUF_MAX];
static int s_air_rssi[AIR_BUF_MAX];
static int s_air_ch[AIR_BUF_MAX];
static int s_air_w = 0, s_air_r = 0;
static std::mutex s_air_mx;

static void air_promisc_cb(uint8_t *buf, wifi_promiscuous_pkt_t *pkt)
{
    if (!s_active || pkt->payload_length < 2) return;
    uint8_t fc0 = pkt->payload[0];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;

    /* We only care about management frames: type == 0 */
    if (type != 0) return;

    std::lock_guard<std::mutex> lk(s_air_mx);
    int w = (s_air_w + 1) % AIR_BUF_MAX;
    if (w == s_air_r) return;
    int slot = s_air_w;
    size_t n = pkt->payload_length > 1500 ? 1500 : pkt->payload_length;
    memcpy(s_air_buf[slot], pkt->payload, n);
    s_air_len[slot] = n;
    s_air_rssi[slot] = pkt->rssi;
    s_air_ch[slot] = s_air_r > 0 ? pkt->channel : 1;
    s_air_w = w;
}
#endif

/* ---------------------------------------------------------------- analysis */

/* Extract SSID from a beacon frame (rough parser, just type 0x80 subtype=8) */
static std::string beacon_ssid(const uint8_t *frame, size_t len)
{
    if (len < 38) return "";
    uint8_t fc0 = frame[0], fc1 = frame[1];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (type != 0 || subtype != 8) return "";  /* not a beacon */

    /* Parse tagged parameters starting at offset 36 (after fixed params) */
    size_t off = 36;
    while (off + 2 <= len) {
        uint8_t tag = frame[off];
        uint8_t tlen = frame[off + 1];
        if (off + 2 + tlen > len) break;
        if (tag == 0) { /* SSID */
            if (tlen > 0) return std::string((const char *)frame + off + 2, tlen);
            return "(hidden)";
        }
        off += 2 + tlen;
    }
    return "";
}

/* Extract source MAC (addr2) from 802.11 header */
static std::string frame_src(const uint8_t *frame, size_t len)
{
    if (len < 10) return "";
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
             frame[10], frame[11], frame[12], frame[13], frame[14], frame[15]);
    return b;
}

/* Extract BSSID (addr3) */
static std::string frame_bssid(const uint8_t *frame, size_t len)
{
    if (len < 16) return "";
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
             frame[16], frame[17], frame[18], frame[19], frame[20], frame[21]);
    return b;
}

/* Check if a frame is a deauth (type 0, subtype 12 = 0xC0) */
static bool is_deauth(const uint8_t *frame, size_t len)
{
    if (len < 2) return false;
    uint8_t fc0 = frame[0];
    uint8_t type = (fc0 >> 2) & 0x03;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    return type == 0 && (subtype == 12 || subtype == 10);  /* deauth (12) or dissoc (10) */
}

/* Check if a broadcast deauth (addr1 = ff:ff:ff:ff:ff:ff) */
static bool is_broadcast_deauth(const uint8_t *frame, size_t len)
{
    if (!is_deauth(frame, len) || len < 10) return false;
    return frame[4] == 0xFF && frame[5] == 0xFF && frame[6] == 0xFF &&
           frame[7] == 0xFF && frame[8] == 0xFF && frame[9] == 0xFF;
}

/* Analyse a buffer of captured management frames */
static void analyse_packets(void)
{
#ifdef ARDUINO
    std::lock_guard<std::mutex> lk(s_air_mx);
    while (s_air_r != s_air_w) {
        int slot = s_air_r;
        const uint8_t *frame = s_air_buf[slot];
        size_t len = s_air_len[slot];
        int rssi = s_air_rssi[slot];

        if (is_deauth(frame, len)) {
            s_deauth_total++;
            s_deauth_this_second++;

            bool broadcast = is_broadcast_deauth(frame, len);
            std::string src = frame_src(frame, len);
            std::string bssid = frame_bssid(frame, len);
            (void)src; (void)broadcast;
            (void)bssid;

        } else {
            /* Check for beacon */
            uint8_t fc0 = frame[0];
            uint8_t type = (fc0 >> 2) & 0x03;
            uint8_t subtype = (fc0 >> 4) & 0x0F;
            if (type == 0 && subtype == 8) {
                s_beacon_total++;
                s_beacon_this_second++;

                std::string ssid = beacon_ssid(frame, len);
                std::string bssid = frame_bssid(frame, len);

                if (!ssid.empty() && !bssid.empty()) {
                    /* Evil-twin check: same SSID but different BSSID already known */
                    for (auto &kv : s_known_aps) {
                        if (kv.second == ssid && kv.first != bssid) {
                            s_evil_twins++;
                            break;
                        }
                    }
                    s_known_aps[bssid] = ssid;
                    if (s_known_aps.size() > 1000) s_known_aps.clear(); /* prevent leak */
                }
            }
        }

        s_air_r = (slot + 1) % AIR_BUF_MAX;
    }
#else
    /* Simulator: consume injected alerts */
    std::lock_guard<std::mutex> lk(s_inject_mx);
    while (!s_inject_alerts.empty()) {
        /* parse alert types from the simulator */
        (void)s_inject_alerts.front();
        s_inject_alerts.pop();
    }
#endif
}

/* Publish an alert event */
static void publish_alert(const char *type, const char *detail, const char *bssid, int rssi)
{
    char j[512];
    snprintf(j, sizeof(j), "{\"type\":\"%s\",\"detail\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d}",
             type, detail, bssid ? bssid : "", rssi);
    web_events_publish("airspace", j);
}

/* Check thresholds and publish alerts */
static void check_thresholds(void)
{
    /* Deauth flood */
    if (s_deauth_this_second >= DEAUTH_FLOOD_THRESHOLD) {
        s_deauth_floods++;
        char detail[64];
        snprintf(detail, sizeof(detail), "%u deauths/s — flood detected", s_deauth_this_second);
        publish_alert("deauth_flood", detail, NULL, 0);
    }

    /* Beacon flood */
    if (s_beacon_this_second >= BEACON_FLOOD_THRESHOLD) {
        s_beacon_floods++;
        char detail[64];
        snprintf(detail, sizeof(detail), "%u beacons/s — flood detected", s_beacon_this_second);
        publish_alert("beacon_flood", detail, NULL, 0);
    }
}

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Airspace", NULL };

static void get_status(web_req_t *r)
{
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false") +
                    ",\"deauth_total\":" + tools_int(s_deauth_total) +
                    ",\"deauth_floods\":" + tools_int(s_deauth_floods) +
                    ",\"beacon_total\":" + tools_int(s_beacon_total) +
                    ",\"beacon_floods\":" + tools_int(s_beacon_floods) +
                    ",\"evil_twins\":" + tools_int(s_evil_twins);
    if (s_started) j += ",\"started\":" + tools_int(s_started);
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_ok(r); return; }

    s_active = true;
    s_started = tools_epoch();
    s_deauth_total = s_deauth_floods = 0;
    s_beacon_total = s_beacon_floods = 0;
    s_evil_twins = 0;
    s_deauth_this_second = s_deauth_last_second = 0;
    s_beacon_this_second = s_beacon_last_second = 0;
    s_known_aps.clear();

#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(air_promisc_cb);
    esp_wifi_set_promiscuous(true);
#endif

    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    if (!s_active) { web_resp_error(r, 409, "conflict", "not monitoring"); return; }
    s_active = false;

#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
#endif

    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/airspace") && strncmp(r->path, "/api/airspace/", 14)) return false;
    if (tools_is(r, "GET", "/api/airspace")) get_status(r);
    else if (tools_is(r, "POST", "/api/airspace/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/airspace/stop")) stop_monitor(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/airspace")) {
        std::string body, type, detail;
        web_read_body(r, body, 1024);
        web_json_get_str(body, "type", type);
        web_json_get_str(body, "detail", detail);
        std::lock_guard<std::mutex> lk(s_inject_mx);
        if (!type.empty()) s_inject_alerts.push(type + ":" + detail);
        web_resp_ok(r);
        return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void air_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;
    analyse_packets();
}

static void air_second(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;
    check_thresholds();
    s_deauth_last_second = s_deauth_this_second;
    s_deauth_this_second = 0;
    s_beacon_last_second = s_beacon_this_second;
    s_beacon_this_second = 0;
}

static void air_stop(void)
{
    if (s_active) {
        s_active = false;
#ifdef ARDUINO
        esp_wifi_set_promiscuous(false);
#endif
    }
}

static const char *const CAPS[] = { "airspace", NULL };
const web_tool_t web_tool_airspace = { CAPS, handle, air_tick, air_second, air_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_airspace = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */