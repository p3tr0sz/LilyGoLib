/**
 * @file      web_tool_beaconspam.cpp
 * @brief     Authorized fake-beacon injection with generated or custom SSIDs.
 * WARNING: disruptive radio testing only in an environment you control.
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

#define BEACON_RADIO_OWNER "Beacon Spam"
#define BEACON_FRAME_MAX 128
#define BEACON_MAX_CUSTOM 64
#define BEACON_TARGET_ERROR_LIMIT 25

enum beacon_campaign_state_t { BC_PENDING, BC_RUNNING, BC_DONE, BC_ERROR };
typedef struct {
    uint32_t accepted;
    uint32_t failed;
    uint32_t consecutive_errors;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    beacon_campaign_state_t state;
    char error[64];
} beacon_campaign_target_t;

static bool s_active = false;
static bool s_campaign_active = false;
static beacon_campaign_target_t s_campaign[BEACON_MAX_CUSTOM];
static uint32_t s_campaign_goal = 10;
static uint32_t s_reconnect_started_ms = 0;
static uint32_t s_reconnect_ms = 0;
static int s_reconnect_attempts = 0;
static bool s_reconnect_pending = false;
static bool s_reconnected = false;
static int s_channel = 0;                 /* 0 = hop 1-13 */
static int s_mode = 0;                    /* 0 = generated, 2 = custom */
static uint32_t s_beacon_count = 0, s_tx_errors = 0;
static uint32_t s_ssid_count = 0, s_ssid_index = 0;
static char s_prefix[21] = "FreeNet";
static long long s_started = 0;
static uint32_t s_last_send_ms = 0;
static char s_error[160] = "";
static char s_custom_ssids[BEACON_MAX_CUSTOM][33];
static int s_custom_count = 0;
static std::mutex s_custom_mx;
#ifdef ARDUINO
static bool s_reconnect = false;
static int s_tx_channel = 0;
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

static void bssid_from_index(uint8_t bssid[6], uint32_t idx)
{
    bssid[0] = 0x02; bssid[1] = 0x00; bssid[2] = 0x00;
    bssid[3] = idx >> 16; bssid[4] = idx >> 8; bssid[5] = idx;
}

#ifdef ARDUINO
static size_t build_beacon(uint8_t *frame, const char *ssid, const uint8_t *bssid, int channel)
{
    memset(frame, 0, BEACON_FRAME_MAX);
    frame[0] = 0x80;
    memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, bssid, 6);
    memcpy(frame + 16, bssid, 6);
    uint16_t seq = (uint16_t)(s_beacon_count & 0x0fff);
    frame[22] = seq & 0xff;
    frame[23] = (seq >> 8) & 0x0f;
    frame[32] = 0x64;                         /* 100 TU */
    frame[34] = 0x01;                         /* ESS, open network */
    size_t off = 36;
    size_t ssid_len = strlen(ssid);
    if (ssid_len > 32) ssid_len = 32;
    frame[off++] = 0;
    frame[off++] = (uint8_t)ssid_len;
    memcpy(frame + off, ssid, ssid_len);
    off += ssid_len;
    const uint8_t rates[] = {1, 4, 0x82, 0x84, 0x8b, 0x96}; /* 1, 2, 5.5, 11 Mb/s basic */
    memcpy(frame + off, rates, sizeof(rates));
    off += sizeof(rates);
    frame[off++] = 3;
    frame[off++] = 1;
    frame[off++] = (uint8_t)channel;
    return off;
}

static void send_beacon(const char *ssid, uint32_t index)
{
    uint8_t bssid[6], frame[BEACON_FRAME_MAX];
    bssid_from_index(bssid, index);
    int channel = s_channel > 0 ? s_channel : (int)(index % 13) + 1;
    if (channel != s_tx_channel) {
        esp_err_t rc = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (rc != ESP_OK) {
            s_tx_errors++;
            set_error("Could not select beacon channel");
            return;
        }
        s_tx_channel = channel;
    }
    size_t len = build_beacon(frame, ssid, bssid, channel);
    esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false);
    if (rc == ESP_OK) s_beacon_count++;
    else {
        s_tx_errors++;
        if (s_tx_errors == 1 || s_tx_errors % 20 == 0) {
            char b[96];
            snprintf(b, sizeof(b), "Beacon injection failed: %s", esp_err_to_name(rc));
            set_error(b);
        }
    }
}
#endif

/* Parse a top-level JSON array of strings, including normal JSON escapes. */
static bool hex4(const char *p, unsigned &v)
{
    v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
        else return false;
    }
    return true;
}

static bool parse_ssid_array(const std::string &body, std::vector<std::string> &out, std::string &error)
{
    out.clear();
    size_t i = 0;
    auto ws = [&]() { while (i < body.size() && isspace((unsigned char)body[i])) i++; };
    ws();
    if (i >= body.size() || body[i++] != '[') { error = "Body must be a JSON array of SSIDs"; return false; }
    ws();
    if (i < body.size() && body[i] == ']') { i++; ws(); return i == body.size(); }
    while (i < body.size()) {
        if (body[i++] != '"') { error = "Every SSID must be a JSON string"; return false; }
        std::string value;
        bool closed = false;
        while (i < body.size()) {
            unsigned char c = body[i++];
            if (c == '"') { closed = true; break; }
            if (c < 0x20) { error = "Control characters are not allowed in SSIDs"; return false; }
            if (c != '\\') { value += (char)c; continue; }
            if (i >= body.size()) break;
            char esc = body[i++];
            switch (esc) {
                case '"': case '\\': case '/': value += esc; break;
                case 'b': value += '\b'; break;
                case 'f': value += '\f'; break;
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                case 'u': {
                    if (i + 4 > body.size()) { error = "Bad Unicode escape"; return false; }
                    unsigned cp;
                    if (!hex4(body.c_str() + i, cp)) { error = "Bad Unicode escape"; return false; }
                    i += 4;
                    if (cp <= 0x7f) value += (char)cp;
                    else if (cp <= 0x7ff) { value += (char)(0xc0 | (cp >> 6)); value += (char)(0x80 | (cp & 63)); }
                    else { value += (char)(0xe0 | (cp >> 12)); value += (char)(0x80 | ((cp >> 6) & 63)); value += (char)(0x80 | (cp & 63)); }
                    break;
                }
                default: error = "Bad string escape"; return false;
            }
            if (value.size() > 32) { error = "Each SSID must be 1-32 UTF-8 bytes"; return false; }
        }
        if (!closed) { error = "Unterminated SSID string"; return false; }
        if (value.empty() || value.size() > 32) { error = "Each SSID must be 1-32 UTF-8 bytes"; return false; }
        if (out.size() >= BEACON_MAX_CUSTOM) { error = "At most 64 custom SSIDs"; return false; }
        out.push_back(value);
        ws();
        if (i < body.size() && body[i] == ',') { i++; ws(); continue; }
        if (i < body.size() && body[i] == ']') { i++; ws(); return i == body.size(); }
        error = "Expected ',' or ']' after SSID";
        return false;
    }
    error = "Unterminated SSID array";
    return false;
}

static int begin_spam(int channel, int count, const char *prefix, bool custom)
{
    if (s_active) { set_error("Beacon test already running"); return 409; }
    s_campaign_active = false;
    memset(s_campaign, 0, sizeof(s_campaign));
    if (channel < 0 || channel > 13) { set_error("Channel must be 0 (hop) or 1-13"); return 400; }
    if (custom) {
        std::lock_guard<std::mutex> lk(s_custom_mx);
        if (!s_custom_count) { set_error("Add at least one custom SSID first"); return 400; }
        s_ssid_count = (uint32_t)s_custom_count;
    } else {
        if (count < 1 || count > 10000) { set_error("SSID count must be 1-10000"); return 400; }
        if (!prefix || !*prefix || strlen(prefix) > 20) { set_error("Prefix must be 1-20 bytes"); return 400; }
        s_ssid_count = (uint32_t)count;
        snprintf(s_prefix, sizeof(s_prefix), "%s", prefix);
    }

    std::string radio_error;
    if (!tools_wifi_claim(BEACON_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    set_error("");
    s_mode = custom ? 2 : 0;
    s_channel = channel;
    s_beacon_count = s_tx_errors = 0;
    s_ssid_index = 0;
    reset_reconnect_metrics();
    s_started = tools_epoch();
    s_last_send_ms = 0;
#ifdef ARDUINO
    s_reconnect = WiFi.status() == WL_CONNECTED;
    WiFi.disconnect(false, false);
    esp_wifi_set_promiscuous(false);
    esp_err_t rc = esp_wifi_set_channel(channel > 0 ? channel : 1, WIFI_SECOND_CHAN_NONE);
    if (rc != ESP_OK) {
        tools_wifi_release(BEACON_RADIO_OWNER);
        if (s_reconnect) WiFi.reconnect();
        set_error("Wi-Fi could not start beacon injection");
        return 500;
    }
    s_tx_channel = channel > 0 ? channel : 1;
#endif
    s_active = true;
    return 0;
}

static void finish_spam(void)
{
    if (!s_active) return;
    s_active = false;
    s_campaign_active = false;
    tools_wifi_release(BEACON_RADIO_OWNER);
    request_reconnect();
#ifdef ARDUINO
    s_tx_channel = 0;
#endif
}

/* ---------------------------------------------------------------- finite per-SSID campaigns */

static int campaign_completed(void)
{
    if (!s_campaign_active && s_campaign[0].state == BC_PENDING) return 0;
    int done = 0;
    for (uint32_t i = 0; i < s_ssid_count && i < BEACON_MAX_CUSTOM; i++) if (s_campaign[i].state >= BC_DONE) done++;
    return done;
}

static const char *campaign_state_name(beacon_campaign_state_t state)
{
    switch (state) {
        case BC_RUNNING: return "running";
        case BC_DONE: return "done";
        case BC_ERROR: return "error";
        default: return "pending";
    }
}

static void campaign_ssid(uint32_t index, char out[33])
{
    if (s_mode == 2) {
        std::lock_guard<std::mutex> lk(s_custom_mx);
        snprintf(out, 33, "%s", s_custom_ssids[index % s_custom_count]);
    } else snprintf(out, 33, "%s%04u", s_prefix, (unsigned)index);
}

static void campaign_advance(void);

static void campaign_finish_target(beacon_campaign_state_t state, const char *error = NULL)
{
    if (!s_campaign_active || s_ssid_index >= s_ssid_count) return;
    beacon_campaign_target_t &t = s_campaign[s_ssid_index];
    t.state = state;
    t.elapsed_ms = tools_now_ms() - t.started_ms;
    if (error) snprintf(t.error, sizeof(t.error), "%s", error);
    campaign_advance();
}

static void campaign_advance(void)
{
    s_ssid_index++;
    if (!s_campaign_active) return;
    if (s_ssid_index >= s_ssid_count) { finish_spam(); return; }
    beacon_campaign_target_t &t = s_campaign[s_ssid_index];
    t.state = BC_RUNNING;
    t.started_ms = tools_now_ms();
}

static int begin_campaign(int channel, int count, const char *prefix, bool custom, int frames_per_ssid)
{
    if (frames_per_ssid < 1 || frames_per_ssid > 10000) { set_error("Frames per SSID must be 1-10000"); return 400; }
    if (custom) {
        std::lock_guard<std::mutex> lk(s_custom_mx);
        if (s_custom_count > BEACON_MAX_CUSTOM) { set_error("At most 64 campaign SSIDs"); return 400; }
    } else if (count > BEACON_MAX_CUSTOM) { set_error("Campaigns support at most 64 generated SSIDs"); return 400; }
    int rc = begin_spam(channel, count, prefix, custom);
    if (rc) return rc;
    if (s_ssid_count > BEACON_MAX_CUSTOM) { finish_spam(); set_error("At most 64 campaign SSIDs"); return 400; }
    memset(s_campaign, 0, sizeof(s_campaign));
    s_campaign_goal = (uint32_t)frames_per_ssid;
    s_campaign_active = true;
    s_ssid_index = 0;
    s_campaign[0].state = BC_RUNNING;
    s_campaign[0].started_ms = tools_now_ms();
    return 0;
}

static void campaign_send_current(void)
{
    if (!s_campaign_active || s_ssid_index >= s_ssid_count) return;
    beacon_campaign_target_t &t = s_campaign[s_ssid_index];
    char ssid[33];
    campaign_ssid(s_ssid_index, ssid);
#ifdef ARDUINO
    uint32_t old_count = s_beacon_count, old_errors = s_tx_errors;
    send_beacon(ssid, s_ssid_index);
    if (s_beacon_count != old_count) { t.accepted++; t.consecutive_errors = 0; }
    else if (s_tx_errors != old_errors) {
        t.failed++; t.consecutive_errors++;
        if (t.consecutive_errors >= BEACON_TARGET_ERROR_LIMIT) {
            campaign_finish_target(BC_ERROR, "Injection failed repeatedly; SSID skipped"); return;
        }
    }
#else
    (void)ssid;
    t.accepted++;
    s_beacon_count++;
#endif
    if (t.accepted >= s_campaign_goal) campaign_finish_target(BC_DONE);
}

static void campaign_fail_current(const char *reason)
{
    if (!s_campaign_active || s_ssid_index >= s_ssid_count) return;
    beacon_campaign_target_t &t = s_campaign[s_ssid_index];
    t.failed++;
    s_tx_errors++;
    campaign_finish_target(BC_ERROR, reason);
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Beacon Spam", NULL };

static void status_json(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"channel\":" + tools_int(s_channel) +
        ",\"beacons\":" + tools_int(s_beacon_count) +
        ",\"txErrors\":" + tools_int(s_tx_errors) +
        ",\"ssids\":" + tools_int(s_ssid_count) +
        ",\"mode\":" + tools_int(s_mode) +
        ",\"started\":" + tools_int(s_started) + ",\"prefix\":";
    web_json_str(j, s_prefix);
    j += ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += ",\"reconnect\":{\"pending\":" + std::string(s_reconnect_pending ? "true" : "false") +
        ",\"reconnected\":" + std::string(s_reconnected ? "true" : "false") +
        ",\"attempts\":" + tools_int(s_reconnect_attempts) + ",\"elapsedMs\":" + tools_int(s_reconnect_ms) + "}";
    j += ",\"campaign\":{\"active\":" + std::string(s_campaign_active ? "true" : "false") +
        ",\"current\":" + tools_int(s_ssid_index) + ",\"completed\":" + tools_int(campaign_completed()) +
        ",\"count\":" + tools_int(s_campaign_active || s_campaign[0].state != BC_PENDING ? s_ssid_count : 0) +
        ",\"framesPerSsid\":" + tools_int(s_campaign_goal) + "}";
    j += ",\"progress\":[";
    if (s_campaign_active || s_campaign[0].state != BC_PENDING) {
        for (uint32_t i = 0; i < s_ssid_count; i++) {
            const beacon_campaign_target_t &t = s_campaign[i];
            char ssid[33];
            campaign_ssid(i, ssid);
            if (i) j += ",";
            j += "{\"ssid\":"; web_json_str(j, ssid);
            j += ",\"state\":"; web_json_str(j, campaign_state_name(t.state));
            j += ",\"accepted\":" + tools_int(t.accepted) + ",\"failed\":" + tools_int(t.failed) +
                ",\"goal\":" + tools_int(s_campaign_goal) + ",\"elapsedMs\":" + tools_int(t.elapsed_ms) + ",\"error\":";
            if (t.error[0]) web_json_str(j, t.error); else j += "null";
            j += "}";
        }
    }
    j += "],\"customSsids\":[";
    {
        std::lock_guard<std::mutex> lk(s_custom_mx);
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
    std::string body, prefix = "FreeNet", mode = "generated";
    double channel = 0, count = 20;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "prefix", prefix);
    web_json_get_str(body, "mode", mode);
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    if (web_json_has(body, "count") && !web_json_get_num(body, "count", &count)) {
        web_resp_error(r, 400, "bad_request", "count must be a number"); return;
    }
    if (mode != "generated" && mode != "custom") {
        web_resp_error(r, 400, "bad_request", "mode: generated or custom"); return;
    }
    bool approved = false;
    if (!web_json_get_bool(body, "authorized", &approved) || !approved) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this environment"); return;
    }
    int rc = begin_spam((int)channel, (int)count, prefix.c_str(), mode == "custom");
    if (rc) web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "internal", s_error);
    else web_resp_ok(r);
}

static void start_campaign(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, prefix = "FreeNet", mode = "generated";
    double channel = 0, count = 20, frames = 10;
    if (!tools_read_json(r, body)) return;
    bool approved = false;
    if (!web_json_get_bool(body, "authorized", &approved) || !approved) {
        web_resp_error(r, 400, "authorization_required", "Confirm this beacon campaign is authorized"); return;
    }
    web_json_get_str(body, "prefix", prefix);
    web_json_get_str(body, "mode", mode);
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    if (web_json_has(body, "count") && !web_json_get_num(body, "count", &count)) {
        web_resp_error(r, 400, "bad_request", "count must be a number"); return;
    }
    if (web_json_has(body, "framesPerSsid") && !web_json_get_num(body, "framesPerSsid", &frames)) {
        web_resp_error(r, 400, "bad_request", "framesPerSsid must be a number"); return;
    }
    if (mode != "generated" && mode != "custom") {
        web_resp_error(r, 400, "bad_request", "mode: generated or custom"); return;
    }
    int rc = begin_campaign((int)channel, (int)count, prefix.c_str(), mode == "custom", (int)frames);
    if (rc) web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "internal", s_error);
    else web_resp_ok(r, ("\"active\":" + std::string(s_campaign_active ? "true" : "false")).c_str());
}

static void stop_spam(web_req_t *r)
{
    finish_spam();
    web_resp_ok(r);
}

static void set_custom_ssids(web_req_t *r)
{
    if (s_active) { web_resp_error(r, 409, "conflict", "stop beacon spam before editing SSIDs"); return; }
    std::string body;
    if (!web_read_body(r, body, 4096)) { web_resp_error(r, 400, "bad_request", "body too large"); return; }
    std::vector<std::string> values;
    std::string error;
    if (!parse_ssid_array(body, values, error)) { web_resp_error(r, 400, "bad_request", error.c_str()); return; }
    {
        std::lock_guard<std::mutex> lk(s_custom_mx);
        s_custom_count = (int)values.size();
        for (int i = 0; i < s_custom_count; i++) snprintf(s_custom_ssids[i], 33, "%s", values[i].c_str());
    }
    web_resp_ok(r, ("\"count\":" + tools_int((long long)values.size())).c_str());
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/beaconspam")) {
        std::string body, action;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "action", action);
        if (action == "fail-target") {
            if (!s_campaign_active) web_resp_error(r, 409, "conflict", "no campaign SSID is running");
            else { campaign_fail_current("Simulator injection failure"); web_resp_ok(r); }
            return true;
        }
        int rc = begin_spam(1, 20, "FreeNet", false);
        if (rc) web_resp_error(r, rc, rc == 503 ? "busy" : "conflict", s_error);
        else web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/beaconspam") && strncmp(r->path, "/api/beaconspam/", 16)) return false;
    if (tools_is(r, "GET", "/api/beaconspam")) status_json(r);
    else if (tools_is(r, "POST", "/api/beaconspam/start")) start_spam(r);
    else if (tools_is(r, "POST", "/api/beaconspam/campaign/start")) start_campaign(r);
    else if (tools_is(r, "POST", "/api/beaconspam/stop") || tools_is(r, "POST", "/api/beaconspam/campaign/stop")) stop_spam(r);
    else if (tools_is(r, "PUT", "/api/beaconspam/ssids")) set_custom_ssids(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void beacon_tick(uint32_t now_ms)
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
    if (!s_active || now_ms - s_last_send_ms < 100) return;
    s_last_send_ms = now_ms;
    for (int i = 0; i < 2 && s_active; i++) {
        if (s_campaign_active) {
            campaign_send_current();
            if (s_campaign_active && s_ssid_index < s_ssid_count)
                s_campaign[s_ssid_index].elapsed_ms = now_ms - s_campaign[s_ssid_index].started_ms;
            continue;
        }
        char ssid[33];
        if (s_mode == 2) {
            std::lock_guard<std::mutex> lk(s_custom_mx);
            snprintf(ssid, sizeof(ssid), "%s", s_custom_ssids[s_ssid_index % s_custom_count]);
        } else {
            snprintf(ssid, sizeof(ssid), "%s%04u", s_prefix, (unsigned)s_ssid_index);
        }
#ifdef ARDUINO
        send_beacon(ssid, s_ssid_index);
#else
        s_beacon_count++;
#endif
        s_ssid_index = (s_ssid_index + 1) % s_ssid_count;
    }
}

static void beacon_stop(void) { finish_spam(); }

bool web_hw_beaconspam_start(int channel, int count, const char *prefix)
{
    return begin_spam(channel, count, prefix, false) == 0;
}
bool web_hw_beaconspam_stop(void) { finish_spam(); return true; }
bool web_hw_beaconspam_campaign_start(int channel, int count, const char *prefix, int frames_per_ssid)
{
    return begin_campaign(channel, count, prefix, false, frames_per_ssid) == 0;
}
void web_hw_beaconspam_status(web_hw_beaconspam_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->mode = s_mode;
    out->channel = s_channel;
    out->beacon_count = s_beacon_count;
    out->tx_errors = s_tx_errors;
    out->campaign_active = s_campaign_active;
    out->campaign_completed = campaign_completed();
    out->reconnect_attempts = s_reconnect_attempts;
    out->reconnect_ms = s_reconnect_ms;
    out->ssid_count = s_ssid_count;
    out->started_epoch = s_started;
    out->error = s_error[0] ? s_error : NULL;
}
void web_hw_beaconspam_set_custom_ssids(const char **ssids, int count)
{
    std::lock_guard<std::mutex> lk(s_custom_mx);
    s_custom_count = 0;
    for (int i = 0; i < count && i < BEACON_MAX_CUSTOM; i++) {
        if (!ssids[i] || !*ssids[i] || strlen(ssids[i]) > 32) continue;
        snprintf(s_custom_ssids[s_custom_count++], 33, "%s", ssids[i]);
    }
}
int web_hw_beaconspam_custom_count(void)
{
    std::lock_guard<std::mutex> lk(s_custom_mx);
    return s_custom_count;
}

static const char *const CAPS[] = { "beacon.spam", "beacon.campaign", "beacon.health", NULL };
const web_tool_t web_tool_beaconspam = { CAPS, handle, beacon_tick, NULL, beacon_stop };

#else
const web_tool_t web_tool_beaconspam = { NULL, NULL, NULL, NULL, NULL };
#endif
