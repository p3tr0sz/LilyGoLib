/**
 * @file      web_tool_sentinel.cpp
 * @brief     Sentinel (P9 NucleoOS parity): BLE anti-stalking.
 *            Fully passive — scans for BLE devices and flags those that appear persistently
 *            across multiple scan cycles, indicating they may be following the user.
 *
 *            On the watch, uses the NimBLE scanner. In the simulator, devices are injected
 *            through test hooks.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

#define SENTINEL_MAX_DEVICES 24
#define SENTINEL_STALK_THRESHOLD 4       /* flagged after this many sightings in different locations */
#define SENTINEL_MAX_AGE_S 3600          /* forget devices not seen in 1 hour */

static bool s_active = false;
static long long s_started = 0;
static std::mutex s_dev_mx;

typedef struct {
    char addr[18];
    char name[64];
    int rssi;
    long long first_seen;       /* epoch seconds */
    long long last_seen;
    int sightings;
    bool stalking;
} device_t;

static device_t s_devices[SENTINEL_MAX_DEVICES];
static int s_device_count = 0;
static uint32_t s_scan_interval = 5000;  /* ms between scans */

/* For the simulator: inject devices */
#ifndef ARDUINO
static std::vector<device_t> s_pending;
static std::mutex s_pending_mx;
#endif

/* ---------------------------------------------------------------- device tracking */

static int find_device(const char *addr)
{
    for (int i = 0; i < s_device_count; i++)
        if (!strcmp(s_devices[i].addr, addr)) return i;
    return -1;
}

static int add_or_update(const char *addr, const char *name, int rssi, long long now)
{
    int idx = find_device(addr);
    if (idx >= 0) {
        device_t *d = &s_devices[idx];
        d->last_seen = now;
        d->rssi = rssi;
        if (d->name[0] == 0 && name && name[0]) snprintf(d->name, sizeof(d->name), "%s", name);
        d->sightings++;
        if (d->sightings >= SENTINEL_STALK_THRESHOLD && !d->stalking) {
            d->stalking = true;
            /* Publish alert */
            char j[256];
            snprintf(j, sizeof(j), "{\"type\":\"stalking\",\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"sightings\":%d}",
                     d->addr, d->name, d->rssi, d->sightings);
            web_events_publish("sentinel", j);
        }
        return idx;
    }
    if (s_device_count >= SENTINEL_MAX_DEVICES) return -1;
    device_t *d = &s_devices[s_device_count++];
    snprintf(d->addr, sizeof(d->addr), "%s", addr);
    if (name) snprintf(d->name, sizeof(d->name), "%s", name); else d->name[0] = 0;
    d->rssi = rssi;
    d->first_seen = now;
    d->last_seen = now;
    d->sightings = 1;
    d->stalking = false;
    return s_device_count - 1;
}

static void expire_devices(long long now)
{
    int kept = 0;
    for (int i = 0; i < s_device_count; i++) {
        if (now - s_devices[i].last_seen < SENTINEL_MAX_AGE_S)
            s_devices[kept++] = s_devices[i];
    }
    s_device_count = kept;
}

/* ---------------------------------------------------------------- BLE scan integration */

static void run_scan(void)
{
    if (!s_active) return;

    long long now = tools_epoch();
    std::vector<web_hw_ble_t> results;

    /* Use the existing BLE scan hardware */
    if (!web_hw_ble_scan(3, results)) return;

    std::lock_guard<std::mutex> lk(s_dev_mx);
    for (size_t i = 0; i < results.size(); i++) {
        add_or_update(results[i].addr.c_str(), results[i].name.c_str(), results[i].rssi, now);
    }
    expire_devices(now);
}

/* In the simulator, consume injected devices instead */
#ifndef ARDUINO
static void consume_injected(void)
{
    long long now = tools_epoch();
    std::lock_guard<std::mutex> lk(s_pending_mx);
    std::lock_guard<std::mutex> lk2(s_dev_mx);
    for (const device_t &d : s_pending) {
        add_or_update(d.addr, d.name, d.rssi, now);
    }
    s_pending.clear();
    expire_devices(now);
}
#endif

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Sentinel", NULL };

static void get_status(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_dev_mx);
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false");
    if (s_started) j += ",\"started\":" + tools_int(s_started);
    j += ",\"device_count\":" + tools_int(s_device_count);
    j += ",\"devices\":[";
    for (int i = 0; i < s_device_count; i++) {
        if (i) j += ",";
        j += "{\"addr\":";
        web_json_str(j, s_devices[i].addr);
        j += ",\"name\":";
        web_json_str(j, s_devices[i].name);
        j += ",\"rssi\":" + tools_int(s_devices[i].rssi);
        j += ",\"first_seen\":" + tools_int(s_devices[i].first_seen);
        j += ",\"last_seen\":" + tools_int(s_devices[i].last_seen);
        j += ",\"sightings\":" + tools_int(s_devices[i].sightings);
        j += ",\"stalking\":" + std::string(s_devices[i].stalking ? "true" : "false");
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
    std::lock_guard<std::mutex> lk(s_dev_mx);
    s_device_count = 0;

    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    s_active = false;
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/sentinel") && strncmp(r->path, "/api/sentinel/", 14)) return false;
    if (tools_is(r, "GET", "/api/sentinel")) get_status(r);
    else if (tools_is(r, "POST", "/api/sentinel/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/sentinel/stop")) stop_monitor(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/sentinel")) {
        std::string body, addr, name;
        double rssi = -70;
        web_read_body(r, body, 1024);
        web_json_get_str(body, "addr", addr);
        web_json_get_str(body, "name", name);
        web_json_get_num(body, "rssi", &rssi);
        if (addr.empty()) addr = "00:00:00:00:00:01";
        std::lock_guard<std::mutex> lk(s_pending_mx);
        device_t d;
        snprintf(d.addr, sizeof(d.addr), "%s", addr.c_str());
        snprintf(d.name, sizeof(d.name), "%s", name.c_str());
        d.rssi = (int)rssi;
        d.first_seen = d.last_seen = 0;
        d.sightings = 1;
        d.stalking = false;
        s_pending.push_back(d);
        web_resp_ok(r);
        return true;
    }
#endif
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

static uint32_t s_last_scan_ms = 0;

static void sentinel_second(uint32_t now_ms)
{
    if (!s_active) return;
    if (now_ms - s_last_scan_ms >= s_scan_interval) {
        s_last_scan_ms = now_ms;
#ifndef ARDUINO
        consume_injected();
#else
        run_scan();
#endif
    }
}

static void sentinel_stop(void)
{
    s_active = false;
}

static const char *const CAPS[] = { "sentinel", NULL };
const web_tool_t web_tool_sentinel = { CAPS, handle, sentinel_tick, sentinel_second, sentinel_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_sentinel = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */