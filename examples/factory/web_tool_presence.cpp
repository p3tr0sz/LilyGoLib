/**
 * @file      web_tool_presence.cpp
 * @brief     Nearby ESP-NOW peer tracking built on the Link tool.
 *
 * This is intentionally named as a limited capability: it discovers nearby
 * watches/peers; it is NOT a Wi-Fi CSI human-motion detector.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <string>
#include <mutex>

#define PRESENCE_MAX_PEERS 32
#define PRESENCE_TIMEOUT_S 300
#define PRESENCE_HISTORY_MAX 50

static bool s_active = false;
static long long s_started = 0;
static web_hw_presence_peer_t s_peers[PRESENCE_MAX_PEERS];
static int s_peer_count = 0;
static uint32_t s_peer_total = 0;
static web_hw_presence_history_t s_history[PRESENCE_HISTORY_MAX];
static int s_history_count = 0;
static uint32_t s_poll_last_ms = 0;
static char s_error[128] = "";
static std::mutex s_mx;

static int find_peer(const char *mac)
{
    for (int i = 0; i < s_peer_count; i++) if (!strcasecmp(s_peers[i].mac, mac)) return i;
    return -1;
}

static void publish_change(const char *event, const web_hw_presence_peer_t &p)
{
    std::string j = "{\"event\":";
    web_json_str(j, event);
    j += ",\"mac\":";
    web_json_str(j, p.mac);
    j += ",\"name\":";
    web_json_str(j, p.name);
    j += ",\"rssi\":" + tools_int(p.rssi) + "}";
    web_events_publish("presence", j.c_str());
}

static void add_history(const web_hw_presence_peer_t &p, const char *event)
{
    if (s_history_count >= PRESENCE_HISTORY_MAX) {
        memmove(s_history, s_history + 1, sizeof(s_history[0]) * (PRESENCE_HISTORY_MAX - 1));
        s_history_count--;
    }
    auto &h = s_history[s_history_count++];
    h.ts = tools_epoch();
    snprintf(h.mac, sizeof(h.mac), "%s", p.mac);
    snprintf(h.name, sizeof(h.name), "%s", p.name);
    snprintf(h.event, sizeof(h.event), "%s", event);
    publish_change(event, p);
}

/* s_mx held. */
static void peer_seen(const char *mac, const char *name, int rssi)
{
    long long now = tools_epoch();
    int idx = find_peer(mac);
    if (idx < 0) {
        if (s_peer_count >= PRESENCE_MAX_PEERS) return;
        idx = s_peer_count++;
        memset(&s_peers[idx], 0, sizeof(s_peers[idx]));
        snprintf(s_peers[idx].mac, sizeof(s_peers[idx].mac), "%s", mac);
        s_peers[idx].first_seen = now;
        s_peers[idx].present = true;
        s_peer_total++;
        auto &p = s_peers[idx];
        snprintf(p.name, sizeof(p.name), "%s", name ? name : "");
        p.rssi = rssi;
        p.last_seen = now;
        add_history(p, "appeared");
        return;
    }
    auto &p = s_peers[idx];
    bool returned = !p.present;
    p.present = true;
    p.last_seen = now;
    p.rssi = rssi;
    if (name && *name) snprintf(p.name, sizeof(p.name), "%s", name);
    if (returned) add_history(p, "appeared");
}

/* s_mx held. */
static void check_timeouts(void)
{
    long long now = tools_epoch();
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].present && now - s_peers[i].last_seen > PRESENCE_TIMEOUT_S) {
            s_peers[i].present = false;
            add_history(s_peers[i], "disappeared");
        }
    }
}

static void poll_peers(void)
{
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) {
        std::lock_guard<std::mutex> lk(s_mx);
        snprintf(s_error, sizeof(s_error), "%s", "ESP-NOW could not start");
        return;
    }
    web_hw_peer_t found[16];
    int n = web_hw_link_peers(found, 16);
    std::lock_guard<std::mutex> lk(s_mx);
    if (n < 0) {
        snprintf(s_error, sizeof(s_error), "%s", "Could not read nearby peers");
        return;
    }
    s_error[0] = 0;
    for (int i = 0; i < n; i++) peer_seen(found[i].mac, found[i].name, found[i].rssi);
    check_timeouts();
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Presence", "Nearby Peers", NULL };

static std::string status_json_text(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"started\":" + tools_int(s_started) +
        ",\"peers_ever\":" + tools_int(s_peer_total) + ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += ",\"peers\":[";
    for (int i = 0; i < s_peer_count; i++) {
        if (i) j += ",";
        j += "{\"mac\":";
        web_json_str(j, s_peers[i].mac);
        j += ",\"name\":";
        web_json_str(j, s_peers[i].name);
        j += ",\"rssi\":" + tools_int(s_peers[i].rssi) +
            ",\"first_seen\":" + tools_int(s_peers[i].first_seen) +
            ",\"last_seen\":" + tools_int(s_peers[i].last_seen) +
            ",\"present\":" + (s_peers[i].present ? "true" : "false") + "}";
    }
    j += "],\"history\":[";
    for (int i = 0; i < s_history_count; i++) {
        if (i) j += ",";
        j += "{\"ts\":" + tools_int(s_history[i].ts) + ",\"mac\":";
        web_json_str(j, s_history[i].mac);
        j += ",\"name\":";
        web_json_str(j, s_history[i].name);
        j += ",\"event\":";
        web_json_str(j, s_history[i].event);
        j += "}";
    }
    return j + "]}";
}

static void get_status(web_req_t *r) { web_resp_json(r, 200, status_json_text()); }

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) {
        web_resp_error(r, 503, "internal", "ESP-NOW could not start"); return;
    }
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_active) {
            s_active = true;
            s_started = tools_epoch();
            s_peer_count = s_history_count = 0;
            s_peer_total = 0;
            s_error[0] = 0;
            s_poll_last_ms = 0;
        }
    }
    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/presence")) {
        std::string body, mac, name;
        double rssi = -50;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "mac", mac);
        web_json_get_str(body, "name", name);
        web_json_get_num(body, "rssi", &rssi);
        if (!s_active) { web_resp_error(r, 409, "conflict", "start discovery first"); return true; }
        if (mac.empty()) mac = "02:00:00:00:00:01";
        if (name.empty()) name = "Test Watch";
        {
            std::lock_guard<std::mutex> lk(s_mx);
            peer_seen(mac.c_str(), name.c_str(), (int)rssi);
        }
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/presence") && strncmp(r->path, "/api/presence/", 14)) return false;
    if (tools_is(r, "GET", "/api/presence")) get_status(r);
    else if (tools_is(r, "POST", "/api/presence/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/presence/stop")) stop_monitor(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void presence_second(uint32_t now_ms)
{
    if (s_active && now_ms - s_poll_last_ms >= 5000) {
        s_poll_last_ms = now_ms;
        poll_peers();
    }
}

static void presence_stop(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
}

bool web_hw_presence_start(void)
{
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) return false;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_active) {
        s_active = true;
        s_started = tools_epoch();
        s_peer_count = s_history_count = 0;
        s_peer_total = 0;
        s_error[0] = 0;
        s_poll_last_ms = 0;
    }
    return true;
}
bool web_hw_presence_stop(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
    return true;
}
void web_hw_presence_status(web_hw_presence_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_mx);
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->started_epoch = s_started;
    out->peers_ever = (int)s_peer_total;
    out->peer_count = s_peer_count;
    out->history_count = s_history_count;
    out->error = s_error[0] ? s_error : NULL;
    memcpy(out->peers, s_peers, sizeof(s_peers));
    memcpy(out->history, s_history, sizeof(s_history));
}

static const char *const CAPS[] = { "presence.peers", NULL };
const web_tool_t web_tool_presence = { CAPS, handle, NULL, presence_second, presence_stop };

#else
const web_tool_t web_tool_presence = { NULL, NULL, NULL, NULL, NULL };
#endif
