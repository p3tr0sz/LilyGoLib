/**
 * @file      ui_nucleo_tools.cpp
 * @brief     P9 NucleoOS parity tools — watch LVGL UI pages.
 *
 * Conventions (watch-ux-guidelines.md):
 *   - ui_create_app_page() — swipe-right back nav + page animation (2.3, 2.5)
 *   - ui_create_card() — card groups (3.7)
 *   - ui_create_card_info() — status row with icon, title, value (3.7)
 *   - ui_create_card_button() — full-width action pill (3.2, phase 6)
 *   - row_value() → lv_label_set_text() for live status updates
 *   - menu_show() on back to return to app list
 *   - ui_destroy_app_page() on exit
 *   - app_t with .setup_func_cb / .exit_func_cb named fields
 */
#include "ui_define.h"

#if !defined(EXCLUDE_NUCLEO_TOOLS) && defined(FACTORY_WEB)

#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(font_alibaba_12);
LV_FONT_DECLARE(font_alibaba_24);

/* ================================================================ Helpers ===================== */

/* Return the value label child (last child) of a ui_create_card_info / ui_create_card_item row. */
static lv_obj_t *row_value(lv_obj_t *row)
{
    if (!row) return NULL;
    uint32_t n = lv_obj_get_child_count(row);
    return n ? lv_obj_get_child(row, n - 1) : NULL;
}

static void nucleo_cleanup(lv_obj_t **page, lv_timer_t **timer)
{
    if (timer && *timer) { lv_timer_del(*timer); *timer = NULL; }
    if (page && *page) { lv_obj_t *p = *page; *page = NULL; ui_destroy_app_page(p); }
    menu_show();
}

/* ================================================================ App macro ==================== */

#define NUCLEO_APP(name, Title, icon, desc)                                                    \
                                                                                               \
static lv_obj_t *p_##name = NULL;                                                              \
static lv_timer_t *t_##name = NULL;                                                            \
static lv_obj_t *v_##name = NULL;   /* status value label */                                   \
                                                                                               \
static void back_##name(lv_event_t *e) { (void)e; nucleo_cleanup(&p_##name, &t_##name); }      \
                                                                                               \
static void start_##name(lv_event_t *e) {                                                       \
    (void)e; if (v_##name) lv_label_set_text(v_##name, "Running");                             \
}                                                                                              \
                                                                                               \
static void stop_##name(lv_event_t *e) {                                                        \
    (void)e; if (v_##name) lv_label_set_text(v_##name, "Idle");                                \
}                                                                                              \
                                                                                               \
static void enter_##name(lv_obj_t *parent)                                                      \
{                                                                                              \
    p_##name = ui_create_app_page(parent, Title, back_##name);                                 \
                                                                                               \
    lv_obj_t *card = ui_create_card(p_##name, "Status");                                       \
    v_##name = row_value(ui_create_card_info(card, icon, "Status", "Idle"));                   \
                                                                                               \
    card = ui_create_card(p_##name, "Control");                                                \
    ui_create_card_button(card, LV_SYMBOL_PLAY, "Start", "Start", start_##name);              \
    ui_create_card_button(card, LV_SYMBOL_STOP, "Stop", "Stop", stop_##name);                 \
                                                                                               \
    ui_create_card_item(card, LV_SYMBOL_BELL, desc, NULL);                                     \
}                                                                                              \
                                                                                               \
static void exit_##name(lv_obj_t *parent)                                                       \
{                                                                                              \
    (void)parent;                                                                              \
    if (t_##name) { lv_timer_del(t_##name); t_##name = NULL; }                                 \
    if (p_##name) { ui_destroy_app_page(p_##name); p_##name = NULL; }                          \
}

/* ================================================================ All 8 tool apps ============ */

NUCLEO_APP(sniffer,     "Wi-Fi Sniffer",   LV_SYMBOL_WIFI,     "Passive 802.11 capture to pcap");
NUCLEO_APP(airspace,    "Airspace",        LV_SYMBOL_WIFI,     "Attack detection: deauth, beacon, evil-twin");
NUCLEO_APP(sentinel,    "Sentinel",         LV_SYMBOL_BLUETOOTH, "BLE anti-stalking (passive)");
NUCLEO_APP(presence,    "Presence",         LV_SYMBOL_WIFI,     "ESP-NOW nearby peer discovery");
NUCLEO_APP(fido2,       "FIDO2 Key",        LV_SYMBOL_USB,     "USB/BLE security key (passkeys)");
NUCLEO_APP(evilportal,  "Evil Portal",      LV_SYMBOL_WIFI,     "Rogue AP + captive portal (audit)");
NUCLEO_APP(deauthflood, "Deauth Flood",     LV_SYMBOL_WIFI,     "802.11 deauth injection (audit)");
NUCLEO_APP(beaconspam,  "Beacon Spam",      LV_SYMBOL_WIFI,     "Fake SSID beacon flood (audit)");

/* ================================================================ Public app_t entries ======== */

app_t ui_sniffer_main     = { .setup_func_cb = enter_sniffer,     .exit_func_cb = exit_sniffer };
app_t ui_airspace_main    = { .setup_func_cb = enter_airspace,    .exit_func_cb = exit_airspace };
app_t ui_sentinel_main    = { .setup_func_cb = enter_sentinel,    .exit_func_cb = exit_sentinel };
app_t ui_presence_main    = { .setup_func_cb = enter_presence,    .exit_func_cb = exit_presence };
app_t ui_fido2_main       = { .setup_func_cb = enter_fido2,       .exit_func_cb = exit_fido2 };
app_t ui_evilportal_main  = { .setup_func_cb = enter_evilportal,  .exit_func_cb = exit_evilportal };
app_t ui_deauthflood_main = { .setup_func_cb = enter_deauthflood, .exit_func_cb = exit_deauthflood };
app_t ui_beaconspam_main  = { .setup_func_cb = enter_beaconspam,  .exit_func_cb = exit_beaconspam };

#else /* EXCLUDE_NUCLEO_TOOLS stubs */

app_t ui_sniffer_main = { NULL, NULL, NULL };
app_t ui_airspace_main = { NULL, NULL, NULL };
app_t ui_sentinel_main = { NULL, NULL, NULL };
app_t ui_presence_main = { NULL, NULL, NULL };
app_t ui_fido2_main = { NULL, NULL, NULL };
app_t ui_evilportal_main = { NULL, NULL, NULL };
app_t ui_deauthflood_main = { NULL, NULL, NULL };
app_t ui_beaconspam_main = { NULL, NULL, NULL };

#endif /* EXCLUDE_NUCLEO_TOOLS */