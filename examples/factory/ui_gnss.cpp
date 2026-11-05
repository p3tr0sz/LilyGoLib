/**
 * @file      ui_gnss.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-05-28
 * GNSS signal quality view.
 */
#include "ui_define.h"

#ifndef EXCLUDE_GPS

#define GNSS_UI_CN0_MAX      60
#define GNSS_UI_MAX_CN0_BARS 32
#define GNSS_UI_VISIBLE_CONSTELLATIONS 4
#define GNSS_UI_PPS_DEBOUNCE_MS 500
#define GNSS_UI_PPS_LED_MS      300
#define GNSS_UI_FIX_BEEP_HZ     1000
#define GNSS_UI_FIX_BEEP_MS     250
#define GNSS_UI_FIX_BEEP_INTERVAL_MS 5000
#define GNSS_UI_NMEA_SERIAL_BAUD 115200

typedef struct {
    lv_obj_t *column;
    lv_obj_t *value;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *label;
} gnss_cn0_bar_t;

typedef struct {
    lv_obj_t *model;
    lv_obj_t *antenna_row;
    lv_obj_t *antenna;
    lv_obj_t *pps;
    lv_obj_t *visible;
    lv_obj_t *tracking;
    lv_obj_t *used;
    lv_obj_t *ttff;
    lv_obj_t *rx_size;
    lv_obj_t *lat;
    lv_obj_t *lng;
    lv_obj_t *datetime;
    lv_obj_t *speed;
    lv_obj_t *altitude;
    lv_obj_t *cn0_chart;
    gnss_cn0_bar_t cn0_bars[GNSS_UI_MAX_CN0_BARS];
#if FACTORY_WATCH_UI
    lv_obj_t *headline;     /* "No fix · 4 satellites" or the position */
    lv_obj_t *systems;      /* "GPS 8 (3 used) · Galileo 5" */
    lv_obj_t *pos_card;
    lv_obj_t *lat_row;
    lv_obj_t *lng_row;
    lv_obj_t *date_row;
    lv_obj_t *date;
    lv_obj_t *time_row;
    lv_obj_t *time;
    lv_obj_t *speed_row;
    lv_obj_t *alt_row;
#endif
} gnss_label_t;

static lv_obj_t *page_container = NULL;
static lv_timer_t *timer = NULL;
static lv_timer_t *detail_build_timer = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *cn0_card = NULL;
static gnss_label_t gnss_labels;
static bool nmea_to_serial = false;
static bool fix_beep_enabled = true;
static bool detail_ui_ready = false;
static uint16_t detail_cn0_bar_index = 0;
static uint32_t last_pps_event_time = 0;
static uint32_t last_fix_beep_time = 0;
static lv_obj_t *gnss_unavailable_msgbox = NULL;
static lv_obj_t *nmea_serial_overlay = NULL;
static lv_obj_t *nmea_serial_switch = NULL;
static lv_group_t *nmea_serial_prev_group = NULL;
static lv_obj_t *nmea_serial_prev_focus = NULL;

static void nmea_switch_event(lv_event_t *e);
static void fix_beep_switch_event(lv_event_t *e);
#if FACTORY_WATCH_UI
static gps_params_t gnss_param;     /* Last reading, for pages built between updates */
#endif

static const gps_satellite_system_t gnss_systems[GPS_SIGNAL_MAX_CONSTELLATIONS] = {
    GPS_SAT_SYSTEM_GPS,
    GPS_SAT_SYSTEM_GLONASS,
    GPS_SAT_SYSTEM_BEIDOU,
    GPS_SAT_SYSTEM_GALILEO,
    GPS_SAT_SYSTEM_QZSS
};

#if FACTORY_WATCH_UI
static const char *system_name(gps_satellite_system_t system)
{
    switch (system) {
    case GPS_SAT_SYSTEM_GPS:
        return "GPS";
    case GPS_SAT_SYSTEM_GLONASS:
        return "GLONASS";
    case GPS_SAT_SYSTEM_BEIDOU:
        return "BeiDou";
    case GPS_SAT_SYSTEM_GALILEO:
        return "Galileo";
    case GPS_SAT_SYSTEM_QZSS:
        return "QZSS";
    default:
        return "Other";
    }
}
#else
static const char *system_short_name(gps_satellite_system_t system)
{
    switch (system) {
    case GPS_SAT_SYSTEM_GPS:
        return "GPS";
    case GPS_SAT_SYSTEM_GLONASS:
        return "GLO";
    case GPS_SAT_SYSTEM_BEIDOU:
        return "BDS";
    case GPS_SAT_SYSTEM_GALILEO:
        return "GAL";
    case GPS_SAT_SYSTEM_QZSS:
        return "QZS";
    default:
        return "UNK";
    }
}
#endif

static const char *antenna_state_name(gps_antenna_state_t state)
{
    switch (state) {
    case GPS_ANTENNA_OK:
        return "OK";
    case GPS_ANTENNA_OPEN:
        return "Open";
    case GPS_ANTENNA_SHORT:
        return "Short";
    default:
        return "Unknown";
    }
}

static bool is_l76k_model(const char *model)
{
    return model && strcmp(model, "L76K") == 0;
}

static bool is_system_visible(gps_satellite_system_t system)
{
    return system != GPS_SAT_SYSTEM_QZSS;
}

#if !FACTORY_WATCH_UI   /* The CN0 chart: on the watch, counts in words instead */
static lv_color_t system_color(gps_satellite_system_t system)
{
    switch (system) {
    case GPS_SAT_SYSTEM_GPS:
        return lv_color_hex(0x00D4AA);
    case GPS_SAT_SYSTEM_GLONASS:
        return lv_color_hex(0x0088FF);
    case GPS_SAT_SYSTEM_BEIDOU:
        return lv_color_hex(0xFFB800);
    case GPS_SAT_SYSTEM_GALILEO:
        return lv_color_hex(0xAA55FF);
    case GPS_SAT_SYSTEM_QZSS:
        return lv_color_hex(0xFF3366);
    default:
        return UI_COLOR_TEXT_SECONDARY;
    }
}

static lv_opa_t cn0_opa(const gps_signal_satellite_t &sat)
{
    if (!sat.has_cn0) {
        return LV_OPA_50;
    }
    if (sat.cn0 < 20) {
        return LV_OPA_60;
    }
    if (sat.cn0 < 35) {
        return LV_OPA_80;
    }
    return LV_OPA_COVER;
}

static void cn0_chart_encoder_event_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_FOCUSED:
        lv_obj_scroll_to_view_recursive(chart, LV_ANIM_ON);
        break;
    case LV_EVENT_KEY: {
        uint32_t key = lv_event_get_key(e);
        lv_coord_t step = is_screen_small() ? 28 : 36;
        if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) {
            lv_obj_scroll_by_bounded(chart, -step, 0, LV_ANIM_ON);
            lv_event_stop_bubbling(e);
        } else if (key == LV_KEY_LEFT || key == LV_KEY_UP) {
            lv_obj_scroll_by_bounded(chart, step, 0, LV_ANIM_ON);
            lv_event_stop_bubbling(e);
        } else if (key == LV_KEY_ESC) {
            lv_group_t *group = (lv_group_t *)lv_obj_get_group(chart);
            if (group) {
                lv_group_set_editing(group, false);
            }
        }
        break;
    }
    case LV_EVENT_DEFOCUSED: {
        lv_group_t *group = (lv_group_t *)lv_obj_get_group(chart);
        if (group) {
            lv_group_set_editing(group, false);
        }
        break;
    }
    default:
        break;
    }
}

static lv_color_t satellite_bar_color(const gps_signal_satellite_t &sat)
{
    lv_color_t base = system_color(sat.system);
    if (sat.used) {
        return lv_color_mix(lv_color_hex(0xFFD400), base, LV_OPA_50);
    }
    return base;
}
#endif

static lv_obj_t *add_info_row(lv_obj_t *card, const char *title, const char *value, lv_obj_t **out_label)
{
    lv_obj_t *row = ui_create_card_info(card, LV_SYMBOL_GPS, title, value);
    *out_label = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    return row;
}

#if defined(GPS_PPS) && !FACTORY_WATCH_UI
static void pps_led_off_cb(lv_timer_t *t)
{
    lv_obj_t *led = (lv_obj_t *)lv_timer_get_user_data(t);
    if (led && lv_obj_is_valid(led)) {
        lv_led_off(led);
    }
    lv_timer_del(t);
}

static void create_pps_row(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_GPS);
    lv_obj_add_style(icon, &ui_styles.accent_text, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "PPS");
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, 0);
    if (is_screen_small()) {
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    }
    lv_obj_set_flex_grow(label, 1);

    lv_obj_t *led = lv_led_create(row);
    lv_led_set_color(led, lv_color_make(0, 255, 0));
    lv_led_off(led);
    lv_obj_set_size(led, 16, 16);
    gnss_labels.pps = led;
}
#endif

#if !FACTORY_WATCH_UI
static void create_cn0_legend(lv_obj_t *card)
{
    lv_obj_t *legend = lv_obj_create(card);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend, 0, 0);
    lv_obj_set_style_radius(legend, 0, 0);
    lv_obj_set_style_pad_all(legend, 0, 0);
    lv_obj_set_style_pad_column(legend, 10, 0);
    lv_obj_set_style_pad_row(legend, 4, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (uint8_t i = 0; i < GNSS_UI_VISIBLE_CONSTELLATIONS; ++i) {
        lv_obj_t *item = lv_label_create(legend);
        lv_label_set_text(item, system_short_name(gnss_systems[i]));
        lv_obj_set_style_text_color(item, system_color(gnss_systems[i]), 0);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_12, 0);
    }
}

static void create_cn0_bar(lv_obj_t *parent, gnss_cn0_bar_t *bar)
{
    const lv_coord_t col_w = is_screen_small() ? 18 : 22;
    const lv_coord_t track_w = is_screen_small() ? 10 : 12;

    bar->column = lv_obj_create(parent);
    lv_obj_set_size(bar->column, col_w, 116);
    lv_obj_set_style_bg_opa(bar->column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar->column, 0, 0);
    lv_obj_set_style_radius(bar->column, 0, 0);
    lv_obj_set_style_pad_all(bar->column, 0, 0);
    lv_obj_remove_flag(bar->column, LV_OBJ_FLAG_SCROLLABLE);

    bar->value = lv_label_create(bar->column);
    lv_label_set_text(bar->value, "--");
    lv_obj_set_width(bar->value, col_w);
    lv_obj_set_style_text_align(bar->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(bar->value, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(bar->value, &lv_font_montserrat_12, 0);
    lv_obj_align(bar->value, LV_ALIGN_TOP_MID, 0, 0);

    bar->track = lv_obj_create(bar->column);
    lv_obj_set_size(bar->track, track_w, 74);
    lv_obj_set_style_bg_color(bar->track, UI_COLOR_TRACK, 0);
    lv_obj_set_style_bg_opa(bar->track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->track, 0, 0);
    lv_obj_set_style_radius(bar->track, 0, 0);
    lv_obj_set_style_pad_all(bar->track, 0, 0);
    lv_obj_remove_flag(bar->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar->track, LV_ALIGN_TOP_MID, 0, 18);

    bar->fill = lv_obj_create(bar->track);
    lv_obj_set_width(bar->fill, LV_PCT(100));
    lv_obj_set_height(bar->fill, 0);
    lv_obj_set_style_bg_color(bar->fill, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar->fill, 0, 0);
    lv_obj_set_style_radius(bar->fill, 0, 0);
    lv_obj_set_style_pad_all(bar->fill, 0, 0);
    lv_obj_remove_flag(bar->fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bar->fill, LV_ALIGN_BOTTOM_MID, 0, 0);

    bar->label = lv_label_create(bar->column);
    lv_label_set_text(bar->label, "--");
    lv_obj_set_width(bar->label, col_w);
    lv_obj_set_style_text_align(bar->label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(bar->label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(bar->label, &lv_font_montserrat_12, 0);
    lv_obj_align(bar->label, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_flag(bar->column, LV_OBJ_FLAG_HIDDEN);
}

static void create_cn0_chart(lv_obj_t *card)
{
    gnss_labels.cn0_chart = lv_obj_create(card);
    lv_obj_set_size(gnss_labels.cn0_chart, LV_PCT(100), 126);
    lv_obj_set_style_bg_opa(gnss_labels.cn0_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gnss_labels.cn0_chart, 1, 0);
    lv_obj_set_style_border_color(gnss_labels.cn0_chart, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_radius(gnss_labels.cn0_chart, 8, 0);
    lv_obj_set_style_pad_top(gnss_labels.cn0_chart, 4, 0);
    lv_obj_set_style_pad_bottom(gnss_labels.cn0_chart, 4, 0);
    lv_obj_set_style_pad_left(gnss_labels.cn0_chart, 6, 0);
    lv_obj_set_style_pad_right(gnss_labels.cn0_chart, 6, 0);
    lv_obj_set_style_pad_column(gnss_labels.cn0_chart, 5, 0);
    lv_obj_set_flex_flow(gnss_labels.cn0_chart, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gnss_labels.cn0_chart, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(gnss_labels.cn0_chart, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(gnss_labels.cn0_chart, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(gnss_labels.cn0_chart, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(gnss_labels.cn0_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gnss_labels.cn0_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gnss_labels.cn0_chart, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_border_color(gnss_labels.cn0_chart, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(gnss_labels.cn0_chart, LV_OPA_80, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(gnss_labels.cn0_chart, cn0_chart_encoder_event_cb, LV_EVENT_ALL, NULL);

    lv_group_t *group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, gnss_labels.cn0_chart);
    }

}

static void update_cn0_chart(const gps_params_t &param)
{
    uint8_t bar_index = 0;
    for (uint16_t i = 0; i < param.signal_satellite_count && bar_index < GNSS_UI_MAX_CN0_BARS; ++i) {
        const gps_signal_satellite_t &sat = param.signal_satellites[i];
        if (!is_system_visible(sat.system)) {
            continue;
        }
        if (!sat.has_cn0 || sat.cn0 <= 0) {
            continue;
        }

        gnss_cn0_bar_t *bar = &gnss_labels.cn0_bars[bar_index];
        int cn0 = sat.cn0;
        if (cn0 > GNSS_UI_CN0_MAX) {
            cn0 = GNSS_UI_CN0_MAX;
        }

        lv_coord_t track_h = lv_obj_get_height(bar->track);
        lv_coord_t fill_h = (track_h * cn0) / GNSS_UI_CN0_MAX;
        if (fill_h < 2) {
            fill_h = 2;
        }

        lv_color_t color = satellite_bar_color(sat);
        lv_label_set_text_fmt(bar->label, "%u", sat.prn);
        lv_label_set_text_fmt(bar->value, "%d", sat.cn0);
        lv_obj_set_style_text_color(bar->label, system_color(sat.system), 0);
        lv_obj_set_style_text_color(bar->value, UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_bg_color(bar->fill, color, 0);
        lv_obj_set_style_bg_opa(bar->fill, cn0_opa(sat), 0);
        lv_obj_set_style_border_width(bar->track, sat.used ? 1 : 0, 0);
        lv_obj_set_style_border_color(bar->track, sat.used ? lv_color_hex(0xFFD400) : UI_COLOR_DIVIDER, 0);
        lv_obj_set_height(bar->fill, fill_h);
        lv_obj_align(bar->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_remove_flag(bar->column, LV_OBJ_FLAG_HIDDEN);
        bar_index++;
    }

    for (uint8_t i = bar_index; i < GNSS_UI_MAX_CN0_BARS; ++i) {
        lv_obj_add_flag(gnss_labels.cn0_bars[i].column, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

#if FACTORY_WATCH_UI
/* Changes a card row only when it has to; dividers follow (ui_card_row_set_hidden). */
static void set_row_hidden(lv_obj_t *row, bool hidden)
{
    if (!row || !lv_obj_is_valid(row)) return;
    if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    ui_card_row_set_hidden(row, hidden);
}

static void set_label(lv_obj_t *label, const char *text)
{
    if (label && lv_obj_is_valid(label)) lv_label_set_text(label, text);
}

static void update_position_labels(const gps_params_t &param)
{
    char buffer[64];

    /* No rows of "--": each shows once it has a value. */
    if (param.location_valid) {
        lv_snprintf(buffer, sizeof(buffer), "%.06f", param.lat);
        set_label(gnss_labels.lat, buffer);
        lv_snprintf(buffer, sizeof(buffer), "%.06f", param.lng);
        set_label(gnss_labels.lng, buffer);
    }
    set_row_hidden(gnss_labels.lat_row, !param.location_valid);
    set_row_hidden(gnss_labels.lng_row, !param.location_valid);

    if (param.datetime_valid) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%d", &param.datetime);
        set_label(gnss_labels.date, buffer);
        strftime(buffer, sizeof(buffer), "%H:%M:%S", &param.datetime);
        set_label(gnss_labels.time, buffer);
    }
    set_row_hidden(gnss_labels.date_row, !param.datetime_valid);
    set_row_hidden(gnss_labels.time_row, !param.datetime_valid);

    if (param.speed_valid) {
        lv_snprintf(buffer, sizeof(buffer), "%.02f km/h", param.speed);
        set_label(gnss_labels.speed, buffer);
    }
    set_row_hidden(gnss_labels.speed_row, !param.speed_valid);

    if (param.altitude_valid) {
        lv_snprintf(buffer, sizeof(buffer), "%.01f m", param.altitude);
        set_label(gnss_labels.altitude, buffer);
    }
    set_row_hidden(gnss_labels.alt_row, !param.altitude_valid);

    bool any = param.location_valid || param.datetime_valid || param.speed_valid || param.altitude_valid;
    if (gnss_labels.pos_card && lv_obj_is_valid(gnss_labels.pos_card)) {
        if (any) lv_obj_remove_flag(gnss_labels.pos_card, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(gnss_labels.pos_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The Details page: Model, first fix time and bytes received. */
static void render_gnss_details(const gps_params_t &param)
{
    set_label(gnss_labels.model, param.model[0] ? param.model : "N.A");
    if (gnss_labels.ttff && lv_obj_is_valid(gnss_labels.ttff)) {
        if (param.ttff_valid) {
            lv_label_set_text_fmt(gnss_labels.ttff, "%lu.%01lu s",
                                  (unsigned long)(param.ttff_ms / 1000),
                                  (unsigned long)((param.ttff_ms % 1000) / 100));
        } else {
            lv_label_set_text(gnss_labels.ttff, "--");
        }
    }
    if (gnss_labels.rx_size && lv_obj_is_valid(gnss_labels.rx_size)) {
        lv_label_set_text_fmt(gnss_labels.rx_size, "%u", param.rx_size);
    }
}

static void render_gnss_view(const gps_params_t &param)
{
    char buffer[96];

    if (gnss_labels.antenna_row) {
        bool show_antenna = is_l76k_model(param.model);
        if (show_antenna) {
            set_label(gnss_labels.antenna, antenna_state_name(param.antenna_state));
        }
        set_row_hidden(gnss_labels.antenna_row, !show_antenna);
    }
    update_position_labels(param);

    uint16_t visible = 0;
    uint16_t tracking = 0;
    uint16_t used = 0;
    for (uint16_t i = 0; i < param.signal_satellite_count; ++i) {
        if (is_system_visible(param.signal_satellites[i].system)) {
            visible++;
        }
    }

    /* Satellites per system in words: "GPS 8 (3 used) · Galileo 5". */
    size_t len = 0;
    buffer[0] = '\0';
    for (uint16_t i = 0; i < param.constellation_count; ++i) {
        const gps_signal_constellation_t &c = param.constellations[i];
        if (!is_system_visible(c.system)) {
            continue;
        }
        tracking += c.tracking;
        used += c.used;
        uint16_t count = c.visible > c.tracking ? c.visible : c.tracking;
        if (count == 0 || len >= sizeof(buffer) - 1) {
            continue;
        }
        int n;
        if (c.used) {
            n = lv_snprintf(buffer + len, sizeof(buffer) - len, "%s%s %u (%u used)",
                            len ? " \xC2\xB7 " : "", system_name(c.system), count, c.used);
        } else {
            n = lv_snprintf(buffer + len, sizeof(buffer) - len, "%s%s %u",
                            len ? " \xC2\xB7 " : "", system_name(c.system), count);
        }
        if (n > 0) {
            len += (size_t)n;
            if (len > sizeof(buffer) - 1) len = sizeof(buffer) - 1;
        }
    }
    set_label(gnss_labels.systems, len ? buffer : "Looking for satellites");

    if (gnss_labels.visible) lv_label_set_text_fmt(gnss_labels.visible, "%u", visible);
    if (gnss_labels.tracking) lv_label_set_text_fmt(gnss_labels.tracking, "%u", tracking);
    if (gnss_labels.used) lv_label_set_text_fmt(gnss_labels.used, "%u", used);

    /* The fix first, in words. */
    if (param.location_valid) {
        lv_snprintf(buffer, sizeof(buffer), "%.5f, %.5f", param.lat, param.lng);
    } else {
        lv_snprintf(buffer, sizeof(buffer), "No fix \xC2\xB7 %u satellite%s",
                    visible, visible == 1 ? "" : "s");
    }
    set_label(gnss_labels.headline, buffer);

#ifdef GPS_PPS
    /* Time pulse: a pulse arrives every second while the receiver has time. */
    uint32_t now = lv_tick_get();
    bool pulsing = last_pps_event_time != 0 && now - last_pps_event_time < 2500;
    set_label(gnss_labels.pps, pulsing ? "Yes" : "No");
#endif

    render_gnss_details(param);
}
#else
static void update_position_labels(const gps_params_t &param)
{
    char buffer[64];

    if (param.location_valid) {
        lv_label_set_text_fmt(gnss_labels.lat, "%.06f", param.lat);
        lv_label_set_text_fmt(gnss_labels.lng, "%.06f", param.lng);
    } else {
        lv_label_set_text(gnss_labels.lat, "--");
        lv_label_set_text(gnss_labels.lng, "--");
    }

    if (param.datetime_valid) {
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &param.datetime);
        lv_label_set_text(gnss_labels.datetime, buffer);
    } else {
        lv_label_set_text(gnss_labels.datetime, "--");
    }

    if (param.speed_valid) {
        lv_label_set_text_fmt(gnss_labels.speed, "%.02f km/h", param.speed);
    } else {
        lv_label_set_text(gnss_labels.speed, "--");
    }

    if (param.altitude_valid) {
        lv_label_set_text_fmt(gnss_labels.altitude, "%.01f m", param.altitude);
    } else {
        lv_label_set_text(gnss_labels.altitude, "--");
    }
}

static void render_gnss_view(const gps_params_t &param)
{
    lv_label_set_text(gnss_labels.model, param.model);
    bool show_antenna = is_l76k_model(param.model);
    if (gnss_labels.antenna_row && lv_obj_is_valid(gnss_labels.antenna_row)) {
        if (show_antenna) {
            lv_obj_clear_flag(gnss_labels.antenna_row, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(gnss_labels.antenna, antenna_state_name(param.antenna_state));
        } else {
            lv_obj_add_flag(gnss_labels.antenna_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    update_position_labels(param);

    uint16_t visible = 0;
    uint16_t tracking = 0;
    uint16_t used = 0;
    for (uint16_t i = 0; i < param.signal_satellite_count; ++i) {
        if (is_system_visible(param.signal_satellites[i].system)) {
            visible++;
        }
    }
    for (uint16_t i = 0; i < param.constellation_count; ++i) {
        if (!is_system_visible(param.constellations[i].system)) {
            continue;
        }
        tracking += param.constellations[i].tracking;
        used += param.constellations[i].used;
    }
    lv_label_set_text_fmt(gnss_labels.visible, "%u", visible);
    lv_label_set_text_fmt(gnss_labels.tracking, "%u", tracking);
    lv_label_set_text_fmt(gnss_labels.used, "%u", used);
    if (param.ttff_valid) {
        lv_label_set_text_fmt(gnss_labels.ttff, "%lu.%01lu s",
                              (unsigned long)(param.ttff_ms / 1000),
                              (unsigned long)((param.ttff_ms % 1000) / 100));
    } else {
        lv_label_set_text(gnss_labels.ttff, "--");
    }
    lv_label_set_text_fmt(gnss_labels.rx_size, "%u", param.rx_size);

    if (detail_ui_ready) {
        update_cn0_chart(param);
    }
}
#endif

static void handle_pps_event(const gps_params_t &param)
{
#ifdef GPS_PPS
    if (!param.pps) {
        return;
    }

    uint32_t now = lv_tick_get();
    if (last_pps_event_time != 0 && now - last_pps_event_time < GNSS_UI_PPS_DEBOUNCE_MS) {
        return;
    }
    last_pps_event_time = now;

#if !FACTORY_WATCH_UI   /* The watch says "Time pulse: Yes" instead of blinking */
    if (gnss_labels.pps && lv_obj_is_valid(gnss_labels.pps)) {
        lv_led_on(gnss_labels.pps);
        lv_timer_create(pps_led_off_cb, GNSS_UI_PPS_LED_MS, gnss_labels.pps);
    }
#endif

    if (!param.location_valid) {
        last_fix_beep_time = 0;
        return;
    }

    if (fix_beep_enabled &&
            (last_fix_beep_time == 0 || now - last_fix_beep_time >= GNSS_UI_FIX_BEEP_INTERVAL_MS)) {
        hw_audio_beep(GNSS_UI_FIX_BEEP_HZ, GNSS_UI_FIX_BEEP_MS);
        last_fix_beep_time = now;
    }
#else
    (void)param;
#endif
}

static void update_gnss_view(void)
{
#if FACTORY_WATCH_UI
    gps_params_t &param = gnss_param;
#else
    static gps_params_t param;
#endif
    static uint32_t screen_update_time = 0;

    param.nmea_to_serial = nmea_to_serial;
    hw_get_gps_info(param);
    handle_pps_event(param);

    if (nmea_to_serial) {
        if (gnss_labels.rx_size && lv_obj_is_valid(gnss_labels.rx_size)) {
            lv_label_set_text_fmt(gnss_labels.rx_size, "%u", param.rx_size);
        }
        if (lv_tick_get() < screen_update_time) {
            return;
        }
        screen_update_time = lv_tick_get() + 1000;
    } else {
        screen_update_time = 0;
    }

    render_gnss_view(param);
}

static void timer_cb(lv_timer_t *t)
{
    update_gnss_view();
}

#if FACTORY_WATCH_UI
static void details_page_delete_cb(lv_event_t *e)
{
    (void)e;
    /* The labels go with the page; the updaters skip NULL. */
    gnss_labels.model = NULL;
    gnss_labels.ttff = NULL;
    gnss_labels.rx_size = NULL;
    nmea_serial_switch = NULL;
}

static void open_details_cb(lv_event_t *e)
{
    lv_obj_t *content = ui_push_page(lv_event_get_target_obj(e), "Details");
    lv_obj_add_event_cb(content, details_page_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *card = ui_create_card(content, NULL);
    add_info_row(card, "Model", "N.A", &gnss_labels.model);
    add_info_row(card, "First fix", "--", &gnss_labels.ttff);
    add_info_row(card, "Data received", "0", &gnss_labels.rx_size);
    lv_obj_t *row = ui_create_card_switch(card, LV_SYMBOL_USB, "NMEA to serial",
                                          nmea_to_serial, nmea_switch_event);
    nmea_serial_switch = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
#ifdef GPS_PPS
    ui_create_card_switch(card, LV_SYMBOL_AUDIO, "Beep on fix", fix_beep_enabled, fix_beep_switch_event);
#endif
    render_gnss_details(gnss_param);
}

static void create_gnss_summary(void)
{
    /* The fix first, in body type. */
    lv_obj_t *card = ui_create_card(page_container, NULL);
    gnss_labels.headline = lv_label_create(card);
    lv_label_set_text(gnss_labels.headline, "No fix");
    lv_label_set_long_mode(gnss_labels.headline, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(gnss_labels.headline, LV_PCT(100));
    lv_obj_set_style_text_font(gnss_labels.headline, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(gnss_labels.headline, UI_COLOR_TEXT_PRIMARY, 0);

    /* Position rows show once they have a value; the card hides while none has. */
    card = ui_create_card(page_container, "Position");
    gnss_labels.pos_card = card;
    gnss_labels.lat_row = add_info_row(card, "Latitude", "--", &gnss_labels.lat);
    gnss_labels.lng_row = add_info_row(card, "Longitude", "--", &gnss_labels.lng);
    gnss_labels.date_row = add_info_row(card, "Date", "--", &gnss_labels.date);
    gnss_labels.time_row = add_info_row(card, "Time", "--", &gnss_labels.time);
    gnss_labels.speed_row = add_info_row(card, "Speed", "--", &gnss_labels.speed);
    gnss_labels.alt_row = add_info_row(card, "Altitude", "--", &gnss_labels.altitude);
    ui_card_row_set_hidden(gnss_labels.lat_row, true);
    ui_card_row_set_hidden(gnss_labels.lng_row, true);
    ui_card_row_set_hidden(gnss_labels.date_row, true);
    ui_card_row_set_hidden(gnss_labels.time_row, true);
    ui_card_row_set_hidden(gnss_labels.speed_row, true);
    ui_card_row_set_hidden(gnss_labels.alt_row, true);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

    /* Satellites in words instead of the sideways CN0 chart. */
    card = ui_create_card(page_container, "Satellites");
    gnss_labels.systems = lv_label_create(card);
    lv_label_set_text(gnss_labels.systems, "Looking for satellites");
    lv_label_set_long_mode(gnss_labels.systems, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(gnss_labels.systems, LV_PCT(100));
    lv_obj_set_style_text_font(gnss_labels.systems, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(gnss_labels.systems, UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_pad_bottom(gnss_labels.systems, 8, 0);
    add_info_row(card, "In view", "0", &gnss_labels.visible);
    add_info_row(card, "Tracking", "0", &gnss_labels.tracking);
    add_info_row(card, "Used", "0", &gnss_labels.used);
#ifdef GPS_PPS
    add_info_row(card, "Time pulse", "No", &gnss_labels.pps);
#endif
    gnss_labels.antenna_row = add_info_row(card, "Antenna", "Unknown", &gnss_labels.antenna);
    ui_card_row_set_hidden(gnss_labels.antenna_row, true);

    ui_create_menu_row(page_container, LV_SYMBOL_LIST, "Details", open_details_cb, NULL);
}
#else
static void create_gnss_summary(void)
{
    lv_obj_t *card = ui_create_card(page_container, "Summary");
    add_info_row(card, "Model", "N.A", &gnss_labels.model);
    gnss_labels.antenna_row = add_info_row(card, "Antenna", "Unknown", &gnss_labels.antenna);
    lv_obj_add_flag(gnss_labels.antenna_row, LV_OBJ_FLAG_HIDDEN);
#ifdef GPS_PPS
    create_pps_row(card);
#endif
    add_info_row(card, "Visible", "0", &gnss_labels.visible);
    add_info_row(card, "Tracking", "0", &gnss_labels.tracking);
    add_info_row(card, "Used", "0", &gnss_labels.used);
    add_info_row(card, "TTFF", "--", &gnss_labels.ttff);
    add_info_row(card, "RX Char", "0", &gnss_labels.rx_size);
    ui_create_card_switch(card, LV_SYMBOL_GPS, "NMEA Serial", nmea_to_serial, nmea_switch_event);
#ifdef GPS_PPS
    ui_create_card_switch(card, LV_SYMBOL_AUDIO, "Fix Beep", fix_beep_enabled, fix_beep_switch_event);
#endif

    card = ui_create_card(page_container, "Position");
    add_info_row(card, "Latitude", "--", &gnss_labels.lat);
    add_info_row(card, "Longitude", "--", &gnss_labels.lng);
    add_info_row(card, "Time", "--", &gnss_labels.datetime);
    add_info_row(card, "Speed", "--", &gnss_labels.speed);
    add_info_row(card, "Altitude", "--", &gnss_labels.altitude);
}
#endif

/* Build page content in small batches after the page is visible. */
static void detail_build_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!page_container) {
        if (detail_build_timer) {
            lv_timer_del(detail_build_timer);
            detail_build_timer = NULL;
        }
        return;
    }

#if FACTORY_WATCH_UI
    /* One page, no chart: done after the summary. */
    create_gnss_summary();
    detail_ui_ready = true;
    lv_timer_del(detail_build_timer);
    detail_build_timer = NULL;
    update_gnss_view();
    return;
#else
    if (!gnss_labels.model) {
        create_gnss_summary();
        update_gnss_view();
        return;
    }

    if (!gnss_labels.cn0_chart) {
        cn0_card = ui_create_card(page_container, "CN0");
        create_cn0_chart(cn0_card);
        detail_cn0_bar_index = 0;
        return;
    }

    if (detail_cn0_bar_index < GNSS_UI_MAX_CN0_BARS) {
        const uint16_t bars_per_tick = 8;
        uint16_t end = detail_cn0_bar_index + bars_per_tick;
        if (end > GNSS_UI_MAX_CN0_BARS) {
            end = GNSS_UI_MAX_CN0_BARS;
        }
        for (; detail_cn0_bar_index < end; ++detail_cn0_bar_index) {
            create_cn0_bar(gnss_labels.cn0_chart, &gnss_labels.cn0_bars[detail_cn0_bar_index]);
        }
        if (detail_cn0_bar_index < GNSS_UI_MAX_CN0_BARS) {
            return;
        }
        create_cn0_legend(cn0_card);
    }
    detail_ui_ready = true;
    lv_timer_del(detail_build_timer);
    detail_build_timer = NULL;
    update_gnss_view();
#endif
}

static void close_nmea_serial_mode(void)
{
    nmea_to_serial = false;
    hw_set_gps_nmea_serial(false);

    if (nmea_serial_switch && lv_obj_is_valid(nmea_serial_switch)) {
        lv_obj_clear_state(nmea_serial_switch, LV_STATE_CHECKED);
    }

    if (nmea_serial_prev_group) {
        lv_group_focus_freeze(nmea_serial_prev_group, false);
    }

    if (nmea_serial_overlay && lv_obj_is_valid(nmea_serial_overlay)) {
        lv_obj_delete(nmea_serial_overlay);
    }
    nmea_serial_overlay = NULL;

    if (nmea_serial_prev_focus && lv_obj_is_valid(nmea_serial_prev_focus)) {
        lv_group_focus_obj(nmea_serial_prev_focus);
    }
    nmea_serial_prev_group = NULL;
    nmea_serial_prev_focus = NULL;

    if (timer) {
        lv_timer_set_period(timer, 1000);
    }
}

static void nmea_serial_exit_event(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) {
        lv_indev_wait_release(indev);
    }
    hw_feedback();
    close_nmea_serial_mode();
}

static void show_nmea_serial_mode(void)
{
    if (nmea_serial_overlay) {
        return;
    }

    nmea_serial_prev_group = lv_group_get_default();
    nmea_serial_prev_focus = nmea_serial_prev_group ? lv_group_get_focused(nmea_serial_prev_group) : NULL;

    nmea_serial_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(nmea_serial_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(nmea_serial_overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(nmea_serial_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nmea_serial_overlay, 0, 0);
    lv_obj_set_style_radius(nmea_serial_overlay, 0, 0);
    lv_obj_set_style_pad_all(nmea_serial_overlay, is_screen_small() ? 16 : 24, 0);
    lv_obj_set_style_pad_row(nmea_serial_overlay, is_screen_small() ? 14 : 20, 0);
    lv_obj_remove_flag(nmea_serial_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(nmea_serial_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_group_remove_obj(nmea_serial_overlay);
    lv_obj_set_flex_flow(nmea_serial_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(nmea_serial_overlay,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(nmea_serial_overlay);
    lv_label_set_text(icon, LV_SYMBOL_GPS);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);

    lv_obj_t *title = lv_label_create(nmea_serial_overlay);
    lv_label_set_text(title, "NMEA Serial");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_PRIMARY, 0);
#if FACTORY_WATCH_UI
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
#else
    lv_obj_set_style_text_font(title, is_screen_small() ? &lv_font_montserrat_16 : &lv_font_montserrat_20, 0);
#endif

    lv_obj_t *message = lv_label_create(nmea_serial_overlay);
#if FACTORY_WATCH_UI
    lv_label_set_text(message, "GPS data is going to the USB serial port.");
#else
    lv_label_set_text(message, "All GPS data is currently being redirected to Serial.");
#endif
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(message, LV_PCT(90));
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(message, UI_COLOR_TEXT_SECONDARY, 0);
#if FACTORY_WATCH_UI
    lv_obj_set_style_text_font(message, UI_FONT_SMALL, 0);

    /* A full-width 82 px pill, like the other actions. */
    lv_obj_t *exit_btn = lv_btn_create(nmea_serial_overlay);
    lv_obj_set_size(exit_btn, LV_PCT(100), 82);
    lv_obj_set_style_bg_color(exit_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(exit_btn, UI_COLOR_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(exit_btn, 0, 0);
    lv_obj_set_style_shadow_width(exit_btn, 0, 0);
    lv_obj_set_style_radius(exit_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(exit_btn, nmea_serial_exit_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, "Stop sending");
    lv_obj_set_style_text_font(exit_label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(exit_label, lv_color_black(), 0);
    lv_obj_center(exit_label);
#else
    lv_obj_set_style_text_font(message, is_screen_small() ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);

    lv_obj_t *exit_btn = lv_btn_create(nmea_serial_overlay);
    lv_obj_set_size(exit_btn, is_screen_small() ? 120 : 150, is_screen_small() ? 40 : 44);
    lv_obj_set_style_bg_color(exit_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(exit_btn, 0, 0);
    lv_obj_set_style_radius(exit_btn, 8, 0);
    ui_add_accent_focus_style(exit_btn);
    lv_obj_add_event_cb(exit_btn, nmea_serial_exit_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, LV_SYMBOL_CLOSE " Exit");
    lv_obj_set_style_text_color(exit_label, lv_color_white(), 0);
    lv_obj_center(exit_label);
#endif

    if (nmea_serial_prev_group) {
        lv_group_add_obj(nmea_serial_prev_group, exit_btn);
        lv_group_set_editing(nmea_serial_prev_group, false);
        lv_group_focus_obj(exit_btn);
        lv_group_focus_freeze(nmea_serial_prev_group, true);
    }
}

static void nmea_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    nmea_serial_switch = sw;
    nmea_to_serial = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (nmea_to_serial) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev) {
            lv_indev_wait_release(indev);
        }
        Serial.begin(GNSS_UI_NMEA_SERIAL_BAUD);
        hw_set_gps_nmea_serial(true);
        show_nmea_serial_mode();
    } else {
        close_nmea_serial_mode();
    }
    if (timer) {
        lv_timer_set_period(timer, nmea_to_serial ? 20 : 1000);
    }
}

static void fix_beep_switch_event(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    fix_beep_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
}

static void back_event_handler(lv_event_t *e)
{
    close_nmea_serial_mode();
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    if (detail_build_timer) {
        lv_timer_del(detail_build_timer);
        detail_build_timer = NULL;
    }
    if (page_container) {
        ui_destroy_app_page(page_container);
        page_container = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    nmea_serial_switch = NULL;
    detail_ui_ready = false;
    detail_cn0_bar_index = 0;
    cn0_card = NULL;
    last_pps_event_time = 0;
    last_fix_beep_time = 0;
    hw_gps_detach_pps();
    menu_show();
}

static bool check_gnss_available(void)
{
    return (hw_get_device_online() & HW_GPS_ONLINE);
}

static void gnss_unavailable_msgbox_cb(lv_event_t *e)
{
    (void)e;
    if (gnss_unavailable_msgbox) {
        destroy_msgbox(gnss_unavailable_msgbox);
        gnss_unavailable_msgbox = NULL;
    }
    menu_show();
}

static void show_gnss_unavailable_msgbox(void)
{
    if (gnss_unavailable_msgbox) {
        return;
    }
    static const char *btns[] = {"OK", ""};
    gnss_unavailable_msgbox = create_msgbox(
                                  lv_scr_act(),
                                  "GNSS",
                                  "GNSS not detected.",
                                  btns,
                                  gnss_unavailable_msgbox_cb,
                                  NULL);
}

void ui_gnss_enter(lv_obj_t *parent)
{
    if (!check_gnss_available()) {
        show_gnss_unavailable_msgbox();
        return;
    }

    memset(&gnss_labels, 0, sizeof(gnss_labels));
    nmea_to_serial = false;
    hw_set_gps_nmea_serial(false);
    nmea_serial_switch = NULL;
    fix_beep_enabled = true;
    detail_ui_ready = false;
    detail_cn0_bar_index = 0;
    cn0_card = NULL;
    last_pps_event_time = 0;
    last_fix_beep_time = 0;

    page_container = ui_create_app_page(parent, "GNSS", back_event_handler);
    hw_gps_attach_pps();

    timer = lv_timer_create(timer_cb, 1000, NULL);
    detail_build_timer = lv_timer_create(detail_build_timer_cb, 1, NULL);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        hw_feedback();
        back_event_handler(e);
    }, NULL);
#endif
}

void ui_gnss_exit(lv_obj_t *parent)
{
    (void)parent;
    close_nmea_serial_mode();
}

app_t ui_gnss_main = {
    .setup_func_cb = ui_gnss_enter,
    .exit_func_cb = ui_gnss_exit,
    .user_data = nullptr,
};

#endif
