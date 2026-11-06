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

/* ============================================== Presence: dual-link CSI, Faro, alarm + peers */

static native_page_t presence_ui;
static lv_obj_t *presence_state, *presence_count, *presence_error, *presence_start_row, *presence_stop_row, *presence_rows[4];
static lv_obj_t *csi_state, *csi_metrics, *csi_network, *csi_error, *csi_threshold;
static lv_obj_t *csi_faro_state, *csi_faro_metrics, *csi_history;
static lv_chart_series_t *csi_router_series, *csi_faro_series;
static lv_obj_t *csi_start_row, *csi_stop_row, *csi_calibrate_row;
static lv_obj_t *faro_state, *faro_metrics, *faro_error, *faro_start_row, *faro_stop_row;
static lv_obj_t *alarm_state, *alarm_metrics, *alarm_error, *alarm_arm_row, *alarm_disarm_row;
static lv_obj_t *alarm_exit, *alarm_duration;
static const float CSI_THRESHOLDS[] = { 0.20f, 0.35f, 0.55f };
static const int ALARM_EXIT_VALUES[] = { 5, 10, 30, 60 };
static const int ALARM_DURATION_VALUES[] = { 10, 30, 60, 120 };

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
    label_set(csi_faro_state, !csi.faro.discovered ? "Waiting" : !csi.faro.linked ? "Absent" :
              csi.faro.calibrating ? "Calibrating" : csi.faro.motion ? "Motion" : csi.faro.present ? "Present" : "Clear");
    label_set(csi_faro_metrics, "%d%% · %lu/s · %d dBm\n%s", (int)(csi.faro.score * 100 + 0.5f),
              (unsigned long)csi.faro.rate, csi.faro.rssi, csi.faro.mac[0] ? csi.faro.mac : "No Faro discovered");
    label_set(csi_error, csi.error ? csi.error : "Calibrate each link in an empty room. Zones estimate radio paths, not locations.");
    row_disabled(csi_start_row, csi.active);
    row_disabled(csi_stop_row, !csi.active);
    row_disabled(csi_calibrate_row, !csi.active);
    if (csi_threshold) {
        if (csi.active) lv_obj_add_state(csi_threshold, LV_STATE_DISABLED);
        else lv_obj_remove_state(csi_threshold, LV_STATE_DISABLED);
    }
    if (csi_history) {
        lv_chart_set_all_values(csi_history, csi_router_series, LV_CHART_POINT_NONE);
        lv_chart_set_all_values(csi_history, csi_faro_series, LV_CHART_POINT_NONE);
        int offset = 120 - csi.history_count;
        for (int i = 0; i < csi.history_count; i++) {
            lv_chart_set_value_by_id(csi_history, csi_router_series, offset + i, csi.history[i].router);
            lv_chart_set_value_by_id(csi_history, csi_faro_series, offset + i, csi.history[i].faro);
        }
        lv_chart_refresh(csi_history);
    }

    web_hw_presence_faro_t faro;
    web_hw_presence_faro_status(&faro);
    label_set(faro_state, faro.active ? "Transmitting" : "Stopped");
    label_set(faro_metrics, "ch %d · %lu accepted · %lu/s · %lu errors\n%s", faro.channel,
              (unsigned long)faro.sent, (unsigned long)faro.rate, (unsigned long)faro.tx_errors,
              faro.mac[0] ? faro.mac : "Second-watch transmitter");
    label_set(faro_error, faro.error ? faro.error : "100 Hz ESP-NOW beacon. Same channel as receiver; no sensing in this mode.");
    row_disabled(faro_start_row, faro.active || csi.active);
    row_disabled(faro_stop_row, !faro.active);

    web_hw_presence_alarm_t alarm;
    web_hw_presence_alarm_status(&alarm);
    label_set(alarm_state, alarm.state);
    label_set(alarm_metrics, "exit %d s · siren %d s · left %d s · %lu triggers%s%s",
              alarm.exit_s, alarm.duration_s, alarm.remaining_s, (unsigned long)alarm.triggers,
              alarm.zone[0] ? " · " : "", alarm.zone);
    label_set(alarm_error, alarm.error ? alarm.error : "Only live motion triggers; the 10-second presence hold does not.");
    row_disabled(alarm_arm_row, csi.active == false || csi.calibrating || strcmp(alarm.state, "off"));
    row_disabled(alarm_disarm_row, !strcmp(alarm.state, "off"));

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
    web_hw_presence_alarm_disarm();
    web_hw_presence_csi_stop();
    web_hw_presence_faro_stop();
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
static void faro_start(lv_event_t *) { web_hw_presence_faro_start(0); presence_refresh(NULL); }
static void faro_stop(lv_event_t *) { web_hw_presence_faro_stop(); presence_refresh(NULL); }
static int dropdown_value(lv_obj_t *obj, const int *values, int fallback)
{
    int selected = obj ? (int)lv_dropdown_get_selected(obj) : fallback;
    return selected >= 0 && selected < 4 ? values[selected] : values[fallback];
}
static void alarm_arm(lv_event_t *)
{
    web_hw_presence_alarm_arm(dropdown_value(alarm_exit, ALARM_EXIT_VALUES, 1),
                              dropdown_value(alarm_duration, ALARM_DURATION_VALUES, 1));
    presence_refresh(NULL);
}
static void alarm_disarm(lv_event_t *) { web_hw_presence_alarm_disarm(); presence_refresh(NULL); }
static void enter_presence(lv_obj_t *parent)
{
    presence_ui = {};
    presence_ui.tool = &web_tool_presence;
    presence_ui.page = ui_create_app_page(parent, "Presence", presence_back);
    lv_obj_t *card = ui_create_card(presence_ui.page, "Router CSI zone");
    csi_state = info_row(card, LV_SYMBOL_EYE_OPEN, "State", "Stopped");
    csi_metrics = info_row(card, LV_SYMBOL_WIFI, "Activity", "0% motion · 0/s");
    csi_network = wrapped_text(card, "No network");
    csi_error = wrapped_text(card, "Calibrate each link in an empty room. Presence is an estimate.", true);
    csi_threshold = row_value(ui_create_card_dropdown(card, NULL, "Sensitivity",
        "High (0.20)\nBalanced (0.35)\nLow (0.55)", 1, NULL));
    csi_start_row = action_row(card, LV_SYMBOL_PLAY, "Start CSI sensing", csi_start);
    csi_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop CSI sensing", csi_stop);
    csi_calibrate_row = action_row(card, LV_SYMBOL_REFRESH, "Recalibrate both zones", csi_calibrate);
    wrapped_text(card, "Keep watch and router fixed. CSI reports radio-path motion, not identity or location.");

    card = ui_create_card(presence_ui.page, "Faro CSI zone");
    csi_faro_state = info_row(card, LV_SYMBOL_WIFI, "Second link", "Waiting");
    csi_faro_metrics = wrapped_text(card, "No Faro discovered");
    wrapped_text(card, "Run Faro transmitter mode on a second watch on this channel. Each link calibrates and detects independently.");

    card = ui_create_card(presence_ui.page, "Motion history · last 60 s");
    csi_history = lv_chart_create(card);
    lv_obj_set_size(csi_history, LV_PCT(100), is_screen_small() ? 130 : 170);
    lv_chart_set_type(csi_history, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(csi_history, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(csi_history, 120);
    lv_chart_set_axis_range(csi_history, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(csi_history, 4, 5);
    lv_obj_set_style_line_width(csi_history, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(csi_history, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(csi_history, LV_OBJ_FLAG_SCROLLABLE);
    csi_router_series = lv_chart_add_series(csi_history, lv_color_hex(0x36D7FF), LV_CHART_AXIS_PRIMARY_Y);
    csi_faro_series = lv_chart_add_series(csi_history, lv_color_hex(0xFF6B35), LV_CHART_AXIS_PRIMARY_Y);
    wrapped_text(card, "Blue: router · Orange: Faro · 500 ms peak buckets. Session only; reboot clears it.");

    card = ui_create_card(presence_ui.page, "Faro transmitter");
    faro_state = info_row(card, LV_SYMBOL_WIFI, "Mode", "Stopped");
    faro_metrics = wrapped_text(card, "Second-watch transmitter");
    faro_error = wrapped_text(card, "100 Hz ESP-NOW beacon. Same channel as receiver; no sensing in this mode.", true);
    faro_start_row = action_row(card, LV_SYMBOL_PLAY, "Start Faro transmitter", faro_start);
    faro_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop Faro transmitter", faro_stop);

    card = ui_create_card(presence_ui.page, "Intrusion alarm");
    alarm_state = info_row(card, LV_SYMBOL_WARNING, "State", "off");
    alarm_metrics = wrapped_text(card, "exit 10 s · siren 30 s");
    alarm_exit = row_value(ui_create_card_dropdown(card, NULL, "Exit delay",
        "5 seconds\n10 seconds\n30 seconds\n60 seconds", 1, NULL));
    alarm_duration = row_value(ui_create_card_dropdown(card, NULL, "Siren duration",
        "10 seconds\n30 seconds\n60 seconds\n120 seconds", 1, NULL));
    alarm_arm_row = action_row(card, LV_SYMBOL_PLAY, "Arm alarm", alarm_arm);
    alarm_disarm_row = action_row(card, LV_SYMBOL_STOP, "Disarm alarm", alarm_disarm);
    alarm_error = wrapped_text(card, "Only live motion triggers; the 10-second presence hold does not.", true);

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
static lv_obj_t *portal_start_row, *portal_stop_row, *portal_scan_row, *portal_karma_row, *portal_karma_info;
static lv_obj_t *portal_twin_bssid, *portal_twin_channel, *portal_twin_psk, *portal_coherent, *portal_verify;
static lv_obj_t *portal_twin_start_row, *portal_clone_row, *portal_clone_info;
static void portal_build(void);
static void portal_refresh(lv_timer_t *)
{
    if (!portal_consented || !portal_state) return;
    drive_native(portal_ui);
    web_hw_evilportal_t st;
    web_hw_evilportal_status(&st);
    bool scanning = web_hw_evilportal_karma_busy();
    bool cloning = web_hw_evilportal_cloning();
    label_set(portal_state, st.active ? (st.mode == EP_MODE_KARMA ? "KARMA AP active" : st.twin_coherent ? "WPA2 twin active" : st.mode == EP_MODE_TWIN ? "Open twin active" : "Standalone active") : cloning ? "Cloning page" : scanning ? "Discovering probes" : "Stopped");
    label_set(portal_clients, "%d", st.clients);
    label_set(portal_attempts, "%lu · PSK %d ok / %d bad", (unsigned long)st.attempts,
              web_hw_evilportal_verified_ok(), web_hw_evilportal_verified_bad());
    label_set(portal_error, st.error ? st.error : "");
    const char *clone_name = web_hw_evilportal_clone_name();
    label_set(portal_clone_info, cloning ? "Joining open lab AP and fetching page..." : clone_name && *clone_name ? "Saved: %s" : "No page cloned yet", clone_name);
    char found[256] = "";
    int count = web_hw_evilportal_karma_count();
    for (int i = 0; i < count && i < 4; i++) {
        char line[72];
        snprintf(line, sizeof(line), "%s%s (%d hits)", i ? "\n" : "",
                 web_hw_evilportal_karma_ssid(i), web_hw_evilportal_karma_hits(i));
        strlcat(found, line, sizeof(found));
    }
    label_set(portal_karma_info, count ? "%s" : scanning ? "Listening..." : "No probe SSIDs discovered", found);
    row_disabled(portal_start_row, st.active || scanning || cloning);
    row_disabled(portal_stop_row, !st.active && !scanning && !cloning);
    row_disabled(portal_scan_row, st.active || scanning || cloning);
    row_disabled(portal_karma_row, st.active || scanning || cloning || count == 0);
    row_disabled(portal_twin_start_row, st.active || scanning || cloning);
    row_disabled(portal_clone_row, st.active || scanning || cloning);
}
static void portal_back(lv_event_t *) { close_native(portal_ui, web_hw_evilportal_stop, true); }
static void portal_start(lv_event_t *)
{
    int ssid_idx = portal_ssid ? lv_dropdown_get_selected(portal_ssid) : 0;
    int template_idx = web_hw_evilportal_template_id(portal_template ? lv_dropdown_get_selected(portal_template) : 0);
    const char *manual = portal_ssid_text ? lv_textarea_get_text(portal_ssid_text) : "";
    const char *ssid = manual && *manual ? manual : web_hw_evilportal_ssid_preset(ssid_idx);
    web_hw_evilportal_start(template_idx, ssid, web_hw_evilportal_gateway());
    portal_refresh(NULL);
}
static void portal_stop_cb(lv_event_t *) { web_hw_evilportal_stop(); portal_refresh(NULL); }
static void portal_verify_cb(lv_event_t *)
{
    web_hw_evilportal_set_verify(portal_verify && lv_obj_has_state(portal_verify, LV_STATE_CHECKED));
}
static void portal_twin_start(lv_event_t *)
{
    int ssid_idx = portal_ssid ? lv_dropdown_get_selected(portal_ssid) : 0;
    int template_idx = web_hw_evilportal_template_id(portal_template ? lv_dropdown_get_selected(portal_template) : 0);
    const char *manual = portal_ssid_text ? lv_textarea_get_text(portal_ssid_text) : "";
    const char *ssid = manual && *manual ? manual : web_hw_evilportal_ssid_preset(ssid_idx);
    const char *mac = portal_twin_bssid ? lv_textarea_get_text(portal_twin_bssid) : "";
    uint8_t bssid[6];
    int used = 0;
    if (sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%n", &bssid[0], &bssid[1], &bssid[2],
               &bssid[3], &bssid[4], &bssid[5], &used) != 6 || used != 17) {
        label_set(portal_error, "Enter the authorized lab AP BSSID"); return;
    }
    int channel = portal_twin_channel ? atoi(lv_textarea_get_text(portal_twin_channel)) : 0;
    bool coherent = portal_coherent && lv_obj_has_state(portal_coherent, LV_STATE_CHECKED);
    const char *psk = portal_twin_psk ? lv_textarea_get_text(portal_twin_psk) : "";
    web_hw_evilportal_start_twin(template_idx, ssid, bssid, channel, coherent ? 3 : 0, coherent, coherent ? psk : NULL);
    portal_refresh(NULL);
}
static void portal_clone_cb(lv_event_t *)
{
    int ssid_idx = portal_ssid ? lv_dropdown_get_selected(portal_ssid) : 0;
    const char *manual = portal_ssid_text ? lv_textarea_get_text(portal_ssid_text) : "";
    const char *ssid = manual && *manual ? manual : web_hw_evilportal_ssid_preset(ssid_idx);
    web_hw_evilportal_clone_page(ssid);
    portal_refresh(NULL);
}
static void portal_scan_cb(lv_event_t *) { web_hw_evilportal_karma_start(10); portal_refresh(NULL); }
static void portal_karma_cb(lv_event_t *)
{
    int template_idx = web_hw_evilportal_template_id(portal_template ? lv_dropdown_get_selected(portal_template) : 0);
    web_hw_evilportal_start_karma(template_idx);
    portal_refresh(NULL);
}
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
    char templates[1024] = "";
    for (int i = 0; i < web_hw_evilportal_template_count(); i++) {
        if (i) strlcat(templates, "\n", sizeof(templates));
        strlcat(templates, web_hw_evilportal_template_name(i), sizeof(templates));
    }
    portal_template = row_value(ui_create_card_dropdown(card, NULL, "Template", templates, 0, NULL));
    portal_start_row = action_row(card, LV_SYMBOL_PLAY, "Start portal", portal_start);
    portal_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop portal / scan / clone", portal_stop_cb);
    card = ui_create_card(portal_ui.page, "Authorized lab twin");
    portal_twin_bssid = text_input_row(card, "Target BSSID", "aa:bb:cc:dd:ee:ff", 17);
    portal_twin_channel = text_input_row(card, "Channel", "1-13", 2);
    lv_textarea_set_accepted_chars(portal_twin_channel, "0123456789");
    portal_coherent = row_value(ui_create_card_switch(card, LV_SYMBOL_WIFI, "WPA2-coherent AP", false, NULL));
    portal_twin_psk = text_input_row(card, "Known lab PSK", "Required for WPA2 twin", 63);
    lv_textarea_set_password_mode(portal_twin_psk, true);
    portal_verify = row_value(ui_create_card_switch(card, LV_SYMBOL_OK, "Verify captured PSKs", web_hw_evilportal_verify_enabled(), portal_verify_cb));
    portal_twin_start_row = action_row(card, LV_SYMBOL_PLAY, "Start selected twin", portal_twin_start);
    wrapped_text(card, "Open twin needs no PSK. Coherent mode uses your known WPA2 lab PSK and never silently downgrades security. Verification associates only with the selected BSSID.");
    card = ui_create_card(portal_ui.page, "Clone safe lab portal");
    portal_clone_row = action_row(card, LV_SYMBOL_DOWNLOAD, "Clone current SSID", portal_clone_cb);
    portal_clone_info = wrapped_text(card, "No page cloned yet");
    wrapped_text(card, "Joins the open lab SSID, fetches its captive page and saves only an already-safe self-contained POST /login page as cloned.html. Active or external content is rejected.");
    card = ui_create_card(portal_ui.page, "Passive probe discovery");
    portal_scan_row = action_row(card, LV_SYMBOL_REFRESH, "Scan probe requests (10 s)", portal_scan_cb);
    portal_karma_row = action_row(card, LV_SYMBOL_PLAY, "Start KARMA AP (most seen)", portal_karma_cb);
    portal_karma_info = wrapped_text(card, "No probe SSIDs discovered", true);
    wrapped_text(card, "KARMA core opens one discovered SSID; it does not yet answer every probe dynamically. Channel hopping may interrupt Wi-Fi. Closing this page stops the AP or scan.");
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
static lv_obj_t *deauth_start_row, *deauth_campaign_row, *deauth_stop_row, *deauth_scan_row;
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
    if (st.active && st.campaign_count) label_set(deauth_state, "Campaign %d/%d", st.campaign_completed, st.campaign_count);
    else label_set(deauth_state, st.active ? "Injecting" : scanning ? "Scanning" : "Stopped");
    label_set(deauth_sent, "%lu ok / %lu failed", (unsigned long)st.sent_count, (unsigned long)st.tx_errors);
    if (st.error) label_set(deauth_error, "%s", st.error);
    else if (!st.active && st.reconnect_attempts) label_set(deauth_error, "Reconnect: %lu ms, %d attempt(s)", (unsigned long)st.reconnect_ms, st.reconnect_attempts);
    else label_set(deauth_error, "WPA3/PMF may ignore deauth");
    label_set(deauth_scan_label, scanning ? "Scanning channels..." : count ? "Select a target" : "Scan for targets");
    row_disabled(deauth_start_row, st.active || scanning || count == 0);
    row_disabled(deauth_campaign_row, st.active || scanning || count == 0);
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
static void deauth_campaign_start(lv_event_t *)
{
    static const int rates[] = {1, 5, 10, 20, 50};
    int rate_idx = deauth_rate ? (int)lv_dropdown_get_selected(deauth_rate) : 2;
    web_hw_deauthflood_campaign_start_all(50, rates[rate_idx]);
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
    deauth_start_row = action_row(card, LV_SYMBOL_PLAY, "Start selected test", deauth_start);
    deauth_campaign_row = action_row(card, LV_SYMBOL_PLAY, "Test first 8 scanned APs", deauth_campaign_start);
    deauth_stop_row = action_row(card, LV_SYMBOL_STOP, "Stop and reconnect", deauth_stop_cb);
    wrapped_text(card, "Broadcast station. Normal Wi-Fi disconnects during the test. Closing this page stops injection.", true);
    wrapped_text(card, "Campaign uses the first 8 scan rows. Inspect and confirm every one is authorized before starting it.", true);
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
static lv_obj_t *beacon_start_row, *beacon_campaign_row, *beacon_stop_row;
static void beacon_build(void);
static void beacon_refresh(lv_timer_t *)
{
    if (!beacon_consented || !beacon_state) return;
    drive_native(beacon_ui);
    web_hw_beaconspam_t st;
    web_hw_beaconspam_status(&st);
    if (st.active && st.campaign_active) label_set(beacon_state, "Campaign %d/%lu", st.campaign_completed, (unsigned long)st.ssid_count);
    else label_set(beacon_state, st.active ? "Injecting" : "Stopped");
    label_set(beacon_sent, "%lu ok / %lu failed · %lu SSIDs", (unsigned long)st.beacon_count,
              (unsigned long)st.tx_errors, (unsigned long)st.ssid_count);
    if (st.error) label_set(beacon_error, "%s", st.error);
    else if (!st.active && st.reconnect_attempts) label_set(beacon_error, "Reconnect: %lu ms, %d attempt(s)", (unsigned long)st.reconnect_ms, st.reconnect_attempts);
    else label_set(beacon_error, "");
    row_disabled(beacon_start_row, st.active);
    row_disabled(beacon_campaign_row, st.active);
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
static void beacon_campaign_start(lv_event_t *)
{
    static const char *prefixes[] = {"FreeNet", "LabTest", "BeaconTest"};
    static const int counts[] = {20, 50, 64};
    int channel = beacon_channel ? (int)lv_dropdown_get_selected(beacon_channel) : 0;
    const char *manual = beacon_prefix_text ? lv_textarea_get_text(beacon_prefix_text) : "";
    const char *prefix = manual && *manual ? manual : prefixes[lv_dropdown_get_selected(beacon_prefix)];
    int selected = beacon_count ? (int)lv_dropdown_get_selected(beacon_count) : 0;
    web_hw_beaconspam_campaign_start(channel, counts[selected], prefix, 10);
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
    beacon_start_row = action_row(card, LV_SYMBOL_PLAY, "Start continuous test", beacon_start);
    beacon_campaign_row = action_row(card, LV_SYMBOL_PLAY, "Run finite SSID campaign", beacon_campaign_start);
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
