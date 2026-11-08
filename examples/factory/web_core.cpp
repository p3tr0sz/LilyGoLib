/**
 * @file      web_core.cpp
 * @brief     Web API v1 core: routing, pairing, device, time, settings, SD, events, static files.
 *            Platform-independent; see web_internal.h for the transport and platform hooks.
 */
#include "web_internal.h"

#if FACTORY_WEB

#include "ui_define.h"          /* the watch's theme: ui_active_accent, ui_theme_presets */

#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <memory>
#include <atomic>

#define WEB_PORT_DEVICE         80
#define WEB_PORT_SIMULATOR      8080
#define WEB_MAX_SESSIONS        8
#define WEB_SESSION_TTL_S       (90L * 24 * 3600)
#define WEB_PIN_MAX_MISSES      5
#define WEB_PIN_LOCK_MS         30000
#define WEB_STATIC_CHUNK        8192

static const char *const WEB_CAPS[] = { "files", "files.flash", "sd", "settings", "time", "screen", "input", "display", "desktop.install" };

/* ---------------------------------------------------------------- state */

struct web_session_t {
    char token[49];
    int64_t last_used;      /* epoch seconds */
};

static std::recursive_mutex s_lock;
static bool s_running = false;
static char s_pin[7] = "000000";
static std::vector<web_session_t> s_sessions;
static int s_misses = 0;
static uint32_t s_locked_until = 0;
static uint32_t s_seq = 0;
static int s_seat = -1;                  /* connection id of the desktop's event socket */

/* In-process event tap: sinks that receive every published event, seat or no seat (headless
 * automation). A tiny fixed registry — sinks are registered once at startup, never removed. */
#define WEB_EVENT_SINKS_MAX 4
struct web_event_sub_t { web_event_sink_t cb; void *user; };
static web_event_sub_t s_event_sinks[WEB_EVENT_SINKS_MAX];
static int s_event_sink_count = 0;

static char s_tz[48] = "UTC";
static int s_offset_min = 0;
static bool s_time_set = false;
static uint32_t s_idle_off_s = 600;

/* Snapshots refreshed on the UI thread by the monitor timer; read by request handlers. */
static web_dev_info_t s_dev;
static web_sd_info_t s_sd;
static bool s_sd_user_unmounted = false;
static bool s_card_seen = false;        /* the desktop check has seen this card mounted */
static bool s_have_snapshot = false;

/* The watch theme the desktop follows: accent, accent_dim, card, track (0xRRGGBB). */
struct web_theme_t { uint32_t accent, dim, card, track; };
static web_theme_t s_theme = { 0x00D4AA, 0x009977, 0x1A1A1A, 0x333333 };

static std::string theme_json(const web_theme_t &t)
{
    const char *name = "";
    for (size_t i = 0; i < UI_THEME_PRESET_COUNT; i++) {
        if (ui_theme_presets[i].accent == t.accent && ui_theme_presets[i].card_bg == t.card) name = ui_theme_presets[i].name;
    }
    char b[160];
    snprintf(b, sizeof(b), "{\"accent\":\"#%06x\",\"accent_dim\":\"#%06x\",\"card\":\"#%06x\",\"track\":\"#%06x\",\"name\":",
             (unsigned)t.accent, (unsigned)t.dim, (unsigned)t.card, (unsigned)t.track);
    std::string j = b;
    web_json_str(j, name);
    return j + "}";
}

static bool s_pairing_open = false;
static std::atomic<uint32_t> s_last_request_ms { 0 };

static lv_timer_t *s_ui_timer = NULL;
static lv_timer_t *s_monitor_timer = NULL;

static uint32_t now_ms(void)
{
    return lv_tick_get();
}

/* ---------------------------------------------------------------- UI thread queue */

struct ui_job_t {
    std::function<void()> fn;
    bool done = false;
};
static std::mutex s_ui_mutex;
static std::condition_variable s_ui_cv;
static std::deque<std::shared_ptr<ui_job_t>> s_ui_jobs;

bool web_ui_run(std::function<void()> fn, uint32_t timeout_ms)
{
    auto job = std::make_shared<ui_job_t>();
    job->fn = std::move(fn);
    std::unique_lock<std::mutex> lk(s_ui_mutex);
    s_ui_jobs.push_back(job);
    return s_ui_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return job->done; });
}

static void ui_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    for (;;) {
        std::shared_ptr<ui_job_t> job;
        {
            std::lock_guard<std::mutex> lk(s_ui_mutex);
            if (s_ui_jobs.empty()) break;
            job = s_ui_jobs.front();
            s_ui_jobs.pop_front();
        }
        job->fn();
        {
            std::lock_guard<std::mutex> lk(s_ui_mutex);
            job->done = true;
        }
        s_ui_cv.notify_all();
    }
}

/* ---------------------------------------------------------------- helpers */

static std::string sd_json(const web_sd_info_t &sd)
{
    std::string j = "{\"present\":";
    j += sd.present ? "true" : "false";
    j += ",\"mounted\":";
    j += sd.mounted ? "true" : "false";
    j += ",\"fs\":";
    j += sd.mounted ? "\"FAT32\"" : "null";
    j += ",\"card\":";
    if (sd.card) web_json_str(j, sd.card); else j += "null";
    char b[64];
    snprintf(b, sizeof(b), ",\"total\":%llu,\"free\":%llu", (unsigned long long)sd.total, (unsigned long long)sd.free);
    j += b;
    j += ",\"error\":";
    if (sd.error) web_json_str(j, sd.error); else j += "null";
    j += "}";
    return j;
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t clock_epoch(void)
{
    struct tm t;
    hw_get_date_time(t);
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

static void read_sd_snapshot(void)
{
    web_sd_info_t sd;
    web_plat_sd_info(&sd, true);
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    s_sd = sd;
}

/* ---------------------------------------------------------------- persistence (<flash>/.web/) */

static std::string state_path(const char *name)
{
    const char *dir = web_plat_root_dir("flash");
    return std::string(dir ? dir : ".") + "/.web/" + name;
}

static void save_file_atomic(const std::string &path, const std::string &data)
{
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0775);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    remove(path.c_str());               /* FAT's rename does not replace */
    rename(tmp.c_str(), path.c_str());
}

static void save_state(void)
{
    std::string s;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        char line[96];
        for (auto &x : s_sessions) {
            snprintf(line, sizeof(line), "session %s %lld\n", x.token, (long long)x.last_used);
            s += line;
        }
        snprintf(line, sizeof(line), "idle %u\n", (unsigned)s_idle_off_s);
        s += line;
        snprintf(line, sizeof(line), "offset %d\n", s_offset_min);
        s += line;
        s += "tz ";
        s += s_tz;
        s += "\n";
    }
    save_file_atomic(state_path("state.txt"), s);
}

static void load_state(void)
{
    FILE *f = fopen(state_path("state.txt").c_str(), "rb");
    if (!f) return;
    char line[160];
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    s_sessions.clear();
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "session ", 8)) {
            web_session_t x = {};
            long long used = 0;
            if (sscanf(line + 8, "%48s %lld", x.token, &used) == 2 && strlen(x.token) == 48) {
                x.last_used = used;
                s_sessions.push_back(x);
            }
        } else if (!strncmp(line, "idle ", 5)) {
            s_idle_off_s = (uint32_t)strtoul(line + 5, NULL, 10);
        } else if (!strncmp(line, "offset ", 7)) {
            s_offset_min = atoi(line + 7);
        } else if (!strncmp(line, "tz ", 3)) {
            snprintf(s_tz, sizeof(s_tz), "%s", line + 3);
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- answers */

void web_resp_json(web_req_t *r, int status, const std::string &json, const char *headers)
{
    std::string h = "Cache-Control: no-store\r\n";
    if (headers) h += headers;
    if (r->begin(r, status, "application/json", h.c_str(), (long long)json.size())) {
        r->write(r, json.data(), json.size());
    }
    r->end(r);
}

void web_resp_error(web_req_t *r, int status, const char *code, const char *message, const char *extra_json)
{
    std::string j = "{\"error\":";
    web_json_str(j, code);
    if (message) {
        j += ",\"message\":";
        web_json_str(j, message);
    }
    if (extra_json && *extra_json) {
        j += ",";
        j += extra_json;
    }
    j += "}";
    web_resp_json(r, status, j);
}

void web_resp_ok(web_req_t *r, const char *extra_json)
{
    std::string j = "{\"ok\":true";
    if (extra_json && *extra_json) {
        j += ",";
        j += extra_json;
    }
    j += "}";
    web_resp_json(r, 200, j);
}

void web_resp_html(web_req_t *r, int status, const char *html)
{
    const char *h = "Cache-Control: no-store\r\n";
    (void)r->begin(r, status, "text/html; charset=utf-8", h, (long long)strlen(html));
    r->write(r, html, strlen(html));
    r->end(r);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool web_query_get(const web_req_t *r, const char *name, char *out, size_t out_len)
{
    size_t nlen = strlen(name);
    const char *p = r->query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg >= nlen && !strncmp(p, name, nlen) && (seg == nlen || p[nlen] == '=')) {
            const char *v = seg == nlen ? p + seg : p + nlen + 1;
            const char *e = p + seg;
            size_t o = 0;
            while (v < e && o + 1 < out_len) {
                if (*v == '%' && v + 2 < e + 1 && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

bool web_read_body(web_req_t *r, std::string &out, size_t max)
{
    out.clear();
    if (r->content_length > (long long)max) return false;
    char buf[512];
    for (;;) {
        int n = r->read(r, buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) return true;
        out.append(buf, (size_t)n);
        if (out.size() > max) return false;
    }
}

/* ---------------------------------------------------------------- pairing */

static bool token_from_cookie(const char *cookie, char *out, size_t n)
{
    const char *p = cookie;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, "session=", 8)) {
            p += 8;
            size_t i = 0;
            while (p[i] && p[i] != ';' && p[i] != ' ' && i + 1 < n) {
                out[i] = p[i];
                i++;
            }
            out[i] = 0;
            return i > 0;
        }
        p = strchr(p, ';');
    }
    return false;
}

static int find_session(const char *cookie)
{
    char token[64];
    if (!token_from_cookie(cookie, token, sizeof(token))) return -1;
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    int64_t now = clock_epoch();
    for (size_t i = 0; i < s_sessions.size(); i++) {
        if (!strcmp(s_sessions[i].token, token)) {
            if (now > 1700000000 && s_sessions[i].last_used > 1700000000 &&
                    now - s_sessions[i].last_used > WEB_SESSION_TTL_S) {
                s_sessions.erase(s_sessions.begin() + i);
                return -1;
            }
            if (now > s_sessions[i].last_used) s_sessions[i].last_used = now;
            return (int)i;
        }
    }
    return -1;
}

static bool is_paired(const web_req_t *r)
{
    return find_session(r->cookie) >= 0;
}

static void new_pin(void)
{
    uint8_t b[4];
    web_plat_random(b, sizeof(b));
    uint32_t v = ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]) % 1000000;
    snprintf(s_pin, sizeof(s_pin), "%06u", (unsigned)v);
}

static void handle_pair(web_req_t *r)
{
    uint32_t now = now_ms();
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (!s_pairing_open) {
            web_resp_error(r, 403, "pairing_closed", "Open Settings > Web desktop on the watch to see the PIN");
            return;
        }
        if (s_locked_until && (int32_t)(s_locked_until - now) > 0) {
            char extra[48];
            snprintf(extra, sizeof(extra), "\"retry_ms\":%u", (unsigned)(s_locked_until - now));
            web_resp_error(r, 429, "locked", "too many tries", extra);
            return;
        }
    }
    std::string body, pin;
    if (!web_read_body(r, body, 1024) || !web_json_get_str(body, "pin", pin) || pin != s_pin) {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (++s_misses >= WEB_PIN_MAX_MISSES) {
            s_misses = 0;
            s_locked_until = now + WEB_PIN_LOCK_MS;
        }
        web_resp_error(r, 401, "bad_pin", "wrong PIN");
        return;
    }
    web_session_t x = {};
    uint8_t raw[24];
    web_plat_random(raw, sizeof(raw));
    for (int i = 0; i < 24; i++) snprintf(x.token + i * 2, 3, "%02x", raw[i]);
    x.last_used = clock_epoch();
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_misses = 0;
        if (s_sessions.size() >= WEB_MAX_SESSIONS) {
            size_t oldest = 0;
            for (size_t i = 1; i < s_sessions.size(); i++) {
                if (s_sessions[i].last_used < s_sessions[oldest].last_used) oldest = i;
            }
            s_sessions.erase(s_sessions.begin() + oldest);
        }
        s_sessions.push_back(x);
    }
    save_state();
    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "Set-Cookie: session=%s; Path=/; Max-Age=7776000; HttpOnly; SameSite=Strict\r\n", x.token);
    web_resp_json(r, 200, "{\"ok\":true}", cookie);
}

static void handle_logout(web_req_t *r)
{
    std::string body;
    web_read_body(r, body, 1024);
    bool all = false;
    web_json_get_bool(body, "all", &all);
    char token[64] = "";
    token_from_cookie(r->cookie, token, sizeof(token));
    int revoked = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        for (size_t i = 0; i < s_sessions.size();) {
            if (all || !strcmp(s_sessions[i].token, token)) {
                s_sessions.erase(s_sessions.begin() + i);
                revoked++;
            } else {
                i++;
            }
        }
    }
    save_state();
    char extra[32];
    snprintf(extra, sizeof(extra), "\"revoked\":%d", revoked);
    web_resp_ok(r, extra);
}

/* ---------------------------------------------------------------- system routes */

static std::string caps_array(void)
{
    std::vector<const char *> caps(WEB_CAPS, WEB_CAPS + sizeof(WEB_CAPS) / sizeof(WEB_CAPS[0]));
    web_tools_caps(caps);
    std::string j = "[";
    for (size_t i = 0; i < caps.size(); i++) {
        if (i) j += ",";
        web_json_str(j, caps[i]);
    }
    return j + "]";
}

static void handle_caps(web_req_t *r)
{
    std::string j = "{\"api\":1,\"board\":\"twatch-ultra\",\"fw\":";
    web_json_str(j, web_plat_fw_version());
    j += ",\"caps\":" + caps_array() + "}";
    web_resp_json(r, 200, j);
}

static void handle_device(web_req_t *r)
{
    bool paired = is_paired(r);
    web_dev_info_t d;
    web_sd_info_t sd;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        d = s_dev;
        sd = s_sd;
    }
    char b[256];
    std::string j = "{\"board\":\"twatch-ultra\",\"fw\":";
    web_json_str(j, web_plat_fw_version());
    snprintf(b, sizeof(b), ",\"battery\":{\"percent\":%d,\"charging\":%s,\"mv\":%d}",
             d.percent, d.charging ? "true" : "false", d.mv);
    j += b;
    web_theme_t theme;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        theme = s_theme;
    }
    j += ",\"theme\":" + theme_json(theme);
    if (!paired) {
        j += ",\"sd\":{\"present\":";
        j += sd.present ? "true" : "false";
        j += "}}";
        web_resp_json(r, 200, j);
        return;
    }
    snprintf(b, sizeof(b), ",\"uptime_s\":%u", (unsigned)(now_ms() / 1000));
    j += b;
    j += ",\"sd\":{\"present\":";
    j += sd.present ? "true" : "false";
    j += ",\"mounted\":";
    j += sd.mounted ? "true" : "false";
    snprintf(b, sizeof(b), "},\"flash\":{\"total\":%llu,\"free\":%llu}",
             (unsigned long long)d.flash_total, (unsigned long long)d.flash_free);
    j += b;
    j += ",\"net\":{\"mode\":";
    web_json_str(j, d.mode);
    j += ",\"ssid\":";
    web_json_str(j, d.ssid);
    j += ",\"ip\":";
    web_json_str(j, d.ip);
    snprintf(b, sizeof(b), ",\"rssi\":%d,\"host\":\"watch.local\"}", d.rssi);
    j += b;
    struct tm t;
    hw_get_date_time(t);
    struct tm tl = t;
    tl.tm_isdst = -1;
    int64_t epoch = (int64_t)mktime(&tl);
    /* The offset the watch really shows: its local time read as UTC, minus the epoch. */
    int offset = (int)((days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400LL +
                        t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec - epoch) / 60);
    bool synced;
    char tz[48];
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        synced = s_time_set || t.tm_year + 1900 >= 2025;
        snprintf(tz, sizeof(tz), "%s", s_tz);
    }
    snprintf(b, sizeof(b), ",\"time\":{\"epoch\":%lld,\"offset_min\":%d,\"synced\":%s,\"tz\":",
             (long long)epoch, offset, synced ? "true" : "false");
    j += b;
    web_json_str(j, tz);
    snprintf(b, sizeof(b), "},\"mem\":{\"heap_free\":%u,\"psram_free\":%u}}",
             (unsigned)d.heap_free, (unsigned)d.psram_free);
    j += b;
    web_resp_json(r, 200, j);
}

static void handle_time(web_req_t *r)
{
    std::string body, tz;
    double epoch = 0, offset = 0;
    if (!web_read_body(r, body, 1024) || !web_json_get_num(body, "epoch", &epoch) || epoch < 0) {
        web_resp_error(r, 400, "bad_request", "epoch");
        return;
    }
    bool has_offset = web_json_get_num(body, "offset_min", &offset);
    if (has_offset && (offset < -14 * 60 || offset > 14 * 60)) {
        web_resp_error(r, 400, "bad_request", "offset_min");
        return;
    }
    bool has_tz = web_json_get_str(body, "tz", tz) && !tz.empty() && tz.size() < 48;
    bool ok = false;
    web_ui_run([&] {
        if (has_offset) hw_set_timezone_offset((int32_t)offset * 60, 0);
        time_t e = (time_t)epoch;
        struct tm t;
        localtime_r(&e, &t);
        ok = hw_set_date_time(t);
        hw_write_rtc_from_system_time();
    });
    if (!ok) {
        web_resp_error(r, 500, "internal", "could not set the clock");
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_time_set = true;
        if (has_offset) s_offset_min = (int)offset;
        if (has_tz) snprintf(s_tz, sizeof(s_tz), "%s", tz.c_str());
    }
    save_state();
    web_resp_ok(r);
}

/* Brightness 0-100 on the API, the display's level range on the watch. */
static int level_to_pct(int level)
{
    int lo = hw_get_disp_min_brightness(), hi = hw_get_disp_max_brightness();
    if (hi <= lo) return 100;
    int p = (level - lo) * 100 / (hi - lo);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static int pct_to_level(int pct)
{
    int lo = hw_get_disp_min_brightness(), hi = hw_get_disp_max_brightness();
    return lo + (hi - lo) * pct / 100;
}

static std::string settings_json(void)
{
    user_setting_params_t s;
    web_ui_run([&] { hw_get_user_setting(s); });
    char b[160];
    uint32_t idle;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        idle = s_idle_off_s;
    }
    snprintf(b, sizeof(b), "{\"brightness\":%d,\"screen_timeout_s\":%u,\"web_idle_off_s\":%u}",
             level_to_pct(s.brightness_level), (unsigned)s.disp_timeout_second, (unsigned)idle);
    return b;
}

static void handle_settings_patch(web_req_t *r)
{
    std::string body;
    if (!web_read_body(r, body, 2048) || !web_json_is_object(body)) {
        web_resp_error(r, 400, "bad_request", "bad JSON");
        return;
    }
    double v;
    int brightness = -1, timeout = -1;
    long idle = -1;
    if (web_json_has(body, "brightness")) {
        if (!web_json_get_num(body, "brightness", &v) || v < 0 || v > 100) {
            web_resp_error(r, 400, "bad_request", "brightness 0-100");
            return;
        }
        brightness = (int)v;
    }
    if (web_json_has(body, "screen_timeout_s")) {
        if (!web_json_get_num(body, "screen_timeout_s", &v) || v < 0 || v > 255) {
            web_resp_error(r, 400, "bad_request", "screen_timeout_s 0-255");
            return;
        }
        timeout = (int)v;
    }
    if (web_json_has(body, "web_idle_off_s")) {
        if (!web_json_get_num(body, "web_idle_off_s", &v) || v < 0 || v > 86400) {
            web_resp_error(r, 400, "bad_request", "web_idle_off_s 0-86400");
            return;
        }
        idle = (long)v;
    }
    web_ui_run([&] {
        user_setting_params_t s;
        hw_get_user_setting(s);
        if (brightness >= 0) {
            s.brightness_level = (uint8_t)pct_to_level(brightness);
            hw_set_disp_backlight(s.brightness_level);
        }
        if (timeout >= 0) s.disp_timeout_second = (uint8_t)timeout;
        hw_set_user_setting(s);
    });
    if (idle >= 0) {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_idle_off_s = (uint32_t)idle;
    }
    if (idle >= 0) save_state();
    std::string j = settings_json();
    web_events_publish("settings.changed", j.c_str());
    web_resp_json(r, 200, j);
}

/* ---------------------------------------------------------------- SD routes */

static void publish_sd(void)
{
    web_sd_info_t sd;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        sd = s_sd;
    }
    web_events_publish("sd.changed", sd_json(sd).c_str());
}

bool web_fs_sd_mounted(void)
{
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    return s_sd.mounted;
}

static void handle_sd_mount(web_req_t *r, bool mount)
{
    bool ok = true;
    web_ui_run([&] {
        if (mount) {
            web_sd_info_t sd;
            web_plat_sd_info(&sd, false);
            ok = sd.present && (sd.mounted || web_plat_sd_mount());
            if (ok) s_card_seen = false;
        } else {
            web_plat_sd_unmount();
        }
        read_sd_snapshot();
    }, 10000);
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_sd_user_unmounted = !mount;
    }
    publish_sd();
    if (!ok) {
        web_resp_error(r, 503, "sd_missing", "no card, or it could not be mounted");
        return;
    }
    web_resp_ok(r);
}

/* ---------------------------------------------------------------- events */

void web_events_publish(const char *topic, const char *d_json)
{
    std::string msg = "{\"t\":";
    web_json_str(msg, topic);
    int seat;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        char b[32];
        snprintf(b, sizeof(b), ",\"seq\":%u,\"d\":", (unsigned)++s_seq);
        msg += b;
        seat = s_seat;
    }
    msg += d_json && *d_json ? d_json : "{}";
    msg += "}";
    if (seat >= 0) web_transport_ws_send(seat, msg.data(), msg.size());
    /* Headless tap: also deliver to in-process sinks, even when no desktop holds the seat. */
    web_events_dispatch_local(topic, d_json && *d_json ? d_json : "{}");
}

void web_events_subscribe(web_event_sink_t cb, void *user)
{
    if (!cb) return;
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    if (s_event_sink_count >= WEB_EVENT_SINKS_MAX) return;   /* silently ignore overflow */
    s_event_sinks[s_event_sink_count++] = { cb, user };
}

void web_events_dispatch_local(const char *topic, const char *d_json)
{
    /* Snapshot the registry under the lock, then call the sinks unlocked: a sink must only enqueue,
     * but calling it outside s_lock keeps a misbehaving sink from deadlocking or re-entering us. */
    web_event_sub_t subs[WEB_EVENT_SINKS_MAX];
    int n;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        n = s_event_sink_count;
        for (int i = 0; i < n; i++) subs[i] = s_event_sinks[i];
    }
    for (int i = 0; i < n; i++) subs[i].cb(topic, d_json ? d_json : "{}", subs[i].user);
}

#ifndef ARDUINO
/* Step-1 headless-tap proof (simulator only, docs/automation-rule-engine.md "B step 1"): a count
 * sink + the /api/_dev/emit hook below show that a published event reaches an in-process sink even
 * with no desktop WebSocket holding the seat. A test fixture — the real automation module registers
 * its own sink. Never built for the watch. */
static std::atomic<uint32_t> s_tap_count{0};
static char s_tap_last_topic[64] = "";
static void dev_tap_sink(const char *topic, const char *d_json, void *user)
{
    (void)d_json; (void)user;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        snprintf(s_tap_last_topic, sizeof(s_tap_last_topic), "%s", topic ? topic : "");
    }
    s_tap_count.fetch_add(1, std::memory_order_relaxed);
}
#endif

bool web_api_ws_open(int conn, const char *cookie)
{
    if (find_session(cookie) < 0) return false;
    int old;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        old = s_seat;
        s_seat = conn;
    }
    if (old >= 0 && old != conn) {
        std::string m = "{\"t\":\"seat.lost\",\"seq\":0,\"d\":{}}";
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            char b[64];
            snprintf(b, sizeof(b), "{\"t\":\"seat.lost\",\"seq\":%u,\"d\":{}}", (unsigned)++s_seq);
            m = b;
        }
        web_transport_ws_send(old, m.data(), m.size());
        web_transport_ws_close(old);
    }
    std::string hello = "{\"t\":\"hello\",\"seq\":0,\"d\":{\"api\":1,\"caps\":" + caps_array() + "}}";
    web_transport_ws_send(conn, hello.data(), hello.size());
    return true;
}

void web_api_ws_closed(int conn)
{
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    if (s_seat == conn) s_seat = -1;
}

/* ---------------------------------------------------------------- monitor (UI thread) */

static void monitor_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    web_plat_net_tick(s_running);

    /* Turn the server off after the idle time with no desktop and no request. */
    if (t && s_running) {
        uint32_t idle_s;
        int seat;
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            idle_s = s_idle_off_s;
            seat = s_seat;
        }
        if (idle_s && seat < 0 && !s_pairing_open &&
                lv_tick_diff(now_ms(), s_last_request_ms.load()) > idle_s * 1000UL) {
            web_plat_log("[web] no browser for %u s: server off", (unsigned)idle_s);
            web_server_stop();
            return;
        }
    }

    web_dev_info_t d;
    web_plat_dev_info(&d);
    web_sd_info_t sd;
    web_plat_sd_info(&sd, false);

    /* Card detection: mount on insert, unmount on removal. */
    bool user_unmounted;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        if (!sd.present) s_sd_user_unmounted = false;
        user_unmounted = s_sd_user_unmounted;
    }
    if (sd.present && !sd.mounted && !user_unmounted && !sd.error) {
        web_plat_sd_mount();
        web_plat_sd_info(&sd, false);
    } else if (!sd.present && sd.mounted) {
        web_plat_sd_unmount();
        web_plat_sd_info(&sd, false);
    }

    web_theme_t theme = { ui_active_accent, ui_active_accent_dim, ui_active_card_bg, ui_active_track };
    bool theme_changed;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        theme_changed = memcmp(&theme, &s_theme, sizeof(theme)) != 0;
        s_theme = theme;
    }
    if (theme_changed && s_have_snapshot) web_events_publish("theme", theme_json(theme).c_str());

    bool battery_changed, sd_changed, net_changed, first;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        first = !s_have_snapshot;
        battery_changed = first || d.percent != s_dev.percent || d.charging != s_dev.charging;
        sd_changed = first || sd.present != s_sd.present || sd.mounted != s_sd.mounted ||
                     (sd.error != s_sd.error && (!sd.error || !s_sd.error || strcmp(sd.error, s_sd.error)));
        net_changed = first || strcmp(d.ip, s_dev.ip) || strcmp(d.ssid, s_dev.ssid) || strcmp(d.mode, s_dev.mode);
        s_dev = d;
        if (!sd_changed) {
            sd.total = s_sd.total;           /* space is read only on demand or on a change */
            sd.free = s_sd.free;
        }
        s_sd = sd;
        s_have_snapshot = true;
    }
    if (sd_changed) read_sd_snapshot();
    /* A card was mounted (or the server just started with one): the watch makes sure its desktop is there. */
    if (s_running && sd.mounted && !s_card_seen) web_desktop_card_mounted();
    s_card_seen = sd.mounted;
    if (first) return;
    char b[160];
    if (battery_changed) {
        snprintf(b, sizeof(b), "{\"percent\":%d,\"charging\":%s,\"mv\":%d}", d.percent, d.charging ? "true" : "false", d.mv);
        web_events_publish("battery", b);
    }
    if (sd_changed) publish_sd();
    if (net_changed) {
        std::string j = "{\"mode\":";
        web_json_str(j, d.mode);
        j += ",\"ssid\":";
        web_json_str(j, d.ssid);
        j += ",\"ip\":";
        web_json_str(j, d.ip);
        snprintf(b, sizeof(b), ",\"rssi\":%d}", d.rssi);
        j += b;
        web_events_publish("net", j.c_str());
    }
}

/* ---------------------------------------------------------------- static files */

const char *web_mime_type(const char *path)
{
    static const struct { const char *ext, *type; } T[] = {
        { ".html", "text/html; charset=utf-8" }, { ".css", "text/css" }, { ".js", "text/javascript" },
        { ".mjs", "text/javascript" }, { ".json", "application/json" }, { ".svg", "image/svg+xml" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".webp", "image/webp" }, { ".ico", "image/x-icon" }, { ".wav", "audio/wav" }, { ".mp3", "audio/mpeg" },
        { ".txt", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" }, { ".csv", "text/csv" },
        { ".gpx", "application/gpx+xml" }, { ".woff2", "font/woff2" }, { ".webmanifest", "application/manifest+json" },
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/')) {
        for (auto &x : T) {
            if (!strcasecmp(dot, x.ext)) return x.type;
        }
    }
    return "application/octet-stream";
}

/* The rescue page (web-rescue/index.html, gzipped by tools/web_build.py --rescue). Served at /rescue,
 * and at / when the SD card holds no desktop for API v1. */
#include "web_rescue.h"

static void send_rescue(web_req_t *r)
{
    if (r->begin(r, 200, "text/html; charset=utf-8",
                 "Content-Encoding: gzip\r\nCache-Control: no-cache\r\nX-Frame-Options: SAMEORIGIN\r\n",
                 (long long)WEB_RESCUE_HTML_GZ_LEN)) {
        r->write(r, (const char *)WEB_RESCUE_HTML_GZ, WEB_RESCUE_HTML_GZ_LEN);
    }
    r->end(r);
}

static bool desktop_dir(std::string &out)
{
    if (!web_fs_sd_mounted()) return false;
    const char *sd = web_plat_root_dir("sd");
    if (!sd) return false;
    std::string m = std::string(sd) + "/web/manifest.json";
    web_plat_io_lock();
    FILE *f = fopen(m.c_str(), "rb");
    std::string data;
    if (f) {
        char b[1024];
        size_t n = fread(b, 1, sizeof(b), f);
        data.assign(b, n);
        fclose(f);
    }
    web_plat_io_unlock();
    double api = 0;
    if (!web_json_get_num(data, "api", &api) || api != 1) return false;
    out = std::string(sd) + "/web";
    return true;
}

static void handle_static(web_req_t *r)
{
    if (strstr(r->path, "..")) {
        web_resp_error(r, 400, "bad_request");
        return;
    }
    if (!strcmp(r->path, "/rescue") || !strcmp(r->path, "/rescue/")) {
        send_rescue(r);
        return;
    }
    std::string dir;
    if (!desktop_dir(dir)) {
        if (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html")) send_rescue(r);
        else web_resp_error(r, 404, "not_found");
        return;
    }
    std::string rel = r->path;
    if (rel == "/") rel = "/index.html";
    if (rel.back() == '/') rel += "index.html";
    std::string abs = dir + rel;
    bool gz = strstr(r->accept_encoding, "gzip") != NULL;
    struct stat st;
    std::string file = abs;
    web_plat_io_lock();
    if (gz && stat((abs + ".gz").c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        file = abs + ".gz";
    } else {
        gz = false;
        if (stat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            web_plat_io_unlock();
            web_resp_error(r, 404, "not_found");
            return;
        }
    }
    web_plat_io_unlock();
    char etag[48];
    snprintf(etag, sizeof(etag), "\"%c%lx-%lx\"", gz ? 'g' : 'r', (unsigned long)st.st_size, (unsigned long)st.st_mtime);
    std::string h = "ETag: ";
    h += etag;
    h += "\r\nCache-Control: no-cache\r\n";
    if (!strcmp(r->if_none_match, etag)) {
        r->begin(r, 304, NULL, h.c_str(), 0);
        r->end(r);
        return;
    }
    if (gz) h += "Content-Encoding: gzip\r\n";
    const char *type = web_mime_type(abs.c_str());
    if (!strncmp(type, "text/html", 9)) h += "X-Frame-Options: SAMEORIGIN\r\n";
    web_plat_io_lock();
    FILE *f = fopen(file.c_str(), "rb");
    web_plat_io_unlock();
    if (!f) {
        web_resp_error(r, 404, "not_found");
        return;
    }
    if (r->begin(r, 200, type, h.c_str(), (long long)st.st_size)) {
        std::unique_ptr<char[]> buf(new char[WEB_STATIC_CHUNK]);
        for (;;) {
            web_plat_io_lock();
            size_t n = fread(buf.get(), 1, WEB_STATIC_CHUNK, f);
            web_plat_io_unlock();
            if (n == 0 || !r->write(r, buf.get(), n)) break;
        }
    }
    fclose(f);
    r->end(r);
}

/* ---------------------------------------------------------------- router */

#ifndef ARDUINO
extern void web_sim_set_sd_present(bool present);
#endif

static bool is(const web_req_t *r, const char *method, const char *path)
{
    return !strcmp(r->method, method) && !strcmp(r->path, path);
}

void web_api_handle(web_req_t *r)
{
    s_last_request_ms = now_ms();
    if (strncmp(r->path, "/api/", 5) != 0) {
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD") && strcmp(r->method, "POST")) {
            web_resp_error(r, 405, "bad_request", "GET or POST");
            return;
        }
        if (web_evilportal_captive(r)) return;
        if (strcmp(r->method, "GET") && strcmp(r->method, "HEAD")) {
            web_resp_error(r, 405, "bad_request", "GET");
            return;
        }
        handle_static(r);
        return;
    }

    /* public */
    if (is(r, "GET", "/api/auth")) {
        web_resp_json(r, 200, is_paired(r) ? "{\"paired\":true,\"method\":\"pin\"}" : "{\"paired\":false,\"method\":\"pin\"}");
        return;
    }
    if (is(r, "POST", "/api/auth/pair")) return handle_pair(r);
    if (is(r, "GET", "/api/caps")) return handle_caps(r);
    if (is(r, "GET", "/api/device")) return handle_device(r);
#ifndef ARDUINO
    /* Simulator-only test hooks (docs/web-roadmap.md, Testing). Never built for the watch. */
    if (is(r, "GET", "/api/_dev/pin")) {
        /* Stands in for looking at the watch: opens the pairing window like the Web page does. */
        web_ui_run([] { if (!s_pairing_open) web_server_set_pairing(true); });
        std::string j = "{\"pin\":";
        web_json_str(j, s_pin);
        web_resp_json(r, 200, j + "}");
        return;
    }
    if (is(r, "POST", "/api/_dev/pairing")) {
        std::string body;
        bool open = true;
        web_read_body(r, body, 256);
        web_json_get_bool(body, "open", &open);
        web_ui_run([&] { web_server_set_pairing(open); });
        web_resp_ok(r);
        return;
    }
    if (is(r, "POST", "/api/_dev/theme")) {
        std::string body;
        double preset = 0;
        web_read_body(r, body, 256);
        if (!web_json_get_num(body, "preset", &preset) || preset < 0 || preset >= UI_THEME_PRESET_COUNT) {
            web_resp_error(r, 400, "bad_request", "preset");
            return;
        }
        web_ui_run([&] { ui_apply_theme_preset((uint8_t)preset); });
        web_resp_ok(r);
        return;
    }
    if (is(r, "GET", "/api/_dev/input")) {
        web_resp_json(r, 200, web_screen_last_input());
        return;
    }
    if (is(r, "POST", "/api/_dev/emit")) {
        /* Publish an event server-side (no WebSocket needed) and report the in-process tap's state,
         * so a test can prove a seatless event still reaches a sink (automation B step 1). */
        std::string body, topic;
        web_read_body(r, body, 512);
        if (!web_json_get_str(body, "topic", topic) || topic.empty()) {
            web_resp_error(r, 400, "bad_request", "topic");
            return;
        }
        int seat;
        { std::lock_guard<std::recursive_mutex> lk(s_lock); seat = s_seat; }
        web_events_publish(topic.c_str(), "{}");
        std::string j = "{\"events_seen\":";
        j += std::to_string((unsigned)s_tap_count.load(std::memory_order_relaxed));
        j += ",\"last_topic\":";
        { std::lock_guard<std::recursive_mutex> lk(s_lock); web_json_str(j, s_tap_last_topic); }
        j += ",\"seat\":";
        j += std::to_string(seat);
        j += "}";
        web_resp_json(r, 200, j);
        return;
    }
    if (is(r, "POST", "/api/_dev/sd")) {
        std::string body;
        bool present = true;
        web_read_body(r, body, 256);
        web_json_get_bool(body, "present", &present);
        web_ui_run([&] {
            web_sim_set_sd_present(present);
            if (present) web_plat_sd_mount(); else web_plat_sd_unmount();
            read_sd_snapshot();
            s_card_seen = false;        /* the monitor checks the desktop on this card */
        });
        publish_sd();
        web_sd_info_t sd;
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            sd = s_sd;
        }
        web_resp_json(r, 200, sd_json(sd));
        return;
    }
#endif

    /* paired */
    if (!is_paired(r)) {
        web_resp_error(r, 401, "unpaired", "pair with the PIN on the watch");
        return;
    }
    if (is(r, "POST", "/api/auth/logout")) return handle_logout(r);
    if (is(r, "POST", "/api/time")) return handle_time(r);
    if (is(r, "GET", "/api/settings")) {
        web_resp_json(r, 200, settings_json());
        return;
    }
    if (is(r, "PATCH", "/api/settings")) return handle_settings_patch(r);
    if (is(r, "POST", "/api/system/reboot")) {
        web_resp_ok(r);
        web_ui_run([] { web_plat_reboot(); }, 500);
        return;
    }
    if (is(r, "GET", "/api/sd")) {
        web_ui_run([] { read_sd_snapshot(); });
        web_sd_info_t sd;
        {
            std::lock_guard<std::recursive_mutex> lk(s_lock);
            sd = s_sd;
        }
        web_resp_json(r, 200, sd_json(sd));
        return;
    }
    if (is(r, "POST", "/api/sd/mount")) return handle_sd_mount(r, true);
    if (is(r, "POST", "/api/sd/unmount")) return handle_sd_mount(r, false);
    if (!strcmp(r->path, "/api/files") || !strncmp(r->path, "/api/files/", 11)) return web_fs_handle(r);
    if (web_screen_handle(r)) return;
    if (web_desktop_handle(r)) return;
    if (web_tools_handle(r)) return;
    web_resp_error(r, 404, "not_found");
}

/* ---------------------------------------------------------------- start / stop */

bool web_server_start(void)
{
    if (s_running) return true;
    new_pin();
    load_state();
    if (!s_ui_timer) s_ui_timer = lv_timer_create(ui_timer_cb, 10, NULL);
    monitor_timer_cb(NULL);              /* first snapshot, on the UI thread */
    if (!s_monitor_timer) s_monitor_timer = lv_timer_create(monitor_timer_cb, 1000, NULL);
#ifdef ARDUINO
    uint16_t port = WEB_PORT_DEVICE;
#else
    uint16_t port = WEB_PORT_SIMULATOR;
    const char *port_env = getenv("TWATCH_SIM_WEB_PORT");     /* run a second simulator next to the first */
    if (port_env && atoi(port_env) > 0) port = (uint16_t)atoi(port_env);
#endif
    if (!web_transport_start(port)) {
        web_plat_log("[web] server did not start on port %u", (unsigned)port);
        return false;
    }
    s_running = true;
#ifndef ARDUINO
    static bool s_tap_subscribed = false;
    if (!s_tap_subscribed) { web_events_subscribe(dev_tap_sink, NULL); s_tap_subscribed = true; }
#endif
    web_tools_start();
    s_last_request_ms = now_ms();
    s_card_seen = false;                /* the monitor checks the desktop on the card */
    web_plat_log("[web] server on port %u", (unsigned)port);
    return true;
}

void web_server_stop(void)
{
    if (!s_running) return;
    web_tools_stop();
    web_transport_stop();
    web_plat_net_stop();
    s_pairing_open = false;
    if (s_monitor_timer) {
        lv_timer_delete(s_monitor_timer);
        s_monitor_timer = NULL;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_seat = -1;
        s_have_snapshot = false;
    }
    s_running = false;
}

bool web_server_running(void)
{
    return s_running;
}

const char *web_server_pin(void)
{
    return s_pin;
}

/* ---------------------------------------------------------------- on/off, pairing window, status */

bool web_server_enabled(void)
{
    FILE *f = fopen(state_path("enabled").c_str(), "rb");
    if (!f) {
#ifdef ARDUINO
        return false;               /* Off until the user turns it on: Wi-Fi costs battery */
#else
        return true;                /* The simulator serves from the start, for the tests */
#endif
    }
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

void web_server_set_enabled(bool on)
{
    save_file_atomic(state_path("enabled"), on ? "1" : "0");
    if (on) web_server_start();
    else web_server_stop();
}

void web_server_set_pairing(bool open)
{
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    if (open && !s_pairing_open) {
        new_pin();
        s_misses = 0;
        s_locked_until = 0;
    }
    if (open && !s_pairing_open) web_plat_log("[web] pairing open, PIN %s", s_pin);
    s_pairing_open = open;
    if (open) s_last_request_ms = now_ms();     /* the page counts as activity */
}

bool web_server_keep_awake(void)
{
    if (!s_running) return false;
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    return s_seat >= 0 || lv_tick_diff(now_ms(), s_last_request_ms.load()) < 60000;
}

void web_server_status(web_status_t *out)
{
    memset(out, 0, sizeof(*out));
    std::lock_guard<std::recursive_mutex> lk(s_lock);
    out->running = s_running;
    out->desktop_connected = s_seat >= 0;
    out->pairing_open = s_pairing_open;
    snprintf(out->pin, sizeof(out->pin), "%s", s_pin);
    snprintf(out->ip, sizeof(out->ip), "%s", s_dev.ip);
    snprintf(out->host, sizeof(out->host), "watch.local");
    out->ap_active = web_plat_ap_info(out->ap_ssid, sizeof(out->ap_ssid), out->ap_pass, sizeof(out->ap_pass));
    out->sessions = (unsigned)s_sessions.size();
    web_desktop_status_line(out->desktop, sizeof(out->desktop));
}

void web_server_forget_browsers(void)
{
    int seat;
    {
        std::lock_guard<std::recursive_mutex> lk(s_lock);
        s_sessions.clear();
        seat = s_seat;
    }
    save_state();
    if (seat >= 0) web_transport_ws_close(seat);
}

#endif /* FACTORY_WEB */
