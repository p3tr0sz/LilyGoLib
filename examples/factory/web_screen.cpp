/**
 * @file      web_screen.cpp
 * @brief     Web API v1 screen, input and display routes (docs/web-api.md, P3).
 *
 *            GET  /api/screen   the watch screen, from an LVGL snapshot (screen + top layer),
 *                               as run-length RGB565 (default) or raw RGB565. ETag = frame hash.
 *            POST /api/input    tap / long / swipe through a second LVGL pointer device, so the
 *                               watch UI cannot tell it from the touchscreen; or a button.
 *            POST /api/display  screen on/off and live brightness.
 */
#include "web_internal.h"

#if FACTORY_WEB

#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <deque>
#include <mutex>
#include <vector>

#if LV_USE_SNAPSHOT == 0
#error "The web screen view needs LV_USE_SNAPSHOT=1 (src/lv_conf.h, and -D LV_USE_SNAPSHOT=1 for the simulator)"
#endif

extern "C" void lilygo_power_menu_request(void);
#if FACTORY_WATCH_UI
void watch_shell_wake(void);     /* ui_watch_shell.h */
#endif

/* ---------------------------------------------------------------- capture (UI thread) */

/* Blend an ARGB8888 pixel over an RGB565 one. */
static inline uint16_t blend565(uint16_t under, uint32_t argb)
{
    uint32_t a = argb >> 24;
    if (a == 0) return under;
    uint32_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    if (a < 255) {
        uint32_t ur = ((under >> 11) & 0x1F) << 3, ug = ((under >> 5) & 0x3F) << 2, ub = (under & 0x1F) << 3;
        r = (r * a + ur * (255 - a)) / 255;
        g = (g * a + ug * (255 - a)) / 255;
        b = (b * a + ub * (255 - a)) / 255;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Snapshot buffers owned here, not taken from LVGL's heap: a full frame is ~400 KB (RGB565)
 * plus ~800 KB for the top layer (ARGB8888), more than the simulator's fixed LVGL heap holds
 * once apps have loaded images. Kept between frames (PSRAM on the watch). */
static std::vector<uint32_t> s_base_mem, s_top_mem;

static bool snap(lv_obj_t *obj, lv_color_format_t cf, std::vector<uint32_t> &mem, lv_draw_buf_t *buf,
                 int32_t w, int32_t h)
{
    uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
    size_t size = (size_t)stride * h;
    mem.resize(size / 4 + 32);              /* room to align the start */
    void *data = lv_draw_buf_align(mem.data(), cf);
    size_t avail = mem.size() * 4 - ((uint8_t *)data - (uint8_t *)mem.data());
    if (lv_draw_buf_init(buf, w, h, cf, stride, data, (uint32_t)avail) != LV_RESULT_OK) return false;
    lv_draw_buf_clear(buf, NULL);           /* parts nothing draws stay transparent / black */
    return lv_snapshot_take_to_draw_buf(obj, cf, buf) == LV_RESULT_OK;
}

/* The visible screen as RGB565, row by row. UI thread only. */
static bool capture(std::vector<uint16_t> &px, int &w, int &h)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return false;
    lv_display_t *disp = lv_obj_get_display(scr);
    w = lv_display_get_horizontal_resolution(disp);
    h = lv_display_get_vertical_resolution(disp);
    lv_draw_buf_t base;
    if (!snap(scr, LV_COLOR_FORMAT_RGB565, s_base_mem, &base, w, h)) return false;
    px.resize((size_t)w * h);
    int bw = (int)base.header.w < w ? (int)base.header.w : w;
    int bh = (int)base.header.h < h ? (int)base.header.h : h;
    for (int y = 0; y < bh; y++) {
        memcpy(&px[(size_t)y * w], base.data + (size_t)y * base.header.stride, (size_t)bw * 2);
    }

    /* Toasts, dialogs and the dark overlay live on the top layer. */
    lv_obj_t *top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        lv_draw_buf_t over;
        if (snap(top, LV_COLOR_FORMAT_ARGB8888, s_top_mem, &over, w, h)) {
            int ow = (int)over.header.w < w ? (int)over.header.w : w;
            int oh = (int)over.header.h < h ? (int)over.header.h : h;
            for (int y = 0; y < oh; y++) {
                const uint32_t *row = (const uint32_t *)(over.data + (size_t)y * over.header.stride);
                uint16_t *dst = &px[(size_t)y * w];
                for (int x = 0; x < ow; x++) dst[x] = blend565(dst[x], row[x]);
            }
        }
    }
    return true;
}

static uint32_t fnv1a(const uint16_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n * 2; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static void handle_screen(web_req_t *r)
{
    char fmt[16] = "rle565";
    web_query_get(r, "format", fmt, sizeof(fmt));
    if (strcmp(fmt, "rle565") && strcmp(fmt, "rgb565")) {
        web_resp_error(r, 400, "bad_request", "format: rle565 or rgb565");
        return;
    }
    std::vector<uint16_t> px;
    int w = 0, h = 0;
    bool ok = false;
    if (!web_ui_run([&] { ok = capture(px, w, h); }, 5000) || !ok) {
        web_resp_error(r, 503, "busy", "could not capture the screen");
        return;
    }
    char etag[32];
    snprintf(etag, sizeof(etag), "\"%s-%08x\"", fmt, (unsigned)fnv1a(px.data(), px.size()));
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "ETag: %s\r\nCache-Control: no-cache\r\nX-Width: %d\r\nX-Height: %d\r\n"
             "Access-Control-Expose-Headers: X-Width, X-Height\r\n", etag, w, h);
    if (!strcmp(r->if_none_match, etag)) {
        r->begin(r, 304, NULL, hdr, 0);
        r->end(r);
        return;
    }
    if (!strcmp(fmt, "rgb565")) {
        /* Little-endian RGB565, row by row. */
        if (r->begin(r, 200, "application/octet-stream", hdr, (long long)px.size() * 2)) {
            r->write(r, (const char *)px.data(), px.size() * 2);
        }
        r->end(r);
        return;
    }
    /* rle565: runs of [count u16 LE][colour u16 LE], count 1..65535, over the rows. */
    std::vector<uint16_t> out;
    out.reserve(4096);
    size_t i = 0, n = px.size();
    while (i < n) {
        uint16_t c = px[i];
        size_t run = 1;
        while (i + run < n && px[i + run] == c && run < 65535) run++;
        out.push_back((uint16_t)run);
        out.push_back(c);
        i += run;
    }
    if (r->begin(r, 200, "application/octet-stream", hdr, (long long)out.size() * 2)) {
        r->write(r, (const char *)out.data(), out.size() * 2);
    }
    r->end(r);
}

/* ---------------------------------------------------------------- input (a second pointer device) */

struct step_t {
    int16_t x, y;
    bool pressed;
    uint16_t hold_ms;       /* how long this state is reported */
};

static std::mutex s_in_lock;
static std::deque<step_t> s_steps;
static step_t s_cur = { 0, 0, false, 0 };
static uint32_t s_cur_since = 0;
static lv_indev_t *s_indev = NULL;
static std::string s_last_input = "{}";

static void web_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    std::lock_guard<std::mutex> lk(s_in_lock);
    uint32_t now = lv_tick_get();
    if (lv_tick_diff(now, s_cur_since) >= s_cur.hold_ms && !s_steps.empty()) {
        s_cur = s_steps.front();
        s_steps.pop_front();
        s_cur_since = now;
    }
    data->point.x = s_cur.x;
    data->point.y = s_cur.y;
    data->state = s_cur.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

/* UI thread. */
static void ensure_indev(void)
{
    if (s_indev) return;
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, web_indev_read);
    lv_indev_set_display(s_indev, lv_display_get_default());
}

static void queue(std::initializer_list<step_t> steps)
{
    std::lock_guard<std::mutex> lk(s_in_lock);
    for (const step_t &s : steps) s_steps.push_back(s);
}

static void handle_input(web_req_t *r)
{
    std::string body, type, name;
    if (!web_read_body(r, body, 1024) || !web_json_get_str(body, "type", type)) {
        web_resp_error(r, 400, "bad_request", "type");
        return;
    }
    int32_t w = 0, h = 0;
    web_ui_run([&] {
        lv_display_t *d = lv_display_get_default();
        w = lv_display_get_horizontal_resolution(d);
        h = lv_display_get_vertical_resolution(d);
        ensure_indev();
    });
    auto coord = [&](const char *kx, const char *ky, int16_t *x, int16_t *y) {
        double vx, vy;
        if (!web_json_get_num(body, kx, &vx) || !web_json_get_num(body, ky, &vy)) return false;
        if (vx < 0 || vy < 0 || vx >= w || vy >= h) return false;
        *x = (int16_t)vx;
        *y = (int16_t)vy;
        return true;
    };
    int16_t x = 0, y = 0, x2 = 0, y2 = 0;
    if (type == "tap" || type == "long") {
        if (!coord("x", "y", &x, &y)) {
            char m[64];
            snprintf(m, sizeof(m), "x 0-%d, y 0-%d", (int)w - 1, (int)h - 1);
            web_resp_error(r, 400, "bad_request", m);
            return;
        }
        uint16_t hold = type == "tap" ? 60 : 900;
        queue({ { x, y, true, hold }, { x, y, false, 60 } });
    } else if (type == "swipe") {
        if (!coord("x", "y", &x, &y) || !coord("x2", "y2", &x2, &y2)) {
            web_resp_error(r, 400, "bad_request", "x, y, x2, y2 inside the screen");
            return;
        }
        /* Press, move in 10 steps over about 200 ms, release. */
        std::lock_guard<std::mutex> lk(s_in_lock);
        s_steps.push_back({ x, y, true, 30 });
        for (int i = 1; i <= 10; i++) {
            s_steps.push_back({ (int16_t)(x + (x2 - x) * i / 10), (int16_t)(y + (y2 - y) * i / 10), true, 20 });
        }
        s_steps.push_back({ x2, y2, false, 60 });
    } else if (type == "button") {
        if (!web_json_get_str(body, "name", name) || (name != "power_menu" && name != "wake")) {
            web_resp_error(r, 400, "bad_request", "name: power_menu or wake");
            return;
        }
        web_ui_run([&] {
            if (name == "power_menu") lilygo_power_menu_request();
#if FACTORY_WATCH_UI
            else watch_shell_wake();
#endif
            lv_display_trigger_activity(NULL);
        });
    } else {
        web_resp_error(r, 400, "bad_request", "type: tap, long, swipe or button");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s_in_lock);
        s_last_input = body;
    }
    web_resp_ok(r);
}

void web_server_init(void)
{
    ensure_indev();
}

std::string web_screen_last_input(void)
{
    std::lock_guard<std::mutex> lk(s_in_lock);
    return s_last_input;
}

/* ---------------------------------------------------------------- display */

static void handle_display(web_req_t *r)
{
    std::string body;
    if (!web_read_body(r, body, 512) || !web_json_is_object(body)) {
        web_resp_error(r, 400, "bad_request", "bad JSON");
        return;
    }
    bool on = true, has_on = web_json_get_bool(body, "on", &on);
    double b = -1;
    bool has_b = web_json_has(body, "brightness");
    if (has_b && (!web_json_get_num(body, "brightness", &b) || b < 0 || b > 100)) {
        web_resp_error(r, 400, "bad_request", "brightness 0-100");
        return;
    }
    web_ui_run([&] {
        if (has_on && !on) {
            hw_set_disp_backlight(0);
        } else if (has_on && on) {
            user_setting_params_t s;
            hw_get_user_setting(s);
#if FACTORY_WATCH_UI
            watch_shell_wake();
#endif
            hw_set_disp_backlight(s.brightness_level);
            lv_display_trigger_activity(NULL);
        }
        if (has_b) {
            int lo = hw_get_disp_min_brightness(), hi = hw_get_disp_max_brightness();
            hw_set_disp_backlight((uint8_t)(lo + (hi - lo) * (int)b / 100));
        }
    });
    web_resp_ok(r);
}

/* ---------------------------------------------------------------- router entry */

bool web_screen_handle(web_req_t *r)
{
    if (!strcmp(r->path, "/api/screen") && !strcmp(r->method, "GET")) { handle_screen(r); return true; }
    if (!strcmp(r->path, "/api/input") && !strcmp(r->method, "POST")) { handle_input(r); return true; }
    if (!strcmp(r->path, "/api/display") && !strcmp(r->method, "POST")) { handle_display(r); return true; }
    return false;
}

#endif /* FACTORY_WEB */
