/**
 * @file      web_tool_sentinel.cpp
 * @brief     Passive BLE tracker detection. Flags persistent known trackers;
 *            persistence is a heuristic, not proof of hostile intent.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

#define SENTINEL_MAX_DEVICES 24
#define SENTINEL_FOLLOW_WINDOW_S 300
#define SENTINEL_MIN_SIGHTINGS 4
#define SENTINEL_MAX_AGE_S 3600
#define SENTINEL_SCAN_INTERVAL_MS 10000

struct sentinel_device_t {
    char addr[18];
    char name[64];
    char kind[24];
    int rssi;
    long long first_seen, last_seen;
    int sightings;
    bool stalking, separated;
};

static bool s_active = false, s_scanning = false;
static long long s_started = 0;
static uint32_t s_last_scan_ms = 0;
static char s_error[128] = "";
static sentinel_device_t s_devices[SENTINEL_MAX_DEVICES];
static int s_device_count = 0;
static std::mutex s_dev_mx;
#ifndef ARDUINO
static std::vector<sentinel_device_t> s_pending;
static std::mutex s_pending_mx;
#endif

static void set_error(const char *text)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static const char *classify_tracker(const web_hw_ble_t &d, bool *separated = NULL)
{
    std::string mfr = d.mfr;
    std::transform(mfr.begin(), mfr.end(), mfr.begin(), [](unsigned char c) { return (char)tolower(c); });
    if (separated) *separated = false;
    if (!mfr.compare(0, 6, "4c0012")) {
        if (separated) *separated = true;
        return "Find My";
    }
    if (!mfr.compare(0, 4, "7500")) return "Samsung SmartTag";
    for (const std::string &service : d.services) {
        std::string u = service;
        std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (u.find("fd5a") != std::string::npos || u.find("fd59") != std::string::npos) return "Samsung SmartTag";
        if (u.find("feed") != std::string::npos) return "Tile";
        if (u.find("fe33") != std::string::npos) return "Chipolo";
    }
    return NULL;
}

static int find_device(const char *addr)
{
    for (int i = 0; i < s_device_count; i++) if (!strcasecmp(s_devices[i].addr, addr)) return i;
    return -1;
}

/* s_dev_mx must be held. */
static void add_or_update(const char *addr, const char *name, const char *kind, int rssi,
                          long long now, bool separated, long long first_seen = 0, int prior_sightings = 0)
{
    if (!addr || !*addr || !kind) return;
    int idx = find_device(addr);
    if (idx < 0) {
        if (s_device_count >= SENTINEL_MAX_DEVICES) return;
        idx = s_device_count++;
        memset(&s_devices[idx], 0, sizeof(s_devices[idx]));
        snprintf(s_devices[idx].addr, sizeof(s_devices[idx].addr), "%s", addr);
        s_devices[idx].first_seen = first_seen > 0 ? first_seen : now;
        s_devices[idx].sightings = prior_sightings;
    }
    sentinel_device_t &d = s_devices[idx];
    if (name && *name) snprintf(d.name, sizeof(d.name), "%s", name);
    snprintf(d.kind, sizeof(d.kind), "%s", kind);
    d.last_seen = now;
    d.rssi = rssi;
    d.separated = d.separated || separated;
    d.sightings++;
    uint32_t window = d.separated ? SENTINEL_FOLLOW_WINDOW_S / 2 : SENTINEL_FOLLOW_WINDOW_S;
    if (!d.stalking && d.sightings >= SENTINEL_MIN_SIGHTINGS && now - d.first_seen >= (long long)window) {
        d.stalking = true;
        std::string j = "{\"type\":\"stalking\",\"addr\":";
        web_json_str(j, d.addr);
        j += ",\"name\":";
        web_json_str(j, d.name);
        j += ",\"kind\":";
        web_json_str(j, d.kind);
        j += ",\"rssi\":" + tools_int(d.rssi) +
            ",\"sightings\":" + tools_int(d.sightings) + ",\"detail\":";
        char detail[128];
        snprintf(detail, sizeof(detail), "Persistent %s tracker nearby for %u seconds; check your belongings", d.kind, (unsigned)(now - d.first_seen));
        web_json_str(j, detail);
        j += "}";
        web_events_publish("sentinel", j.c_str());
    }
}

static void expire_devices(long long now)
{
    int kept = 0;
    for (int i = 0; i < s_device_count; i++) {
        if (now - s_devices[i].last_seen < SENTINEL_MAX_AGE_S) {
            if (kept != i) s_devices[kept] = s_devices[i];
            kept++;
        }
    }
    s_device_count = kept;
}

static void scan_task(void *)
{
    std::vector<web_hw_ble_t> results;
    bool ok = web_hw_ble_scan(3, results, false);       /* passive: no scan requests */
    long long now = tools_epoch();
    if (s_active) {
        std::lock_guard<std::mutex> lk(s_dev_mx);
        if (!ok) snprintf(s_error, sizeof(s_error), "%s", "BLE scan failed; retrying");
        else {
            s_error[0] = 0;
            for (const auto &result : results) {
                bool separated = false;
                const char *kind = classify_tracker(result, &separated);
                if (kind) add_or_update(result.addr.c_str(), result.name.c_str(), kind, result.rssi, now, separated);
            }
            expire_devices(now);
        }
    }
    std::lock_guard<std::mutex> lk(s_dev_mx);
    s_scanning = false;
}

#ifndef ARDUINO
static void consume_injected(void)
{
    std::vector<sentinel_device_t> pending;
    {
        std::lock_guard<std::mutex> lk(s_pending_mx);
        pending.swap(s_pending);
    }
    long long now = tools_epoch();
    std::lock_guard<std::mutex> lk(s_dev_mx);
    for (const auto &d : pending) add_or_update(d.addr, d.name, d.kind, d.rssi, now, d.separated, d.first_seen, d.sightings);
    expire_devices(now);
}
#endif

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Sentinel", NULL };

static std::string status_text(void)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"scanning\":" + (s_scanning ? "true" : "false") +
        ",\"started\":" + tools_int(s_started) +
        ",\"device_count\":" + tools_int(s_device_count) + ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += ",\"devices\":[";
    for (int i = 0; i < s_device_count; i++) {
        if (i) j += ",";
        j += "{\"addr\":";
        web_json_str(j, s_devices[i].addr);
        j += ",\"name\":";
        web_json_str(j, s_devices[i].name);
        j += ",\"kind\":";
        web_json_str(j, s_devices[i].kind);
        j += ",\"rssi\":" + tools_int(s_devices[i].rssi) +
            ",\"first_seen\":" + tools_int(s_devices[i].first_seen) +
            ",\"last_seen\":" + tools_int(s_devices[i].last_seen) +
            ",\"sightings\":" + tools_int(s_devices[i].sightings) +
            ",\"stalking\":" + (s_devices[i].stalking ? "true" : "false") +
            ",\"separated\":" + (s_devices[i].separated ? "true" : "false") + "}";
    }
    return j + "]}";
}

static void get_status(web_req_t *r) { web_resp_json(r, 200, status_text()); }

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_ok(r); return; }
    {
        std::lock_guard<std::mutex> lk(s_dev_mx);
        s_active = true;
        s_started = tools_epoch();
        s_device_count = 0;
        s_error[0] = 0;
        s_last_scan_ms = 0;
    }
    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    s_active = false;
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/sentinel")) {
        std::string body, addr, name, kind;
        double rssi = -70, sightings = 1, age_s = 0;
        bool separated = false;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "addr", addr);
        web_json_get_str(body, "name", name);
        web_json_get_str(body, "kind", kind);
        web_json_get_num(body, "rssi", &rssi);
        web_json_get_num(body, "sightings", &sightings);
        web_json_get_num(body, "age_s", &age_s);
        web_json_get_bool(body, "separated", &separated);
        if (!s_active) { web_resp_error(r, 409, "conflict", "start monitoring first"); return true; }
        if (addr.empty()) addr = "00:00:00:00:00:01";
        if (kind.empty()) kind = "Find My";
        std::lock_guard<std::mutex> lk(s_pending_mx);
        sentinel_device_t d = {};
        snprintf(d.addr, sizeof(d.addr), "%s", addr.c_str());
        snprintf(d.name, sizeof(d.name), "%s", name.c_str());
        snprintf(d.kind, sizeof(d.kind), "%s", kind.c_str());
        d.rssi = (int)rssi;
        d.separated = separated;
        d.sightings = (int)sightings > 1 ? (int)sightings - 1 : 0;
        d.first_seen = tools_epoch() - (long long)age_s;
        s_pending.push_back(d);
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/sentinel") && strncmp(r->path, "/api/sentinel/", 14)) return false;
    if (tools_is(r, "GET", "/api/sentinel")) get_status(r);
    else if (tools_is(r, "POST", "/api/sentinel/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/sentinel/stop")) stop_monitor(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void sentinel_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;
#ifndef ARDUINO
    consume_injected();
#endif
}

static void sentinel_second(uint32_t now_ms)
{
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(s_dev_mx);
        if (s_active && !s_scanning && now_ms - s_last_scan_ms >= SENTINEL_SCAN_INTERVAL_MS) {
            s_last_scan_ms = now_ms;
            s_scanning = true;
            spawn = true;
        }
    }
    if (spawn) {
#ifdef ARDUINO
        web_plat_spawn(scan_task, NULL);
#else
        scan_task(NULL);
#endif
    }
}

static void sentinel_stop(void)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    s_active = false;
}

bool web_hw_sentinel_start(void)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    if (!s_active) {
        s_active = true;
        s_started = tools_epoch();
        s_device_count = 0;
        s_error[0] = 0;
        s_last_scan_ms = 0;
    }
    return true;
}
bool web_hw_sentinel_stop(void)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    s_active = false;
    return true;
}
void web_hw_sentinel_status(web_hw_sentinel_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_dev_mx);
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->scanning = s_scanning;
    out->device_count = s_device_count;
    out->started_epoch = s_started;
    out->error = s_error[0] ? s_error : NULL;
    for (int i = 0; i < s_device_count; i++) {
        snprintf(out->devices[i].addr, sizeof(out->devices[i].addr), "%s", s_devices[i].addr);
        snprintf(out->devices[i].name, sizeof(out->devices[i].name), "%s", s_devices[i].name);
        snprintf(out->devices[i].kind, sizeof(out->devices[i].kind), "%s", s_devices[i].kind);
        out->devices[i].rssi = s_devices[i].rssi;
        out->devices[i].first_seen = s_devices[i].first_seen;
        out->devices[i].last_seen = s_devices[i].last_seen;
        out->devices[i].sightings = s_devices[i].sightings;
        out->devices[i].stalking = s_devices[i].stalking;
        out->devices[i].separated = s_devices[i].separated;
    }
}

static const char *const CAPS[] = { "sentinel", NULL };
const web_tool_t web_tool_sentinel = { CAPS, handle, sentinel_tick, sentinel_second, sentinel_stop };

#else
const web_tool_t web_tool_sentinel = { NULL, NULL, NULL, NULL, NULL };
#endif
