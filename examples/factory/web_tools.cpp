/**
 * @file      web_tools.cpp
 * @brief     Tools registry (web_tools.h): which tools this board has, their capabilities,
 *            routing to them, the timer that drives them, and the helpers they share.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <mutex>

#define TOOLS_TICK_MS 50

/* The tools this board has. A tool left out here has no routes (404) and no capability. */
static const web_tool_t *const TOOLS[] = {
    &web_tool_monitor,
    &web_tool_notify,
#if FACTORY_HAS_MOTION_SENSOR
    &web_tool_sensors,
#endif
#if FACTORY_HAS_GPS
    &web_tool_gnss,
#endif
#if FACTORY_HAS_RADIO
    &web_tool_lora,
#endif
#if FACTORY_HAS_NFC
    &web_tool_nfc,
#endif
#if FACTORY_HAS_MICROPHONE
    &web_tool_recorder,
#endif
#if FACTORY_HAS_AUDIO_OUT
    &web_tool_audio,
#endif
    &web_tool_wifi_scan,
    &web_tool_ble_scan,
    &web_tool_link,
    &web_tool_hid,
    &web_tool_net,
    &web_tool_ota,
    /* P9 — NucleoOS parity tools */
    &web_tool_sniffer,
    &web_tool_airspace,
    &web_tool_sentinel,
    &web_tool_presence,
    &web_tool_fido2,
    &web_tool_evilportal,
    &web_tool_deauthflood,
    &web_tool_beaconspam,
};

static lv_timer_t *s_timer = NULL;
static std::mutex s_app_mx;
static char s_dev_app[32] = "";         /* /api/_dev/app: an app "open" on the watch (tests) */

/* ---------------------------------------------------------------- registry */

void web_tools_caps(std::vector<const char *> &out)
{
    for (const web_tool_t *t : TOOLS) {
        for (const char *const *c = t->caps; c && *c; c++) out.push_back(*c);
    }
}

bool web_tools_handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/app")) {         /* simulator only: see docs/web-api.md, Test hooks */
        std::string body, name;
        web_read_body(r, body, 256);
        web_json_get_str(body, "name", name);
        {
            std::lock_guard<std::mutex> lk(s_app_mx);
            snprintf(s_dev_app, sizeof(s_dev_app), "%s", name.c_str());
        }
        web_resp_ok(r);
        return true;
    }
#endif
    for (const web_tool_t *t : TOOLS) {
        if (t->handle(r)) return true;
    }
    return false;
}

static void tools_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    static uint32_t last_second = 0;
    uint32_t now = lv_tick_get();
    for (const web_tool_t *t : TOOLS) {
        if (t->tick) t->tick(now);
    }
    if (now - last_second < 1000) return;
    last_second = now;
    for (const web_tool_t *t : TOOLS) {
        if (t->second) t->second(now);
    }
}

void web_tools_start(void)
{
    if (!s_timer) s_timer = lv_timer_create(tools_timer_cb, TOOLS_TICK_MS, NULL);
}

void web_tools_stop(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    for (const web_tool_t *t : TOOLS) {
        if (t->stop) t->stop();
    }
}

/* ---------------------------------------------------------------- who holds a tool */

const char *tools_current_app(void)
{
    {
        std::lock_guard<std::mutex> lk(s_app_mx);
        if (s_dev_app[0]) return s_dev_app;
    }
    return web_hw_current_app();
}

const char *tools_holder(const char *const *apps)
{
    const char *app = tools_current_app();
    if (!app || !*app) return NULL;
    for (int i = 0; apps[i]; i++) {
        if (!strcmp(app, apps[i])) return apps[i];
    }
    return NULL;
}

bool tools_busy(web_req_t *r, const char *const *apps)
{
    const char *app = tools_holder(apps);
    if (!app) return false;
    std::string extra = "\"app\":";
    web_json_str(extra, app);
    char msg[96];
    snprintf(msg, sizeof(msg), "Close %s on the watch to use it here", app);
    web_resp_error(r, 503, "busy", msg, extra.c_str());
    return true;
}

/* ---------------------------------------------------------------- helpers */

uint32_t tools_now_ms(void)
{
    return lv_tick_get();
}

long long tools_epoch(void)
{
    struct tm t;
    hw_get_date_time(t);
    t.tm_isdst = -1;
    return (long long)mktime(&t);
}

std::string tools_int(long long v)
{
    char b[24];
    snprintf(b, sizeof(b), "%lld", v);
    return b;
}

std::string tools_num(double v, int decimals)
{
    if (isnan(v) || isinf(v)) return "null";
    char b[48];
    snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

bool tools_is(const web_req_t *r, const char *method, const char *path)
{
    return !strcmp(r->method, method) && !strcmp(r->path, path);
}

bool tools_read_json(web_req_t *r, std::string &body, size_t max)
{
    if (!web_read_body(r, body, max)) {
        web_resp_error(r, 400, "bad_request", "body too large");
        return false;
    }
    if (body.empty()) body = "{}";
    if (!web_json_is_object(body)) {
        web_resp_error(r, 400, "bad_request", "bad JSON");
        return false;
    }
    return true;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string tools_b64_encode(const uint8_t *d, size_t n)
{
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += B64[(v >> 18) & 63];
        o += B64[(v >> 12) & 63];
        o += i + 1 < n ? B64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}

bool tools_b64_decode(const std::string &s, std::string &out)
{
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const char *p = c ? strchr(B64, c) : NULL;
        if (!p) return false;
        v = v << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((v >> bits) & 0xFF);
        }
    }
    return true;
}

#endif /* FACTORY_WEB */
