/**
 * @file      ui_web.cpp
 * @brief     Settings > Web desktop on the watch: turn the web server on or off, and see how
 *            to reach it (QR code, address, PIN). While this page is open the PIN works;
 *            leaving it closes pairing. docs/web-roadmap.md, P3.
 */
#include "ui_define.h"

#if FACTORY_WATCH_UI && FACTORY_WEB

#include "web_server.h"
#include <stdio.h>
#include <string.h>

static struct {
    lv_obj_t *running;          /* everything shown only while the server runs */
    lv_obj_t *note;
    lv_obj_t *qr;
    lv_obj_t *qr_caption;
    lv_obj_t *pin;
    lv_obj_t *address;
    lv_obj_t *ip_row, *ip;
    lv_obj_t *ap_ssid_row, *ap_ssid;
    lv_obj_t *ap_pass_row, *ap_pass;
    lv_obj_t *browser;
    lv_obj_t *paired;
    lv_timer_t *timer;
    char qr_text[128];
} w;

static lv_obj_t *value_label(lv_obj_t *row)
{
    return lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
}

static void set_text_if(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static void refresh(void)
{
    web_status_t st;
    web_server_status(&st);
    lv_obj_set_flag(w.running, LV_OBJ_FLAG_HIDDEN, !st.running);
    set_text_if(w.note, st.running
                ? (st.ap_active ? "No known Wi-Fi nearby, so the watch made its own network. Join it, then open the address."
                   : "Open the address in a browser on the same Wi-Fi and enter the PIN.")
                : "Turn on to use the watch from a browser: files, settings and the screen.");
    if (!st.running) return;

    char url[64], buf[96];
    snprintf(url, sizeof(url), "http://%s/", st.ip[0] ? st.ip : st.host);
    /* QR: join the watch's own network, or open the desktop. */
    if (st.ap_active) {
        snprintf(buf, sizeof(buf), "WIFI:T:WPA;S:%s;P:%s;;", st.ap_ssid, st.ap_pass);
    } else {
        snprintf(buf, sizeof(buf), "%s", url);
    }
    if (strcmp(buf, w.qr_text)) {
        snprintf(w.qr_text, sizeof(w.qr_text), "%s", buf);
        lv_qrcode_update(w.qr, w.qr_text, strlen(w.qr_text));
    }
    set_text_if(w.qr_caption, st.ap_active ? "Scan to join the watch's Wi-Fi" : "Scan to open the desktop");
    set_text_if(w.pin, st.pairing_open ? st.pin : "------");
    snprintf(buf, sizeof(buf), "%s", st.ap_active ? url : st.host);
    set_text_if(w.address, buf);
    set_text_if(w.ip, st.ip[0] ? st.ip : "--");
    ui_card_row_set_hidden(w.ip_row, st.ap_active);
    ui_card_row_set_hidden(w.ap_ssid_row, !st.ap_active);
    ui_card_row_set_hidden(w.ap_pass_row, !st.ap_active);
    set_text_if(w.ap_ssid, st.ap_ssid);
    set_text_if(w.ap_pass, st.ap_pass);
    set_text_if(w.browser, st.desktop_connected ? "Connected" : "None");
    snprintf(buf, sizeof(buf), "%u", st.sessions);
    set_text_if(w.paired, buf);
}

static void timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    refresh();
}

static void switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    web_server_set_enabled(on);
    if (on) web_server_set_pairing(true);
    if (on && !web_server_running()) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
        ui_toast("The web server could not start");
    }
    refresh();
}

static void forget_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    web_server_forget_browsers();
    hw_feedback();
    refresh();
}

static void page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (w.timer) lv_timer_delete(w.timer);
    web_server_set_pairing(false);
    memset(&w, 0, sizeof(w));
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(l, UI_COLOR_TEXT_SECONDARY, 0);
    return l;
}

void ui_web_settings_build(lv_obj_t *content)
{
    memset(&w, 0, sizeof(w));
    lv_obj_t *card = ui_create_card(content, NULL);
    ui_create_card_switch(card, LV_SYMBOL_WIFI, "Web desktop", web_server_running(), switch_cb);
    w.note = caption(content, "");

    w.running = lv_obj_create(content);
    lv_obj_set_size(w.running, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(w.running, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w.running, 0, 0);
    lv_obj_set_style_pad_all(w.running, 0, 0);
    lv_obj_set_style_pad_row(w.running, 12, 0);
    lv_obj_set_flex_flow(w.running, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w.running, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(w.running, LV_OBJ_FLAG_SCROLLABLE);

    /* PIN first: it is what the user types. */
    lv_obj_t *pin_title = caption(w.running, "PIN");
    lv_obj_set_style_text_align(pin_title, LV_TEXT_ALIGN_CENTER, 0);
    w.pin = lv_label_create(w.running);
    lv_obj_set_style_text_font(w.pin, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(w.pin, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_letter_space(w.pin, 6, 0);
    lv_label_set_text(w.pin, "------");

    /* QR on white, with a quiet zone, so phone cameras read it off the AMOLED. */
    lv_obj_t *qr_box = lv_obj_create(w.running);
    lv_obj_set_size(qr_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(qr_box, 16, 0);
    lv_obj_set_style_border_width(qr_box, 0, 0);
    lv_obj_set_style_pad_all(qr_box, 14, 0);
    lv_obj_remove_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);
    w.qr = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(w.qr, 200);
    lv_qrcode_set_dark_color(w.qr, lv_color_black());
    lv_qrcode_set_light_color(w.qr, lv_color_white());
    w.qr_caption = caption(w.running, "");
    lv_obj_set_style_text_align(w.qr_caption, LV_TEXT_ALIGN_CENTER, 0);

    card = ui_create_card(w.running, NULL);
    w.address = value_label(ui_create_card_info(card, LV_SYMBOL_HOME, "Address", "watch.local"));
    w.ip_row = ui_create_card_info(card, LV_SYMBOL_WIFI, "IP", "--");
    w.ip = value_label(w.ip_row);
    w.ap_ssid_row = ui_create_card_info(card, LV_SYMBOL_WIFI, "Wi-Fi", "--");
    w.ap_ssid = value_label(w.ap_ssid_row);
    w.ap_pass_row = ui_create_card_info(card, LV_SYMBOL_EDIT, "Password", "--");
    w.ap_pass = value_label(w.ap_pass_row);

    card = ui_create_card(w.running, NULL);
    w.browser = value_label(ui_create_card_info(card, LV_SYMBOL_EYE_OPEN, "Browser", "None"));
    w.paired = value_label(ui_create_card_info(card, LV_SYMBOL_OK, "Paired", "0"));
    lv_obj_t *forget = ui_create_card_action(card, LV_SYMBOL_TRASH, "Forget browsers", forget_cb);
    ui_require_hold_hint(lv_obj_get_child(forget, -1), "Hold to forget all browsers");

    if (web_server_running()) web_server_set_pairing(true);
    refresh();
    w.timer = lv_timer_create(timer_cb, 1000, NULL);
    lv_obj_add_event_cb(content, page_delete_cb, LV_EVENT_DELETE, NULL);
}

#endif /* FACTORY_WATCH_UI && FACTORY_WEB */
