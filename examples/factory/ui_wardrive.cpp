/**
 * @file      ui_wardrive.cpp
 * @brief     On-device Wardrive app. Starts/stops a wardriving session, scans for Wi-Fi access
 *            points (each tagged with the current GPS fix), shows the running tally and exports the
 *            WiGLE CSV to /sd/wardrive — the same session and catalogue the web Wardrive app drives
 *            (web_tool_wardrive.cpp). You read the tally and trigger a scan/export here; the browser
 *            is the big-screen view of the same list. Passive: it only logs what it already sees.
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* Session + catalogue + WiGLE export (web_tool_wardrive.cpp). */
extern void wardrive_counts(bool *running, bool *scanning, int *count, int *with_fix, long long *last_ts);
extern void wardrive_start(void);
extern void wardrive_stop(void);
extern bool wardrive_scan_async(void);
extern int  wardrive_export(std::string &path_out, int *skipped);

static lv_obj_t *page_container = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *msg_label = NULL;
static lv_obj_t *start_btn_lbl = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *export_btn = NULL;
static lv_timer_t *refresh_timer = NULL;

static void refresh_status(void)
{
    if (!status_label) return;
    bool running = false, scanning = false;
    int count = 0, with_fix = 0;
    long long ts = 0;
    wardrive_counts(&running, &scanning, &count, &with_fix, &ts);

    std::string s = running ? "Recording" : "Stopped";
    if (scanning) s += " · scanning…";
    s += "\n" + std::to_string(count) + (count == 1 ? " network" : " networks");
    s += "\n" + std::to_string(with_fix) + " with a GPS fix";
    lv_label_set_text(status_label, s.c_str());

    if (start_btn_lbl) lv_label_set_text(start_btn_lbl, running ? "Stop" : "Start");
    if (scan_btn) {
        if (running && !scanning) lv_obj_clear_state(scan_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(scan_btn, LV_STATE_DISABLED);
    }
    if (export_btn) {
        if (count > 0) lv_obj_clear_state(export_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(export_btn, LV_STATE_DISABLED);
    }
}

static void start_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    bool running = false;
    wardrive_counts(&running, NULL, NULL, NULL, NULL);
    if (running) wardrive_stop();
    else { wardrive_start(); if (msg_label) lv_label_set_text(msg_label, ""); }
    refresh_status();
}

static void scan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (wardrive_scan_async() && msg_label) lv_label_set_text(msg_label, "Scanning…");
    refresh_status();
}

static void export_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    std::string path;
    int skipped = 0;
    int rows = wardrive_export(path, &skipped);
    if (!msg_label) return;
    if (rows < 0) {
        lv_label_set_text(msg_label, "Export failed — is the SD card present?");
    } else {
        std::string m = "Saved " + std::to_string(rows) + (rows == 1 ? " network\n" : " networks\n") + path;
        if (skipped) m += "\n(" + std::to_string(skipped) + " without a fix skipped)";
        lv_label_set_text(msg_label, m.c_str());
    }
}

static void refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    refresh_status();
}

static void cleanup(void)
{
    if (refresh_timer) { lv_timer_delete(refresh_timer); refresh_timer = NULL; }
    page_container = status_label = msg_label = start_btn_lbl = scan_btn = export_btn = NULL;
}

static void back_event_handler(lv_event_t *e)
{
    LV_UNUSED(e);
    if (page_container) ui_destroy_app_page(page_container);
    cleanup();
    menu_show();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    if (lbl_out) *lbl_out = lbl;
    return btn;
}

static void ui_wardrive_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "Wardrive", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page_container, 10, 0);

    status_label = lv_label_create(page_container);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, lv_pct(100));

    lv_obj_t *btn_row = lv_obj_create(page_container);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 8, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);

    make_button(btn_row, "Start", start_cb, &start_btn_lbl);
    scan_btn = make_button(btn_row, "Scan", scan_cb, NULL);
    export_btn = make_button(btn_row, "Export", export_cb, NULL);

    msg_label = lv_label_create(page_container);
    lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_label, lv_pct(100));
    lv_label_set_text(msg_label, "Start a session, then Scan as you move.\nExport writes a WiGLE CSV to /sd/wardrive.");

    refresh_status();
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
}

static void ui_wardrive_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    cleanup();
}

app_t ui_wardrive_main = {
    .setup_func_cb = ui_wardrive_enter,
    .exit_func_cb = ui_wardrive_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
