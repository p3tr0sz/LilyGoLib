/**
 * @file      web_tool_presence.cpp
 * @brief     Presence (P9 NucleoOS parity): ESP-NOW "nearby devices" that extends the existing
 *            Link/ESP-NOW tool with persistent peer tracking, presence events when a peer
 *            comes into or leaves range, and a historical log of seen peers.
 *
 *            Builds on web_hw_link_* from web_tools_hw2.cpp — the same ESP-NOW infrastructure
 *            that the Link tool uses, with additional presence semantics.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>

/* ---------------------------------------------------------------- state */

#define PRESENCE_MAX_PEERS 32
#define PRESENCE_TIMEOUT_S 300          /* 5 min without hello = gone */
#define PRESENCE_HISTORY_MAX 50

static bool s_active = false;
static long long s_started = 0;
static std::mutex s_peers_mx;

typedef struct {
    char mac[18];
    char name[64];
    int rssi;
    long long first_seen;   /* epoch seconds */
    long long last_seen;
    bool present;           /* currently in range */
} presence_peer_t;

static presence_peer_t s_peers[PRESENCE_MAX_PEERS];
static int s_peer_count = 0;
static uint32_t s_peer_total = 0;       /* total unique peers ever seen */

/* History */
typedef struct {
    long long ts;
    char mac[18];
    char name[64];
    char event[16];         /* "appeared", "disappeared" */
} history_entry_t;
static history_entry_t s_history[PRESENCE_HISTORY_MAX];
static int s_history_count = 0;

/* ---------------------------------------------------------------- peer tracking */

static int find_peer(const char *mac)
{
    for (int i = 0; i < s_peer_count; i++)
        if (!strcmp(s_peers[i].mac, mac)) return i;
    return -1;
}

static void add_history(const char *mac, const char *name, const char *event)
{
    if (s_history_count >= PRESENCE_HISTORY_MAX) {
        /* Shift to make room */
        for (int i = 1; i < PRESENCE_HISTORY_MAX; i++)
            s_history[i - 1] = s_history[i];
        s_history_count--;
    }
    history_entry_t *h = &s_history[s_history_count++];
    h->ts = tools_epoch();
    snprintf(h->mac, sizeof(h->mac), "%s", mac);
    snprintf(h->name, sizeof(h->name), "%s", name);
    snprintf(h->event, sizeof(h->event), "%s", event);
}

static void peer_appeared(const char *mac, const char *name, int rssi)
{
    int idx = find_peer(mac);
    if (idx >= 0) {
        presence_peer_t *p = &s_peers[idx];
        if (!p->present) {
            p->present = true;
            p->rssi = rssi;
            p->last_seen = tools_epoch();
            add_history(mac, name, "appeared");
        } else {
            p->last_seen = tools_epoch();
            p->rssi = rssi;
            if (name[0] && p->name[0] == 0) snprintf(p->name, sizeof(p->name), "%s", name);
        }
        return;
    }
    if (s_peer_count >= PRESENCE_MAX_PEERS) return;
    presence_peer_t *p = &s_peers[s_peer_count++];
    snprintf(p->mac, sizeof(p->mac), "%s", mac);
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->rssi = rssi;
    p->first_seen = tools_epoch();
    p->last_seen = p->first_seen;
    p->present = true;
    s_peer_total++;
    add_history(mac, name, "appeared");
    web_events_publish("presence", "{\"peer\":\"appeared\"}");
}

static void check_timeouts(void)
{
    long long now = tools_epoch();
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].present && now - s_peers[i].last_seen > PRESENCE_TIMEOUT_S) {
            s_peers[i].present = false;
            add_history(s_peers[i].mac, s_peers[i].name, "disappeared");
            web_events_publish("presence", "{\"peer\":\"disappeared\"}");
        }
    }
}

/* Update peers from the Link hardware */
static void poll_peers(void)
{
    if (!web_hw_link_enabled()) {
        /* Enable ESP-NOW if not already on */
        web_hw_link_set_enabled(true);
    }

    web_hw_peer_t hw_peers[16];
    int n = web_hw_link_peers(hw_peers, 16);

    std::lock_guard<std::mutex> lk(s_peers_mx);
    for (int i = 0; i < n; i++) {
        peer_appeared(hw_peers[i].mac, hw_peers[i].name, hw_peers[i].rssi);
    }
    check_timeouts();
}

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Presence", NULL };

static void get_status(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_peers_mx);
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false");
    if (s_started) j += ",\"started\":" + tools_int(s_started);
    j += ",\"peers_ever\":" + tools_int(s_peer_total);
    j += ",\"peers\":[";
    for (int i = 0; i < s_peer_count; i++) {
        if (i) j += ",";
        j += "{\"mac\":";
        web_json_str(j, s_peers[i].mac);
        j += ",\"name\":";
        web_json_str(j, s_peers[i].name);
        j += ",\"rssi\":" + tools_int(s_peers[i].rssi);
        j += ",\"first_seen\":" + tools_int(s_peers[i].first_seen);
        j += ",\"last_seen\":" + tools_int(s_peers[i].last_seen);
        j += ",\"present\":" + std::string(s_peers[i].present ? "true" : "false");
        j += "}";
    }
    j += "],\"history\":[";
    for (int i = 0; i < s_history_count; i++) {
        if (i) j += ",";
        j += "{\"ts\":" + tools_int(s_history[i].ts);
        j += ",\"mac\":";
        web_json_str(j, s_history[i].mac);
        j += ",\"name\":";
        web_json_str(j, s_history[i].name);
        j += ",\"event\":";
        web_json_str(j, s_history[i].event);
        j += "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_ok(r); return; }
    s_active = true;
    s_started = tools_epoch();
    std::lock_guard<std::mutex> lk(s_peers_mx);
    s_peer_count = 0;
    s_peer_total = 0;
    s_history_count = 0;
    web_hw_link_set_enabled(true);
    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    s_active = false;
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/presence") && strncmp(r->path, "/api/presence/", 14)) return false;
    if (tools_is(r, "GET", "/api/presence")) get_status(r);
    else if (tools_is(r, "POST", "/api/presence/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/presence/stop")) stop_monitor(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static uint32_t s_poll_last_ms = 0;

static void presence_second(uint32_t now_ms)
{
    if (!s_active) return;
    /* Poll every 5 seconds */
    if (now_ms - s_poll_last_ms >= 5000) {
        s_poll_last_ms = now_ms;
        poll_peers();
    }
}

static void presence_stop(void)
{
    s_active = false;
}

static const char *const CAPS[] = { "presence", NULL };
const web_tool_t web_tool_presence = { CAPS, handle, NULL, presence_second, presence_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_presence = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */