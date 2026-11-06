/**
 * @file      web_tool_airspace.cpp
 * @brief     Passive Wi-Fi attack detection: deauth floods, broadcast deauth,
 *            possible evil twins and beacon floods. Never transmits.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <string>
#include <map>
#include <set>
#include <mutex>

#ifdef ARDUINO
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

#define AIRSPACE_RADIO_OWNER "Airspace"
#define AIR_QUEUE_LEN 12
#define AIR_FRAME_MAX 384
#define DEAUTH_FLOOD_THRESHOLD 50
#define BEACON_FLOOD_THRESHOLD 100
#define AIRSPACE_KNOWN_AP_LIMIT 256

static bool s_active = false;
static long long s_started = 0;
static uint32_t s_deauth_total = 0, s_deauth_floods = 0;
static uint32_t s_beacon_total = 0, s_beacon_floods = 0;
static uint32_t s_evil_twins = 0, s_broadcast_deauth = 0, s_dropped = 0;
static uint32_t s_deauth_window = 0, s_beacon_window = 0;
static char s_error[128] = "";
static std::map<std::string, std::set<std::string>> s_ssid_bssids;
static std::set<std::string> s_twin_alerts;

#ifdef ARDUINO
static uint8_t s_air_buf[AIR_QUEUE_LEN][AIR_FRAME_MAX];
static size_t s_air_len[AIR_QUEUE_LEN];
static int s_air_rssi[AIR_QUEUE_LEN];
static int s_air_w = 0, s_air_r = 0;
static std::mutex s_air_mx;

static void air_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (!s_active || !buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 2) return;
    uint8_t fc0 = pkt->payload[0];
    if (((fc0 >> 2) & 0x03) != 0) return;       /* management frames only */

    std::lock_guard<std::mutex> lk(s_air_mx);
    int next = (s_air_w + 1) % AIR_QUEUE_LEN;
    if (next == s_air_r) {
        s_dropped++;
        return;
    }
    size_t n = pkt->rx_ctrl.sig_len;
    if (n > AIR_FRAME_MAX) n = AIR_FRAME_MAX;
    memcpy(s_air_buf[s_air_w], pkt->payload, n);
    s_air_len[s_air_w] = n;
    s_air_rssi[s_air_w] = pkt->rx_ctrl.rssi;
    s_air_w = next;
}
#endif

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static std::string frame_mac(const uint8_t *frame, size_t len, size_t offset)
{
    if (len < offset + 6) return "";
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
             frame[offset], frame[offset + 1], frame[offset + 2],
             frame[offset + 3], frame[offset + 4], frame[offset + 5]);
    return b;
}

static std::string beacon_ssid(const uint8_t *frame, size_t len)
{
    if (len < 36) return "";
    size_t off = 36;
    while (off + 2 <= len) {
        uint8_t tag = frame[off], tag_len = frame[off + 1];
        if (off + 2 + tag_len > len) break;
        if (tag == 0) return tag_len ? std::string((const char *)frame + off + 2, tag_len) : "(hidden)";
        off += 2 + tag_len;
    }
    return "";
}

static bool is_deauth(const uint8_t *frame, size_t len)
{
    if (len < 2) return false;
    uint8_t fc0 = frame[0];
    uint8_t type = (fc0 >> 2) & 0x03, subtype = (fc0 >> 4) & 0x0f;
    return type == 0 && (subtype == 12 || subtype == 10);
}

static bool is_broadcast_deauth(const uint8_t *frame, size_t len)
{
    if (!is_deauth(frame, len) || len < 10) return false;
    for (int i = 4; i < 10; i++) if (frame[i] != 0xff) return false;
    return true;
}

static void publish_alert(const char *type, const std::string &detail, const std::string &bssid, int rssi)
{
    std::string j = "{\"type\":";
    web_json_str(j, type);
    j += ",\"detail\":";
    web_json_str(j, detail.c_str());
    j += ",\"bssid\":";
    web_json_str(j, bssid.c_str());
    j += ",\"rssi\":" + tools_int(rssi) + "}";
    web_events_publish("airspace", j.c_str());
}

static void analyse_packets(void)
{
#ifdef ARDUINO
    std::lock_guard<std::mutex> lk(s_air_mx);
    while (s_air_r != s_air_w) {
        int slot = s_air_r;
        const uint8_t *frame = s_air_buf[slot];
        size_t len = s_air_len[slot];
        int rssi = s_air_rssi[slot];
        s_air_r = (slot + 1) % AIR_QUEUE_LEN;
        if (len < 2) continue;
        uint8_t type = (frame[0] >> 2) & 0x03, subtype = (frame[0] >> 4) & 0x0f;

        if (is_deauth(frame, len)) {
            s_deauth_total++;
            s_deauth_window++;
            if (is_broadcast_deauth(frame, len)) {
                s_broadcast_deauth++;
                std::string bssid = frame_mac(frame, len, 16);
                publish_alert("broadcast_deauth", "Broadcast deauthentication/disassociation seen", bssid, rssi);
            }
        } else if (type == 0 && subtype == 8) {
            s_beacon_total++;
            s_beacon_window++;
            std::string ssid = beacon_ssid(frame, len);
            std::string bssid = frame_mac(frame, len, 16);
            if (ssid.empty() || bssid.empty()) continue;

            auto &bssids = s_ssid_bssids[ssid];
            if (!bssids.empty() && !bssids.count(bssid)) {
                std::string key = ssid + "\n" + bssid;
                if (!s_twin_alerts.count(key)) {
                    s_twin_alerts.insert(key);
                    s_evil_twins++;
                    publish_alert("evil_twin", "Same SSID seen on another BSSID; verify the access point", bssid, rssi);
                }
            }
            if (bssids.size() < 16) bssids.insert(bssid);
            if (s_ssid_bssids.size() > AIRSPACE_KNOWN_AP_LIMIT) {
                /* Bound memory without repeatedly re-alerting on the same current pair. */
                s_ssid_bssids.erase(s_ssid_bssids.begin());
            }
        }
    }
#endif
}

static void check_thresholds(void)
{
    if (s_deauth_window >= DEAUTH_FLOOD_THRESHOLD) {
        s_deauth_floods++;
        char detail[80];
        snprintf(detail, sizeof(detail), "%u deauth/disassociation frames in one second", (unsigned)s_deauth_window);
        publish_alert("deauth_flood", detail, "", 0);
    }
    if (s_beacon_window >= BEACON_FLOOD_THRESHOLD) {
        s_beacon_floods++;
        char detail[80];
        snprintf(detail, sizeof(detail), "%u beacon frames in one second", (unsigned)s_beacon_window);
        publish_alert("beacon_flood", detail, "", 0);
    }
    s_deauth_window = s_beacon_window = 0;
}

/* ---------------------------------------------------------------- API and shared lifecycle */

static const char *const APPS[] = { "Airspace", NULL };

static void get_status(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"deauth_total\":" + tools_int(s_deauth_total) +
        ",\"deauth_floods\":" + tools_int(s_deauth_floods) +
        ",\"beacon_total\":" + tools_int(s_beacon_total) +
        ",\"beacon_floods\":" + tools_int(s_beacon_floods) +
        ",\"evil_twins\":" + tools_int(s_evil_twins) +
        ",\"broadcast_deauth\":" + tools_int(s_broadcast_deauth) +
        ",\"drops\":" + tools_int(s_dropped) +
        ",\"started\":" + tools_int(s_started) + ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += "}";
    web_resp_json(r, 200, j);
}

static int begin_monitor(void)
{
    if (s_active) return 0;
    std::string radio_error;
    if (!tools_wifi_claim(AIRSPACE_RADIO_OWNER, radio_error)) {
        set_error(radio_error.c_str());
        return 503;
    }
    set_error("");
    s_started = tools_epoch();
    s_deauth_total = s_deauth_floods = s_beacon_total = s_beacon_floods = 0;
    s_evil_twins = s_broadcast_deauth = s_dropped = 0;
    s_deauth_window = s_beacon_window = 0;
    s_ssid_bssids.clear();
    s_twin_alerts.clear();
#ifdef ARDUINO
    {
        std::lock_guard<std::mutex> lk(s_air_mx);
        s_air_r = s_air_w = 0;
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_err_t rc = esp_wifi_set_promiscuous_filter(&filter);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous_rx_cb(air_promisc_cb);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous(true);
    if (rc != ESP_OK) {
        tools_wifi_release(AIRSPACE_RADIO_OWNER);
        set_error("Wi-Fi monitor mode could not start");
        return 500;
    }
#endif
    s_active = true;
    return 0;
}

static void finish_monitor(void)
{
    s_active = false;
#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
#endif
    tools_wifi_release(AIRSPACE_RADIO_OWNER);
}

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    int rc = begin_monitor();
    if (rc) web_resp_error(r, rc, rc == 503 ? "busy" : "internal", s_error);
    else web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    if (!s_active) { web_resp_error(r, 409, "conflict", "not monitoring"); return; }
    finish_monitor();
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/airspace")) {
        std::string body, type, detail, bssid;
        double rssi = 0;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "type", type);
        web_json_get_str(body, "detail", detail);
        web_json_get_str(body, "bssid", bssid);
        web_json_get_num(body, "rssi", &rssi);
        if (!s_active) { web_resp_error(r, 409, "conflict", "start monitoring first"); return true; }
        if (type == "deauth_flood") { s_deauth_floods++; s_deauth_total += 50; }
        else if (type == "beacon_flood") { s_beacon_floods++; s_beacon_total += 100; }
        else if (type == "evil_twin") s_evil_twins++;
        else if (type == "broadcast_deauth") { s_broadcast_deauth++; s_deauth_total++; }
        else { web_resp_error(r, 400, "bad_request", "unknown alert type"); return true; }
        publish_alert(type.c_str(), detail.empty() ? "Injected simulator alert" : detail, bssid, (int)rssi);
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/airspace") && strncmp(r->path, "/api/airspace/", 14)) return false;
    if (tools_is(r, "GET", "/api/airspace")) get_status(r);
    else if (tools_is(r, "POST", "/api/airspace/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/airspace/stop")) stop_monitor(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void air_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (s_active) analyse_packets();
}

static void air_second(uint32_t now_ms)
{
    (void)now_ms;
    if (s_active) check_thresholds();
}

static void air_stop(void)
{
    finish_monitor();
}

bool web_hw_airspace_start(void) { return begin_monitor() == 0; }
bool web_hw_airspace_stop(void) { finish_monitor(); return true; }
void web_hw_airspace_status(web_hw_airspace_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->deauth_count = s_deauth_total;
    out->beacon_count = s_beacon_total;
    out->deauth_floods = s_deauth_floods;
    out->beacon_floods = s_beacon_floods;
    out->evil_twins = s_evil_twins;
    out->broadcast_deauth = s_broadcast_deauth;
    out->drops = s_dropped;
    out->started_epoch = s_started;
    out->error = s_error[0] ? s_error : NULL;
}

static const char *const CAPS[] = { "airspace", NULL };
const web_tool_t web_tool_airspace = { CAPS, handle, air_tick, air_second, air_stop };

#else
const web_tool_t web_tool_airspace = { NULL, NULL, NULL, NULL, NULL };
#endif
