/**
 * @file      ui_nucleo_tools.cpp
 * @brief     Security tool watch pages backed by the same lifecycle as the web API.
 *
 * UX: large shared controls, wrapped explanatory text, text + colour status,
 * pickers instead of typing, and no optimistic success labels. Every start is
 * checked; failures stay visible beside the action that failed.
 */
#include "ui_define.h"
#include "web_tools.h"

#if !defined(EXCLUDE_NUCLEO_TOOLS) && defined(FACTORY_WEB)

#include <stdarg.h>

struct native_page_t {
    lv_obj_t *page = NULL;
    lv_timer_t *timer = NULL;
    const web_tool_t *tool = NULL;
    uint32_t last_second = 0;
};

static lv_obj_t *row_value(lv_obj_t *row)
{
    if (!row) return NULL;
    uint32_t n = lv_obj_get_child_count(row);
    return n ? lv_obj_get_child(row, n - 1) : NULL;
}

static void label_set(lv_obj_t *label, const char *fmt, ...)
{
    if (!label) return;
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    const char *old = lv_label_get_text(label);
    if (old && !strcmp(old, text)) return;
    lv_label_set_text(label, text);
}

static lv_obj_t *info_row(lv_obj_t *card, const char *icon, const char *title, const char *value)
{
    return row_value(ui_create_card_info(card, icon, title, value));
}

static lv_obj_t *wrapped_text(lv_obj_t *card, const char *text, bool warning = false)
{
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(label, warning ? UI_COLOR_WARNING : UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    lv_obj_set_style_pad_bottom(label, 8, 0);
    return label;
}

static lv_obj_t *action_row(lv_obj_t *card, const char *icon, const char *text, lv_event_cb_t cb)
{
    return ui_create_card_button(card, icon, NULL, text, cb);
}

/* ui_create_card_item() turns this into the watch's full-screen keyboard page.
 * On other factory UIs it remains a normal one-line text field. */
static lv_obj_t *text_input_row(lv_obj_t *card, const char *title, const char *placeholder, uint32_t max_length)
{
    lv_obj_t *ta = lv_textarea_create(lv_obj_create(card));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_length);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, 150);
    ui_create_card_item(card, LV_SYMBOL_EDIT, title, ta);
    return ta;
}

static void row_disabled(lv_obj_t *row, bool disabled)
{
    lv_obj_t *obj = row_value(row);
    if (!obj) return;
    if (disabled) lv_obj_add_state(obj, LV_STATE_DISABLED);
    else lv_obj_remove_state(obj, LV_STATE_DISABLED);
}

static void drive_native(native_page_t &ui)
{
    /* With the server off, this page drives the tool. With it on, web_tools.cpp
     * owns the single 50 ms timer; never tick twice. */
    if (web_server_running() || !ui.tool) return;
    uint32_t now = lv_tick_get();
    if (ui.tool->tick) ui.tool->tick(now);
    if (ui.tool->second && now - ui.last_second >= 1000) {
        ui.last_second = now;
        ui.tool->second(now);
    }
}

static void close_native(native_page_t &ui, bool (*stop)(void), bool show_menu)
{
    if (ui.timer) { lv_timer_del(ui.timer); ui.timer = NULL; }
    if (stop) stop();
    if (ui.page) {
        lv_obj_t *page = ui.page;
        ui.page = NULL;
        ui_destroy_app_page(page);
    }
    if (show_menu) menu_show();
}

static void exit_native(native_page_t &ui, bool (*stop)(void))
{
    close_native(ui, stop, false);
}

/* ================================================================ Wi-Fi Sniffer */

static native_page_t sniffer_ui;
static lv_obj_t *sniffer_channel, *sniffer_filter, *sniffer_state, *sniffer_packets, *sniffer_bytes, *sniffer_analysis, *sniffer_file, *sniffer_error;
static lv_obj_t *sniffer_start_row, *sniffer_stop_row;
static const char *const SNIFFER_FILTERS[] = { "all", "management", "data", "beacons", "probes", "eapol" };

static void sniffer_refresh(lv_timer_t *)
{
    drive_native(sniffer_ui);
    web_hw_sniffer_t st;
    web_hw_sniffer_status(&st);
    label_set(sniffer_state, st.active ? "Capturing" : "Stopped");
    label_set(sniffer_packets, "%lu saved / %lu seen / drop %lu",
              (unsigned long)st.packet_count, (unsigned long)st.seen, (unsigned long)st.dropped);
    label_set(sniffer_bytes, "%lu B", (unsigned long)st.bytes_written);
    label_set(sniffer_analysis, "Beacons %lu  Probes %lu  EAPOL %lu  HS %lu",
              (unsigned long)st.beacons, (unsigned long)st.probes,
              (unsigned long)st.eapol, (unsigned long)st.handshakes);
    label_set(sniffer_file, st.file[0] ? st.file : "No capture yet");
    label_set(sniffer_error, st.error ? st.error : "");
    row_disabled(sniffer_start_row, st.active);
    row_disabled(sniffer_stop_row, !st.active);
    if (sniffer_channel) {
        if (st.active) lv_obj_add_state(sniffer_channel, LV_STATE_DISABLED);
        else lv_obj_remove_state(sniffer_channel, LV_STATE_DISABLED);
    }
    if (sniffer_filter) {
        if (st.active) lv_obj_add_state(sniffer_filter, LV_STATE_DISABLED);
        else lv_obj_remove_state(sniffer_filter, LV_STATE_DISABLED);
    }
}
static void sniffer_back(lv_event_t *) { close_native(sniffer_ui, web_hw_sniffer_stop, true); }
static void sniffer_start(lv_event_t *)
{
    int sel = sniffer_channel ? (int)lv_dropdown_get_selected(sniffer_channel) : 0;
    int fi = sniffer_filter ? (int)lv_dropdown_get_selected(sniffer_filter) : 0;
    if (fi < 0 || fi >= (int)(sizeof(SNIFFER_FILTERS) / sizeof(SNIFFER_FILTERS[0]))) fi = 0;
    web_hw_sniffer_start_filter(sel == 0 ? -1 : sel, 0, NULL, SNIFFER_FILTERS[fi]);
    sniffer_refresh(NULL);
}
static void sniffer_stop_cb(lv_event_t *) { web_hw_sniffer_stop(); sniffer_refresh(NULL); }
static void enter_sniffer(lv_obj_t *parent)
{
    sniffer_ui = {};
    sniffer_ui.tool = &web_tool_sniffer;
    sniffer_ui.page = ui_create_app_page(parent, "Wi-Fi Sniffer", sniffer_back);
    lv_obj_t *card = ui_create_card(sniffer_ui.page, "Status");
    sniffer_state = info_row(card, LV_SYMBOL_WIFI, "State", "Stopped");
    sniffer_packets = info_row(card, LV_SYMBOL_LIST, "Packets", "0 saved / 0 seen");
    sniffer_bytes = info_row(card, LV_SYMBOL_SAVE, "Written", "0 B");
    sniffer_analysis = info_row(card, LV_SYMBOL_EYE_OPEN, "Analysis", "Beacons 0  Probes 0  EAPOL 0  HS 0");
    sniffer_file = wrapped_text(card, "No capture yet");
    sniffer_error = wrapped_text(card, "", true);

    card = ui_create_card(sniffer_ui.page, "Capture");
    static const char options[] = "All channels\nChannel 1\nChannel 2\nChannel 3\nChannel 4\nChannel 5\nChannel 6\nChannel 7\nChannel 8\nChannel 9\nChannel 10\nChannel 11\nChannel 12\nChannel 13";
    sniffer_channel = row_value(ui_create_card_dropdown(card, NULL, "Channel", options, 0, NULL));
    sniffer_filter = row_value(ui_create_card_dropdown(card, NULL, "Filter",
        "All frames\nManagement only\nData only\nBeacons only\nProbes only\nEAPOL only", 0, NULL));
    sniffer_start_row = action_row(card, LV_SYMBOL_PLAY, "Start capture", sniffer_start);
    sniffer_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop and save", sniffer_stop_cb);
    wrapped_text(card, "Passive capture to PCAP. Inspect only authorized networks.");
    sniffer_ui.timer = lv_timer_create(sniffer_refresh, 500, NULL);
    sniffer_refresh(NULL);
}
static void exit_sniffer(lv_obj_t *) { exit_native(sniffer_ui, web_hw_sniffer_stop); }

/* ================================================================ Airspace */

static native_page_t airspace_ui;
static lv_obj_t *air_state, *air_deauth, *air_beacons, *air_alerts, *air_error, *air_start_row, *air_stop_row;
static void airspace_refresh(lv_timer_t *)
{
    drive_native(airspace_ui);
    web_hw_airspace_t st;
    web_hw_airspace_status(&st);
    label_set(air_state, st.active ? "Monitoring" : "Stopped");
    label_set(air_deauth, "%lu", (unsigned long)st.deauth_count);
    label_set(air_beacons, "%lu", (unsigned long)st.beacon_count);
    label_set(air_alerts, "D%lu B%lu T%lu X%lu", (unsigned long)st.deauth_floods,
              (unsigned long)st.beacon_floods, (unsigned long)st.evil_twins, (unsigned long)st.broadcast_deauth);
    label_set(air_error, st.error ? st.error : "D=flood B=beacon T=twin X=broadcast");
    row_disabled(air_start_row, st.active);
    row_disabled(air_stop_row, !st.active);
}
static void airspace_back(lv_event_t *) { close_native(airspace_ui, web_hw_airspace_stop, true); }
static void airspace_start(lv_event_t *) { web_hw_airspace_start(); airspace_refresh(NULL); }
static void airspace_stop_cb(lv_event_t *) { web_hw_airspace_stop(); airspace_refresh(NULL); }
static void enter_airspace(lv_obj_t *parent)
{
    airspace_ui = {};
    airspace_ui.tool = &web_tool_airspace;
    airspace_ui.page = ui_create_app_page(parent, "Airspace", airspace_back);
    lv_obj_t *card = ui_create_card(airspace_ui.page, "Passive monitor");
    air_state = info_row(card, LV_SYMBOL_WIFI, "State", "Stopped");
    air_deauth = info_row(card, LV_SYMBOL_WARNING, "Deauth", "0");
    air_beacons = info_row(card, LV_SYMBOL_LIST, "Beacons", "0");
    air_alerts = info_row(card, LV_SYMBOL_BELL, "Alerts", "0");
    air_error = wrapped_text(card, "D=flood B=beacon T=twin X=broadcast");
    card = ui_create_card(airspace_ui.page, "Control");
    air_start_row = action_row(card, LV_SYMBOL_PLAY, "Start monitor", airspace_start);
    air_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop monitor", airspace_stop_cb);
    wrapped_text(card, "Listen only. Alerts need investigation; they are not proof of intent.");
    airspace_ui.timer = lv_timer_create(airspace_refresh, 1000, NULL);
    airspace_refresh(NULL);
}
static void exit_airspace(lv_obj_t *) { exit_native(airspace_ui, web_hw_airspace_stop); }

/* ================================================================ Sentinel */

static native_page_t sentinel_ui;
static lv_obj_t *sent_state, *sent_count, *sent_error, *sent_start_row, *sent_stop_row, *sent_rows[4];
static void sentinel_refresh(lv_timer_t *)
{
    drive_native(sentinel_ui);
    static web_hw_sentinel_t st;
    web_hw_sentinel_status(&st);
    int persistent = 0;
    for (int i = 0; i < st.device_count; i++) if (st.devices[i].stalking) persistent++;
    label_set(sent_state, st.active ? (st.scanning ? "Listening" : "Active") : "Stopped");
    label_set(sent_count, "%d nearby / %d persistent", st.device_count, persistent);
    label_set(sent_error, st.error ? st.error : "Persistent is a heuristic, not proof");
    row_disabled(sent_start_row, st.active);
    row_disabled(sent_stop_row, !st.active);
    for (int i = 0; i < 4; i++) {
        if (i < st.device_count) {
            const auto &d = st.devices[i];
            label_set(sent_rows[i], "%s\n%s · %d dBm · %d sightings", d.kind,
                      d.stalking ? "Persistent: check belongings" : "Nearby", d.rssi, d.sightings);
        } else label_set(sent_rows[i], "");
    }
}
static void sentinel_back(lv_event_t *) { close_native(sentinel_ui, web_hw_sentinel_stop, true); }
static void sentinel_start(lv_event_t *) { web_hw_sentinel_start(); sentinel_refresh(NULL); }
static void sentinel_stop_cb(lv_event_t *) { web_hw_sentinel_stop(); sentinel_refresh(NULL); }
static void enter_sentinel(lv_obj_t *parent)
{
    sentinel_ui = {};
    sentinel_ui.tool = &web_tool_sentinel;
    sentinel_ui.page = ui_create_app_page(parent, "Sentinel", sentinel_back);
    lv_obj_t *card = ui_create_card(sentinel_ui.page, "BLE safety check");
    sent_state = info_row(card, LV_SYMBOL_BLUETOOTH, "State", "Stopped");
    sent_count = info_row(card, LV_SYMBOL_LIST, "Trackers", "0");
    sent_error = wrapped_text(card, "Persistent is a heuristic, not proof");
    card = ui_create_card(sentinel_ui.page, "Control");
    sent_start_row = action_row(card, LV_SYMBOL_PLAY, "Start passive scan", sentinel_start);
    sent_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop scan", sentinel_stop_cb);
    card = ui_create_card(sentinel_ui.page, "Known trackers");
    for (int i = 0; i < 4; i++) sent_rows[i] = wrapped_text(card, "");
    wrapped_text(card, "Find My, SmartTag, Tile and Chipolo. No scan requests sent.");
    sentinel_ui.timer = lv_timer_create(sentinel_refresh, 1000, NULL);
    sentinel_refresh(NULL);
}
static void exit_sentinel(lv_obj_t *) { exit_native(sentinel_ui, web_hw_sentinel_stop); }

/* ================================================================ Presence: Wi-Fi CSI + nearby peers */

static native_page_t presence_ui;
static lv_obj_t *presence_state, *presence_count, *presence_error, *presence_start_row, *presence_stop_row, *presence_rows[4];
static lv_obj_t *csi_state, *csi_metrics, *csi_network, *csi_error, *csi_threshold;
static lv_obj_t *csi_start_row, *csi_stop_row, *csi_calibrate_row;
static const float CSI_THRESHOLDS[] = { 0.20f, 0.35f, 0.55f };

static void presence_refresh(lv_timer_t *)
{
    drive_native(presence_ui);
    static web_hw_presence_t st;
    web_hw_presence_status(&st);
    web_hw_csi_presence_t csi;
    web_hw_presence_csi_status(&csi);
    label_set(csi_state, !csi.active ? "Stopped" : csi.calibrating ? "Calibrating" : csi.motion ? "Motion" : csi.present ? "Present" : "Clear");
    label_set(csi_metrics, "%d%% motion · %lu/s · drop %lu", (int)(csi.score * 100 + 0.5f),
              (unsigned long)csi.rate, (unsigned long)csi.drops);
    label_set(csi_network, "%s · ch %d · %d dBm", csi.ssid[0] ? csi.ssid : "No network", csi.channel, csi.rssi);
    label_set(csi_error, csi.error ? csi.error : "Calibrate in an empty room. Presence is an estimate.");
    row_disabled(csi_start_row, csi.active);
    row_disabled(csi_stop_row, !csi.active);
    row_disabled(csi_calibrate_row, !csi.active);
    if (csi_threshold) {
        if (csi.active) lv_obj_add_state(csi_threshold, LV_STATE_DISABLED);
        else lv_obj_remove_state(csi_threshold, LV_STATE_DISABLED);
    }

    int now = 0;
    for (int i = 0; i < st.peer_count; i++) if (st.peers[i].present) now++;
    label_set(presence_state, st.active ? "Discovering" : "Stopped");
    label_set(presence_count, "%d now / %d ever", now, st.peers_ever);
    label_set(presence_error, st.error ? st.error : "Separate ESP-NOW watch/peer discovery");
    row_disabled(presence_start_row, st.active);
    row_disabled(presence_stop_row, !st.active);
    int shown = 0;
    for (int i = 0; i < st.peer_count && shown < 4; i++) {
        if (!st.peers[i].present) continue;
        label_set(presence_rows[shown++], "%s\n%s · %d dBm", st.peers[i].name[0] ? st.peers[i].name : "Unknown peer", st.peers[i].mac, st.peers[i].rssi);
    }
    for (; shown < 4; shown++) label_set(presence_rows[shown], "");
}
static bool presence_stop_all(void)
{
    web_hw_presence_csi_stop();
    web_hw_presence_stop();
    return true;
}
static void presence_back(lv_event_t *) { close_native(presence_ui, presence_stop_all, true); }
static void presence_start(lv_event_t *) { web_hw_presence_start(); presence_refresh(NULL); }
static void presence_stop_cb(lv_event_t *) { web_hw_presence_stop(); presence_refresh(NULL); }
static void csi_start(lv_event_t *)
{
    int selected = csi_threshold ? (int)lv_dropdown_get_selected(csi_threshold) : 1;
    if (selected < 0 || selected > 2) selected = 1;
    web_hw_presence_csi_start(CSI_THRESHOLDS[selected]);
    presence_refresh(NULL);
}
static void csi_stop(lv_event_t *) { web_hw_presence_csi_stop(); presence_refresh(NULL); }
static void csi_calibrate(lv_event_t *) { web_hw_presence_csi_calibrate(); presence_refresh(NULL); }
static void enter_presence(lv_obj_t *parent)
{
    presence_ui = {};
    presence_ui.tool = &web_tool_presence;
    presence_ui.page = ui_create_app_page(parent, "Presence", presence_back);
    lv_obj_t *card = ui_create_card(presence_ui.page, "Wi-Fi CSI motion");
    csi_state = info_row(card, LV_SYMBOL_EYE_OPEN, "State", "Stopped");
    csi_metrics = info_row(card, LV_SYMBOL_WIFI, "Activity", "0% motion · 0/s");
    csi_network = wrapped_text(card, "No network");
    csi_error = wrapped_text(card, "Calibrate in an empty room. Presence is an estimate.", true);
    csi_threshold = row_value(ui_create_card_dropdown(card, NULL, "Sensitivity",
        "High (0.20)\nBalanced (0.35)\nLow (0.55)", 1, NULL));
    csi_start_row = action_row(card, LV_SYMBOL_PLAY, "Start CSI sensing", csi_start);
    csi_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop CSI sensing", csi_stop);
    csi_calibrate_row = action_row(card, LV_SYMBOL_REFRESH, "Recalibrate", csi_calibrate);
    wrapped_text(card, "Uses changes in Wi-Fi channel response from the connected 2.4 GHz router. Keep watch and router fixed.");

    card = ui_create_card(presence_ui.page, "Nearby ESP-NOW peers");
    presence_state = info_row(card, LV_SYMBOL_WIFI, "Discovery", "Stopped");
    presence_count = info_row(card, LV_SYMBOL_LIST, "Peers", "0");
    presence_error = wrapped_text(card, "Separate ESP-NOW watch/peer discovery");
    presence_start_row = action_row(card, LV_SYMBOL_PLAY, "Start discovery", presence_start);
    presence_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop discovery", presence_stop_cb);
    for (int i = 0; i < 4; i++) presence_rows[i] = wrapped_text(card, "");
    presence_ui.timer = lv_timer_create(presence_refresh, 1000, NULL);
    presence_refresh(NULL);
}
static void exit_presence(lv_obj_t *) { exit_native(presence_ui, presence_stop_all); }

/* ================================================================ FIDO2 (honest unavailable state) */

static native_page_t fido_ui;
static lv_obj_t *fido_state, *fido_creds, *fido_error;
static void fido_refresh(lv_timer_t *)
{
    web_hw_fido2_t st;
    web_hw_fido2_status(&st);
    label_set(fido_state, st.available ? "Available" : "Unavailable");
    label_set(fido_creds, "%d", st.credentials);
    label_set(fido_error, st.error ? st.error : "No FIDO2 transport");
}
static void fido_back(lv_event_t *) { close_native(fido_ui, NULL, true); }
static void enter_fido2(lv_obj_t *parent)
{
    fido_ui = {};
    fido_ui.page = ui_create_app_page(parent, "FIDO2 Key", fido_back);
    lv_obj_t *card = ui_create_card(fido_ui.page, "Transport status");
    fido_state = info_row(card, LV_SYMBOL_USB, "State", "Unavailable");
    fido_creds = info_row(card, LV_SYMBOL_LIST, "Credentials", "0");
    fido_error = wrapped_text(card, "", true);
    wrapped_text(card, "Keyboard HID is not FIDO2. This build has no CTAP transport, credential crypto or secure key storage, so no Register or Authenticate action is offered.");
    fido_ui.timer = lv_timer_create(fido_refresh, 1000, NULL);
    fido_refresh(NULL);
}
static void exit_fido2(lv_obj_t *) { exit_native(fido_ui, NULL); }

/* ================================================================ Evil Portal */

static native_page_t portal_ui;
static bool portal_consented = false;
static lv_obj_t *portal_ssid, *portal_ssid_text, *portal_template, *portal_state, *portal_clients, *portal_attempts, *portal_error;
static lv_obj_t *portal_start_row, *portal_stop_row;
static void portal_build(void);
static void portal_refresh(lv_timer_t *)
{
    if (!portal_consented || !portal_state) return;
    drive_native(portal_ui);
    web_hw_evilportal_t st;
    web_hw_evilportal_status(&st);
    label_set(portal_state, st.active ? "Active" : "Stopped");
    label_set(portal_clients, "%d", st.clients);
    label_set(portal_attempts, "%lu", (unsigned long)st.attempts);
    label_set(portal_error, st.error ? st.error : "");
    row_disabled(portal_start_row, st.active);
    row_disabled(portal_stop_row, !st.active);
}
static void portal_back(lv_event_t *) { close_native(portal_ui, web_hw_evilportal_stop, true); }
static void portal_start(lv_event_t *)
{
    int ssid_idx = portal_ssid ? lv_dropdown_get_selected(portal_ssid) : 0;
    int template_idx = portal_template ? lv_dropdown_get_selected(portal_template) : 0;
    const char *manual = portal_ssid_text ? lv_textarea_get_text(portal_ssid_text) : "";
    const char *ssid = manual && *manual ? manual : web_hw_evilportal_ssid_preset(ssid_idx);
    web_hw_evilportal_start(template_idx, ssid, web_hw_evilportal_gateway());
    portal_refresh(NULL);
}
static void portal_stop_cb(lv_event_t *) { web_hw_evilportal_stop(); portal_refresh(NULL); }
static void portal_consent(lv_event_t *)
{
    portal_consented = true;
    lv_obj_clean(portal_ui.page);
    portal_build();
}
static void portal_build(void)
{
    lv_obj_t *card = ui_create_card(portal_ui.page, "Status");
    portal_state = info_row(card, LV_SYMBOL_WIFI, "State", "Stopped");
    portal_clients = info_row(card, LV_SYMBOL_LIST, "Clients", "0");
    portal_attempts = info_row(card, LV_SYMBOL_LIST, "Captures", "0");
    portal_error = wrapped_text(card, "", true);
    card = ui_create_card(portal_ui.page, "Standalone portal");
    char ssids[384] = "";
    for (int i = 0; i < web_hw_evilportal_ssid_count(); i++) {
        if (i) strlcat(ssids, "\n", sizeof(ssids));
        strlcat(ssids, web_hw_evilportal_ssid_preset(i), sizeof(ssids));
    }
    portal_ssid = row_value(ui_create_card_dropdown(card, NULL, "SSID preset", ssids, 0, NULL));
    portal_ssid_text = text_input_row(card, "Manual SSID", "Empty: use preset", 32);
    char templates[192] = "";
    for (int i = 0; i < web_hw_evilportal_template_count(); i++) {
        if (i) strlcat(templates, "\n", sizeof(templates));
        strlcat(templates, web_hw_evilportal_template_name(i), sizeof(templates));
    }
    portal_template = row_value(ui_create_card_dropdown(card, NULL, "Template", templates, 0, NULL));
    portal_start_row = action_row(card, LV_SYMBOL_PLAY, "Start portal", portal_start);
    portal_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop portal", portal_stop_cb);
    wrapped_text(card, "Standalone mode on watch. Manual SSID overrides the preset. Passwords stay hidden in the browser by default. Closing this page stops the AP.");
}
static void enter_evilportal(lv_obj_t *parent)
{
    portal_ui = {};
    portal_ui.tool = &web_tool_evilportal;
    portal_ui.page = ui_create_app_page(parent, "Evil Portal", portal_back);
    if (!portal_consented) {
        lv_obj_t *card = ui_create_card(portal_ui.page, "Authorized use only");
        wrapped_text(card, "Creates a rogue AP and captures credentials. Use only on your own network or with written permission.", true);
        wrapped_text(card, "Captures are sensitive. Closing the page stops the portal.");
        card = ui_create_card(portal_ui.page, NULL);
        action_row(card, LV_SYMBOL_OK, "I am authorized", portal_consent);
    } else portal_build();
    portal_ui.timer = lv_timer_create(portal_refresh, 1000, NULL);
}
static void exit_evilportal(lv_obj_t *) { exit_native(portal_ui, web_hw_evilportal_stop); }

/* ================================================================ Deauth Flood */

static native_page_t deauth_ui;
static bool deauth_consented = false;
static lv_obj_t *deauth_target, *deauth_rate, *deauth_state, *deauth_sent, *deauth_error, *deauth_scan_label;
static lv_obj_t *deauth_start_row, *deauth_stop_row, *deauth_scan_row;
static char deauth_options[1024] = "Scan first";
static int deauth_options_count = -1;
static bool deauth_scan_was = false;
static void deauth_build(void);
static void deauth_refresh(lv_timer_t *)
{
    if (!deauth_consented || !deauth_state) return;
    drive_native(deauth_ui);
    web_hw_deauthflood_t st;
    web_hw_deauthflood_status(&st);
    bool scanning = web_hw_deauthflood_scanning();
    int count = web_hw_deauthflood_target_count();
    label_set(deauth_state, st.active ? "Injecting" : scanning ? "Scanning" : "Stopped");
    label_set(deauth_sent, "%lu", (unsigned long)st.sent_count);
    label_set(deauth_error, st.error ? st.error : "WPA3/PMF may ignore deauth");
    label_set(deauth_scan_label, scanning ? "Scanning channels..." : count ? "Select a target" : "Scan for targets");
    row_disabled(deauth_start_row, st.active || scanning || count == 0);
    row_disabled(deauth_stop_row, !st.active);
    row_disabled(deauth_scan_row, st.active || scanning);
    if (scanning) deauth_scan_was = true;
    if (!scanning && count > 0 && deauth_target &&
        (deauth_scan_was || count != deauth_options_count || !strcmp(deauth_options, "Scan first"))) {
        deauth_scan_was = false;
        deauth_options_count = count;
        deauth_options[0] = 0;
        int used = 0;
        for (int i = 0; i < count && i < 20; i++) {
            int n = snprintf(deauth_options + used, sizeof(deauth_options) - used, "%s%s (ch%d)", i ? "\n" : "",
                             web_hw_deauthflood_target_ssid(i)[0] ? web_hw_deauthflood_target_ssid(i) : "(hidden)",
                             web_hw_deauthflood_target_channel(i));
            if (n < 0 || used + n >= (int)sizeof(deauth_options)) break;
            used += n;
        }
        lv_dropdown_set_options(deauth_target, deauth_options);
        lv_dropdown_set_selected(deauth_target, 0);
    }
}
static void deauth_back(lv_event_t *) { close_native(deauth_ui, web_hw_deauthflood_stop, true); }
static void deauth_scan(lv_event_t *) { web_hw_deauthflood_scan_targets(); deauth_refresh(NULL); }
static void deauth_start(lv_event_t *)
{
    int idx = deauth_target ? lv_dropdown_get_selected(deauth_target) : -1;
    int rate_idx = deauth_rate ? lv_dropdown_get_selected(deauth_rate) : 2;
    static const int rates[] = {1, 5, 10, 20, 50};
    if (idx >= 0 && idx < web_hw_deauthflood_target_count()) {
        web_hw_deauthflood_start(web_hw_deauthflood_target_bssid(idx), "ff:ff:ff:ff:ff:ff",
                                 web_hw_deauthflood_target_channel(idx), rates[rate_idx]);
    }
    deauth_refresh(NULL);
}
static void deauth_stop_cb(lv_event_t *) { web_hw_deauthflood_stop(); deauth_refresh(NULL); }
static void deauth_consent(lv_event_t *) { deauth_consented = true; lv_obj_clean(deauth_ui.page); deauth_build(); }
static void deauth_build(void)
{
    lv_obj_t *card = ui_create_card(deauth_ui.page, "Status");
    deauth_state = info_row(card, LV_SYMBOL_WIFI, "State", "Stopped");
    deauth_sent = info_row(card, LV_SYMBOL_LIST, "Accepted frames", "0");
    deauth_error = wrapped_text(card, "WPA3/PMF may ignore deauth");
    card = ui_create_card(deauth_ui.page, "Target");
    deauth_target = row_value(ui_create_card_dropdown(card, NULL, "AP", deauth_options, 0, NULL));
    deauth_rate = row_value(ui_create_card_dropdown(card, NULL, "Packets/s", "1\n5\n10\n20\n50", 2, NULL));
    deauth_scan_row = action_row(card, LV_SYMBOL_REFRESH, "Scan APs", deauth_scan);
    deauth_scan_label = wrapped_text(card, "Scan for targets");
    deauth_start_row = action_row(card, LV_SYMBOL_PLAY, "Start test", deauth_start);
    deauth_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop and reconnect", deauth_stop_cb);
    wrapped_text(card, "Broadcast station. Normal Wi-Fi disconnects during the test. Closing this page stops injection.", true);
}
static void enter_deauthflood(lv_obj_t *parent)
{
    deauth_ui = {};
    deauth_ui.tool = &web_tool_deauthflood;
    deauth_ui.page = ui_create_app_page(parent, "Deauth Flood", deauth_back);
    if (!deauth_consented) {
        lv_obj_t *card = ui_create_card(deauth_ui.page, "Authorized use only");
        wrapped_text(card, "Disconnects clients from an AP. Use only on your own network or with written permission.", true);
        wrapped_text(card, "Closing this page stops injection and requests Wi-Fi reconnect.");
        card = ui_create_card(deauth_ui.page, NULL);
        action_row(card, LV_SYMBOL_OK, "I am authorized", deauth_consent);
    } else deauth_build();
    deauth_ui.timer = lv_timer_create(deauth_refresh, 500, NULL);
}
static void exit_deauthflood(lv_obj_t *) { exit_native(deauth_ui, web_hw_deauthflood_stop); }

/* ================================================================ Beacon Spam */

static native_page_t beacon_ui;
static bool beacon_consented = false;
static lv_obj_t *beacon_prefix, *beacon_prefix_text, *beacon_count, *beacon_channel, *beacon_state, *beacon_sent, *beacon_error;
static lv_obj_t *beacon_start_row, *beacon_stop_row;
static void beacon_build(void);
static void beacon_refresh(lv_timer_t *)
{
    if (!beacon_consented || !beacon_state) return;
    drive_native(beacon_ui);
    web_hw_beaconspam_t st;
    web_hw_beaconspam_status(&st);
    label_set(beacon_state, st.active ? "Injecting" : "Stopped");
    label_set(beacon_sent, "%lu / %lu SSIDs", (unsigned long)st.beacon_count, (unsigned long)st.ssid_count);
    label_set(beacon_error, st.error ? st.error : "");
    row_disabled(beacon_start_row, st.active);
    row_disabled(beacon_stop_row, !st.active);
}
static void beacon_back(lv_event_t *) { close_native(beacon_ui, web_hw_beaconspam_stop, true); }
static void beacon_start(lv_event_t *)
{
    static const char *prefixes[] = {"FreeNet", "LabTest", "BeaconTest"};
    static const int counts[] = {20, 50, 100};
    int channel = beacon_channel ? (int)lv_dropdown_get_selected(beacon_channel) : 0;
    const char *manual = beacon_prefix_text ? lv_textarea_get_text(beacon_prefix_text) : "";
    const char *prefix = manual && *manual ? manual : prefixes[lv_dropdown_get_selected(beacon_prefix)];
    web_hw_beaconspam_start(channel, counts[lv_dropdown_get_selected(beacon_count)], prefix);
    beacon_refresh(NULL);
}
static void beacon_stop_cb(lv_event_t *) { web_hw_beaconspam_stop(); beacon_refresh(NULL); }
static void beacon_consent(lv_event_t *) { beacon_consented = true; lv_obj_clean(beacon_ui.page); beacon_build(); }
static void beacon_build(void)
{
    lv_obj_t *card = ui_create_card(beacon_ui.page, "Status");
    beacon_state = info_row(card, LV_SYMBOL_WIFI, "State", "Stopped");
    beacon_sent = info_row(card, LV_SYMBOL_LIST, "Accepted beacons", "0");
    beacon_error = wrapped_text(card, "", true);
    card = ui_create_card(beacon_ui.page, "Generated beacons");
    beacon_prefix = row_value(ui_create_card_dropdown(card, NULL, "Prefix preset", "FreeNet\nLabTest\nBeaconTest", 0, NULL));
    beacon_prefix_text = text_input_row(card, "Manual prefix", "Empty: use preset", 20);
    beacon_count = row_value(ui_create_card_dropdown(card, NULL, "SSID count", "20\n50\n100", 0, NULL));
    static const char channels[] = "Hop all\nChannel 1\nChannel 2\nChannel 3\nChannel 4\nChannel 5\nChannel 6\nChannel 7\nChannel 8\nChannel 9\nChannel 10\nChannel 11\nChannel 12\nChannel 13";
    beacon_channel = row_value(ui_create_card_dropdown(card, NULL, "Channel", channels, 0, NULL));
    beacon_start_row = action_row(card, LV_SYMBOL_PLAY, "Start test", beacon_start);
    beacon_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop and reconnect", beacon_stop_cb);
    wrapped_text(card, "Generated SSIDs on watch; manual prefix overrides the preset. Custom lists are configured in the browser. Closing this page stops injection.", true);
}
static void enter_beaconspam(lv_obj_t *parent)
{
    beacon_ui = {};
    beacon_ui.tool = &web_tool_beaconspam;
    beacon_ui.page = ui_create_app_page(parent, "Beacon Spam", beacon_back);
    if (!beacon_consented) {
        lv_obj_t *card = ui_create_card(beacon_ui.page, "Authorized use only");
        wrapped_text(card, "Floods nearby Wi-Fi lists with fake networks. Use only in a controlled environment you own.", true);
        wrapped_text(card, "Normal Wi-Fi disconnects. Closing this page stops injection.");
        card = ui_create_card(beacon_ui.page, NULL);
        action_row(card, LV_SYMBOL_OK, "I am authorized", beacon_consent);
    } else beacon_build();
    beacon_ui.timer = lv_timer_create(beacon_refresh, 500, NULL);
}
static void exit_beaconspam(lv_obj_t *) { exit_native(beacon_ui, web_hw_beaconspam_stop); }

/* ================================================================ Public entries */

app_t ui_sniffer_main = { .setup_func_cb = enter_sniffer, .exit_func_cb = exit_sniffer };
app_t ui_airspace_main = { .setup_func_cb = enter_airspace, .exit_func_cb = exit_airspace };
app_t ui_sentinel_main = { .setup_func_cb = enter_sentinel, .exit_func_cb = exit_sentinel };
app_t ui_presence_main = { .setup_func_cb = enter_presence, .exit_func_cb = exit_presence };
app_t ui_fido2_main = { .setup_func_cb = enter_fido2, .exit_func_cb = exit_fido2 };
app_t ui_evilportal_main = { .setup_func_cb = enter_evilportal, .exit_func_cb = exit_evilportal };
app_t ui_deauthflood_main = { .setup_func_cb = enter_deauthflood, .exit_func_cb = exit_deauthflood };
app_t ui_beaconspam_main = { .setup_func_cb = enter_beaconspam, .exit_func_cb = exit_beaconspam };

#else
app_t ui_sniffer_main = { NULL, NULL, NULL };
app_t ui_airspace_main = { NULL, NULL, NULL };
app_t ui_sentinel_main = { NULL, NULL, NULL };
app_t ui_presence_main = { NULL, NULL, NULL };
app_t ui_fido2_main = { NULL, NULL, NULL };
app_t ui_evilportal_main = { NULL, NULL, NULL };
app_t ui_deauthflood_main = { NULL, NULL, NULL };
app_t ui_beaconspam_main = { NULL, NULL, NULL };
#endif
