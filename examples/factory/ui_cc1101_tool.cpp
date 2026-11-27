/**
 * @file      ui_cc1101_tool.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-07-30
 *
 */
#include "ui_define.h"
#include <math.h>
#include <string.h>
#ifdef ARDUINO
#include <SD.h>
#endif
#include "capture_store.h"

#if !defined(EXCLUDE_CC1101_TOOL)

#define CC1101_MODE_LIST        "Disable\n""Beacon TX\n""RX Capture"
#define CC1101_INTERVAL_LIST    "1000ms\n""2000ms\n""3000ms"
#define CC1101_BIT_RATE_LIST    "1kbps\n""5kbps\n""10kbps\n""38.4kbps\n""100kbps"
#define CC1101_MOD_LIST         "OOK\n""2-FSK"
#define CC1101_TX_POWER_LIST    "0dBm\n""5dBm\n""7dBm\n""10dBm"
#define CC1101_MAX_PAYLOAD      255

static const uint16_t interval_args_list[] = {1000, 2000, 3000};
static const uint16_t bit_rate_args_list[] = {1, 5, 10, 38, 100};
static const uint8_t tx_power_args_list[] = {0, 5, 7, 10};

static lv_obj_t *page_container = NULL;
static lv_obj_t *radio_msg_label = NULL;
static radio_params_t radio_params_copy;
static uint8_t radio_run_mode = RADIO_DISABLE;
static lv_timer_t *timer = NULL;
static uint8_t last_rx_payload[CC1101_MAX_PAYLOAD];
static uint8_t last_rx_len = 0;
static uint32_t beacon_count = 0;
static uint32_t capture_count = 0;
static uint32_t replay_count = 0;

/* Capture catalog: /subghz/<name>.sub holds the radio params + captured bytes. */
#define SUBGHZ_CATALOG  "subghz"
#define MAX_SUBGHZ      24
static int subghz_counter = 1;
static lv_obj_t *filename_ta = NULL;
static lv_obj_t *capture_dd = NULL;
static char subghz_names[MAX_SUBGHZ][64];
static int subghz_count = 0;

struct subghz_file_hdr {
    char magic[4];       /* "SUBG" */
    uint8_t version;     /* 1 */
    uint8_t sf;          /* modulation flag (1=OOK, 0=2-FSK) */
    uint8_t cr;          /* bit rate */
    uint8_t power;       /* TX power */
    float freq;
    float bandwidth;
    uint8_t len;         /* payload length */
};

static void radio_timer_task(lv_timer_t *t);
static void ui_set_msg_label(const char *msg);
static void replay_last_packet(void);
static void scan_subghz(void);

static uint8_t find_freq_index(float value)
{
    for (uint16_t i = 0; i < radio_get_freq_length(); ++i) {
        if (fabsf(radio_get_freq_from_index(i) - value) < 0.01f) {
            return i;
        }
    }
    return 0;
}

static uint8_t find_bandwidth_index(float value)
{
    for (uint16_t i = 0; i < radio_get_bandwidth_length(); ++i) {
        if (fabsf(radio_get_bandwidth_from_index(i) - value) < 0.1f) {
            return i;
        }
    }
    return 3;
}

static uint8_t find_bit_rate_index(uint16_t value)
{
    for (uint8_t i = 0; i < (sizeof(bit_rate_args_list) / sizeof(bit_rate_args_list[0])); ++i) {
        if (bit_rate_args_list[i] == value || (value == 0 && bit_rate_args_list[i] == 38)) {
            return i;
        }
    }
    return 3;
}

static uint8_t find_power_index(uint8_t value)
{
    for (uint8_t i = 0; i < (sizeof(tx_power_args_list) / sizeof(tx_power_args_list[0])); ++i) {
        if (tx_power_args_list[i] == value) {
            return i;
        }
    }
    return 3;
}

static void format_hex_payload(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!data || len == 0) {
        snprintf(out, out_len, "--");
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 < out_len; ++i) {
        int written = snprintf(out + pos, out_len - pos, "%s%02X", i ? " " : "", data[i]);
        if (written <= 0) {
            break;
        }
        if ((size_t)written >= out_len - pos) {
            break;
        }
        pos += (size_t)written;
    }
}

static void back_event_handler(lv_event_t *e)
{
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }

    radio_run_mode = RADIO_DISABLE;
    radio_params_copy.mode = RADIO_DISABLE;
    hw_set_radio_params(radio_params_copy);

    if (page_container) {
        ui_destroy_app_page(page_container);
        page_container = NULL;
    }
    filename_ta = NULL;
    capture_dd = NULL;

    menu_show();
}

static void ui_set_msg_label(const char *msg)
{
    if (radio_msg_label) {
        lv_textarea_set_text(radio_msg_label, msg);
        lv_textarea_set_cursor_pos(radio_msg_label, 0);
    }
}

static void apply_radio_params(void)
{
    char msg[128];
    int16_t state = hw_set_radio_params(radio_params_copy);
    radio_run_mode = radio_params_copy.mode;

    switch (radio_run_mode) {
    case RADIO_DISABLE:
        if (timer) {
            lv_timer_pause(timer);
        }
        snprintf(msg, sizeof(msg), "RADIO DISABLE state=%d", state);
        ui_set_msg_label(msg);
        break;
    case RADIO_TX:
        if (timer) {
            lv_timer_set_period(timer, radio_params_copy.interval);
            lv_timer_resume(timer);
        }
        snprintf(msg, sizeof(msg), "Beacon TX %.2fMHz %s state=%d",
                 radio_params_copy.freq,
                 radio_params_copy.sf ? "OOK" : "2-FSK",
                 state);
        ui_set_msg_label(msg);
        break;
    case RADIO_RX:
        if (timer) {
            lv_timer_set_period(timer, 300);
            lv_timer_resume(timer);
        }
        snprintf(msg, sizeof(msg), "RX Capture %.2fMHz %s state=%d",
                 radio_params_copy.freq,
                 radio_params_copy.sf ? "OOK" : "2-FSK",
                 state);
        ui_set_msg_label(msg);
        break;
    default:
        break;
    }
}

static void _ui_cc1101_obj_event(lv_event_t *e)
{
    uint16_t selected = 0;
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    const char *flag = (const char *)lv_event_get_user_data(e);

    if (*flag != 'b' && *flag != 'r' && *flag != 's' && *flag != 'l') {
        selected = lv_dropdown_get_selected(obj);
    }

    switch (*flag) {
    case 'f':
        radio_params_copy.freq = radio_get_freq_from_index(selected);
        break;
    case 'w':
        radio_params_copy.bandwidth = radio_get_bandwidth_from_index(selected);
        break;
    case 'p':
        radio_params_copy.power = tx_power_args_list[selected];
        break;
    case 'i':
        radio_params_copy.interval = interval_args_list[selected];
        break;
    case 'c':
        radio_params_copy.cr = bit_rate_args_list[selected];
        break;
    case 'o':
        radio_params_copy.sf = (selected == 0) ? 1 : 0;
        break;
    case 'm':
        radio_params_copy.mode = selected;
        break;
    case 'b':
        apply_radio_params();
        break;
    case 'r':
        replay_last_packet();
        break;
    case 's':
        save_subghz_capture(filename_ta ? lv_textarea_get_text(filename_ta) : "");
        break;
    case 'l': {
        const char *bn = NULL;
        if (capture_dd && subghz_count > 0) {
            uint16_t i = lv_dropdown_get_selected(capture_dd);
            if (i < (uint16_t)subghz_count) bn = subghz_names[i];
        }
        load_subghz_capture(bn);
        break;
    }
    default:
        break;
    }
}

static lv_obj_t *create_state_textarea(lv_obj_t *card)
{
    radio_msg_label = lv_textarea_create(card);
    lv_textarea_set_text_selection(radio_msg_label, false);
    lv_textarea_set_cursor_click_pos(radio_msg_label, false);
    lv_textarea_set_one_line(radio_msg_label, true);
    lv_obj_set_scrollbar_mode(radio_msg_label, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(radio_msg_label, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(radio_msg_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(radio_msg_label, lv_color_white(), 0);
    lv_obj_set_style_border_color(radio_msg_label, UI_COLOR_DIVIDER, 0);
    lv_obj_set_style_border_width(radio_msg_label, 1, 0);
    lv_obj_set_style_radius(radio_msg_label, 6, 0);
    lv_obj_set_style_pad_all(radio_msg_label, 4, 0);

    lv_obj_add_event_cb(radio_msg_label, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
        if (code == LV_EVENT_CLICKED) {
            lv_group_set_editing((lv_group_t *)lv_obj_get_group(ta), false);
        }
    }, LV_EVENT_ALL, NULL);

    ui_create_card_item(card, LV_SYMBOL_WIFI, "State", radio_msg_label);
    return radio_msg_label;
}

static lv_obj_t *create_mode_dropdown(lv_obj_t *card)
{
    static const char flag = 'm';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, CC1101_MODE_LIST);
    lv_dropdown_set_selected(dd, radio_params_copy.mode);
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_SETTINGS, "Mode", dd);
    return dd;
}

static lv_obj_t *create_frequency_dropdown(lv_obj_t *card)
{
    static const char flag = 'f';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, radio_get_freq_list());
    lv_dropdown_set_selected(dd, find_freq_index(radio_params_copy.freq));
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_WIFI, "Frequency", dd);
    return dd;
}

static lv_obj_t *create_bandwidth_dropdown(lv_obj_t *card)
{
    static const char flag = 'w';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, radio_get_bandwidth_list(false));
    lv_dropdown_set_selected(dd, find_bandwidth_index(radio_params_copy.bandwidth));
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_SETTINGS, "RX BW", dd);
    return dd;
}

static lv_obj_t *create_tx_power_dropdown(lv_obj_t *card)
{
    static const char flag = 'p';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, CC1101_TX_POWER_LIST);
    lv_dropdown_set_selected(dd, find_power_index(radio_params_copy.power));
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_WIFI, "TX Power", dd);
    return dd;
}

static lv_obj_t *create_tx_interval_dropdown(lv_obj_t *card)
{
    static const char flag = 'i';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, CC1101_INTERVAL_LIST);
    lv_dropdown_set_selected(dd, 2);
    for (uint8_t i = 0; i < (sizeof(interval_args_list) / sizeof(interval_args_list[0])); ++i) {
        if (interval_args_list[i] == radio_params_copy.interval) {
            lv_dropdown_set_selected(dd, i);
            break;
        }
    }
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_LOOP, "Tx Interval", dd);
    return dd;
}

static lv_obj_t *create_bit_rate_dropdown(lv_obj_t *card)
{
    static const char flag = 'c';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, CC1101_BIT_RATE_LIST);
    lv_dropdown_set_selected(dd, find_bit_rate_index(radio_params_copy.cr));
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_SETTINGS, "Bit Rate", dd);
    return dd;
}

static lv_obj_t *create_mod_dropdown(lv_obj_t *card)
{
    static const char flag = 'o';
    lv_obj_t *dd = lv_dropdown_create(card);
    lv_dropdown_set_options(dd, CC1101_MOD_LIST);
    lv_dropdown_set_selected(dd, radio_params_copy.sf ? 0 : 1);
    lv_obj_add_event_cb(dd, _ui_cc1101_obj_event, LV_EVENT_VALUE_CHANGED, (void *)&flag);
    ui_create_card_item(card, LV_SYMBOL_SETTINGS, "Modulation", dd);
    return dd;
}

static void replay_last_packet(void)
{
#ifdef ARDUINO
    char hex[CC1101_MAX_PAYLOAD * 3 + 4];
    char msg[192];

    if (last_rx_len == 0) {
        ui_set_msg_label("No captured packet");
        return;
    }

    radio_params_t tx_cfg = radio_params_copy;
    tx_cfg.mode = RADIO_TX;
    int16_t cfg_state = hw_set_radio_params(tx_cfg);
    if (cfg_state != 0) {
        snprintf(msg, sizeof(msg), "Replay config failed:%d", cfg_state);
        ui_set_msg_label(msg);
        return;
    }

    bool ok = radio_transmit(last_rx_payload, last_rx_len);
    int16_t tx_state = radio_get_last_transmit_state();
    format_hex_payload(last_rx_payload, last_rx_len, hex, sizeof(hex));

    if (ok) {
        replay_count++;
        snprintf(msg, sizeof(msg), "Replay #%lu len=%u %s",
                 (unsigned long)replay_count,
                 (unsigned)last_rx_len,
                 hex);
    } else {
        snprintf(msg, sizeof(msg), "Replay failed:%d len=%u",
                 tx_state,
                 (unsigned)last_rx_len);
    }
    ui_set_msg_label(msg);

    if (radio_run_mode == RADIO_RX) {
        radio_params_t rx_cfg = radio_params_copy;
        rx_cfg.mode = RADIO_RX;
        hw_set_radio_params(rx_cfg);
    } else if (radio_run_mode == RADIO_DISABLE) {
        radio_params_t off_cfg = radio_params_copy;
        off_cfg.mode = RADIO_DISABLE;
        hw_set_radio_params(off_cfg);
    }
#else
    ui_set_msg_label("Replay is hardware only");
#endif
}

/* Rescan /subghz for saved *.sub captures and fill the picker. */
static void scan_subghz(void)
{
    subghz_count = capture_list(SUBGHZ_CATALOG, ".sub", subghz_names, MAX_SUBGHZ);
    if (!capture_dd) return;
    if (subghz_count == 0) {
        lv_dropdown_set_options(capture_dd, "No captures");
        return;
    }
    char opts[MAX_SUBGHZ * 65];
    opts[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < subghz_count; i++) {
        used += snprintf(opts + used, sizeof(opts) - used, "%s%s",
                         i ? "\n" : "", subghz_names[i]);
        if (used >= sizeof(opts)) break;
    }
    lv_dropdown_set_options(capture_dd, opts);
    lv_dropdown_set_selected(capture_dd, 0);
}

/* Save the last captured packet (and the radio params to replay it) to SD. */
static void save_subghz_capture(const char *name)
{
#ifdef ARDUINO
    if (last_rx_len == 0) { ui_set_msg_label("No captured packet to save"); return; }
    if (!hw_is_sd_insert() || !capture_catalog_ensure(SUBGHZ_CATALOG)) {
        ui_set_msg_label("Insert an SD card to save"); return;
    }
    char safe[48];
    if (!capture_sanitize_name(name, safe, sizeof(safe))) {
        ui_set_msg_label("Enter a filename"); return;
    }
    char path[96];
    capture_path(SUBGHZ_CATALOG, safe, ".sub", path, sizeof(path));
    File f = SD.open(path, FILE_WRITE);
    if (!f) { ui_set_msg_label("SD write failed"); return; }

    subghz_file_hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "SUBG", 4);
    h.version = 1;
    h.sf = radio_params_copy.sf;
    h.cr = radio_params_copy.cr;
    h.power = radio_params_copy.power;
    h.freq = radio_params_copy.freq;
    h.bandwidth = radio_params_copy.bandwidth;
    h.len = last_rx_len;
    f.write((uint8_t *)&h, sizeof(h));
    f.write(last_rx_payload, last_rx_len);
    f.close();

    subghz_counter++;
    char msg[96];
    snprintf(msg, sizeof(msg), "Saved %s.sub (%uB)", safe, (unsigned)last_rx_len);
    ui_set_msg_label(msg);
    scan_subghz();
#else
    (void)name;
    ui_set_msg_label("Save is hardware only");
#endif
}

/* Load a saved capture into last_rx_payload + params, ready for Replay. */
static void load_subghz_capture(const char *basename)
{
#ifdef ARDUINO
    if (!basename || !basename[0]) { ui_set_msg_label("No capture selected"); return; }
    char path[96];
    capture_path(SUBGHZ_CATALOG, basename, ".sub", path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) { ui_set_msg_label("Open failed"); return; }

    subghz_file_hdr h;
    if (f.read((uint8_t *)&h, sizeof(h)) != (int)sizeof(h) ||
            memcmp(h.magic, "SUBG", 4) != 0) {
        f.close(); ui_set_msg_label("Bad capture file"); return;
    }
    uint8_t n = h.len > CC1101_MAX_PAYLOAD ? CC1101_MAX_PAYLOAD : h.len;
    int got = f.read(last_rx_payload, n);
    f.close();
    if (got != (int)n) { ui_set_msg_label("Short read"); return; }

    last_rx_len = n;
    radio_params_copy.sf = h.sf;
    radio_params_copy.cr = h.cr;
    radio_params_copy.power = h.power;
    radio_params_copy.freq = h.freq;
    radio_params_copy.bandwidth = h.bandwidth;

    char msg[96];
    snprintf(msg, sizeof(msg), "Loaded %s (%uB) — tap Replay", basename, (unsigned)n);
    ui_set_msg_label(msg);
#else
    (void)basename;
    ui_set_msg_label("Load is hardware only");
#endif
}

static void radio_timer_task(lv_timer_t *t)
{
    (void)t;
#ifdef ARDUINO
    static radio_rx_params_t rx_params;
    char msg[192];
    char hex[CC1101_MAX_PAYLOAD * 3 + 4];
    int tick = lv_tick_get() / 1000;
    uint8_t tmp_buffer[CC1101_MAX_PAYLOAD] = {0};
    char tx_payload[CC1101_MAX_PAYLOAD] = {0};

    switch (radio_run_mode) {
    case RADIO_DISABLE:
        break;
    case RADIO_TX:
        snprintf(tx_payload, sizeof(tx_payload), "LILYGO-SUBG-%04lu", (unsigned long)beacon_count++);
        if (radio_transmit((const uint8_t *)tx_payload, strlen(tx_payload))) {
            format_hex_payload((const uint8_t *)tx_payload, strlen(tx_payload), hex, sizeof(hex));
            snprintf(msg, sizeof(msg), "[%u] Beacon #%lu len=%u %s",
                     tick,
                     (unsigned long)beacon_count,
                     (unsigned)strlen(tx_payload),
                     hex);
            ui_set_msg_label(msg);
        } else {
            snprintf(msg, sizeof(msg), "[%u] Beacon failed:%d",
                     tick,
                     radio_get_last_transmit_state());
            ui_set_msg_label(msg);
        }
        break;
    case RADIO_RX:
        rx_params.data = tmp_buffer;
        rx_params.length = sizeof(tmp_buffer);
        hw_get_radio_rx(rx_params);
        if (rx_params.state == 0 && rx_params.length > 0) {
            last_rx_len = rx_params.length > CC1101_MAX_PAYLOAD ? CC1101_MAX_PAYLOAD : rx_params.length;
            memcpy(last_rx_payload, rx_params.data, last_rx_len);
            capture_count++;
            format_hex_payload(last_rx_payload, last_rx_len, hex, sizeof(hex));
            snprintf(msg, sizeof(msg), "[%u] RX #%lu RSSI=%d len=%u %s",
                     tick,
                     (unsigned long)capture_count,
                     rx_params.rssi,
                     (unsigned)last_rx_len,
                     hex);
            ui_set_msg_label(msg);
        }
        break;
    default:
        break;
    }
#endif
}

void ui_cc1101_tool_enter(lv_obj_t *parent)
{
    static const char start_flag = 'b';
    static const char replay_flag = 'r';

    page_container = ui_create_app_page(parent, "Sub-G", back_event_handler);

    hw_get_radio_params(radio_params_copy);
    radio_params_copy.cr = radio_params_copy.cr ? radio_params_copy.cr : 38;
    last_rx_len = 0;
    beacon_count = 0;
    capture_count = 0;
    replay_count = 0;

    lv_obj_t *card = ui_create_card(page_container, "State");
    create_state_textarea(card);
    ui_set_msg_label("RADIO DISABLE");

    card = ui_create_card(page_container, "Mode");
    create_mode_dropdown(card);

    card = ui_create_card(page_container, "Parameters");
    create_frequency_dropdown(card);
    create_mod_dropdown(card);
    create_bit_rate_dropdown(card);
    create_bandwidth_dropdown(card);
    create_tx_power_dropdown(card);
    create_tx_interval_dropdown(card);

    static const char save_flag = 's';
    static const char load_flag = 'l';

    card = ui_create_card(page_container, "Save capture");
    filename_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(filename_ta, true);
    lv_textarea_set_max_length(filename_ta, 40);
    {
        char default_name[48];
        capture_make_name(SUBGHZ_CATALOG, subghz_counter, default_name, sizeof(default_name));
        lv_textarea_set_text(filename_ta, default_name);
    }
    lv_obj_set_style_bg_color(filename_ta, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_opa(filename_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(filename_ta, lv_color_white(), 0);
    lv_obj_set_style_border_width(filename_ta, 1, 0);
    lv_obj_set_style_radius(filename_ta, 8, 0);
    ui_create_card_item(card, LV_SYMBOL_FILE, "Name", filename_ta);
    {
        lv_obj_t *save_btn = lv_btn_create(card);
        lv_obj_set_width(save_btn, LV_PCT(100));
        lv_obj_set_style_bg_color(save_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(save_btn, 8, 0);
        lv_obj_set_style_border_width(save_btn, 0, 0);
        ui_add_accent_focus_style(save_btn);
        lv_obj_t *l = lv_label_create(save_btn);
        lv_label_set_text(l, LV_SYMBOL_SAVE " Save to SD");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(save_btn, _ui_cc1101_obj_event, LV_EVENT_CLICKED, (void *)&save_flag);
    }

    card = ui_create_card(page_container, "Saved captures");
    capture_dd = lv_dropdown_create(card);
    lv_dropdown_set_options(capture_dd, "No captures");
    ui_create_card_item(card, LV_SYMBOL_SD_CARD, "Capture", capture_dd);
    {
        lv_obj_t *load_btn = lv_btn_create(card);
        lv_obj_set_width(load_btn, LV_PCT(100));
        lv_obj_set_style_bg_color(load_btn, UI_COLOR_CARD_BG, 0);
        lv_obj_set_style_bg_opa(load_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(load_btn, 8, 0);
        lv_obj_set_style_border_width(load_btn, 1, 0);
        lv_obj_set_style_border_color(load_btn, UI_COLOR_DIVIDER, 0);
        ui_add_accent_focus_style(load_btn);
        lv_obj_t *l = lv_label_create(load_btn);
        lv_label_set_text(l, LV_SYMBOL_DOWNLOAD " Load");
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(load_btn, _ui_cc1101_obj_event, LV_EVENT_CLICKED, (void *)&load_flag);
    }
    scan_subghz();

    timer = lv_timer_create(radio_timer_task, 1000, NULL);
    lv_timer_pause(timer);

    lv_obj_t *btn_row = lv_obj_create(page_container);
    lv_obj_set_size(btn_row, LV_PCT(100), 86);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_set_style_pad_row(btn_row, 8, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back_btn = lv_btn_create(btn_row);
    lv_obj_set_size(back_btn, LV_PCT(30), 36);
    lv_obj_set_style_bg_color(back_btn, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_border_color(back_btn, UI_COLOR_DIVIDER, 0);
    {
        lv_obj_t *l = lv_label_create(back_btn);
        lv_label_set_text(l, LV_SYMBOL_LEFT " Back");
        lv_obj_set_style_text_color(l, UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(back_btn, [](lv_event_t *e) {
        back_event_handler(e);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_btn = lv_btn_create(btn_row);
    lv_obj_set_size(ok_btn, LV_PCT(30), 36);
    lv_obj_set_style_bg_color(ok_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ok_btn, 8, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    ui_add_accent_focus_style(ok_btn);
    {
        lv_obj_t *l = lv_label_create(ok_btn);
        lv_label_set_text(l, LV_SYMBOL_OK " Start");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(ok_btn, _ui_cc1101_obj_event, LV_EVENT_CLICKED, (void *)&start_flag);

    lv_obj_t *replay_btn = lv_btn_create(btn_row);
    lv_obj_set_size(replay_btn, LV_PCT(30), 36);
    lv_obj_set_style_bg_color(replay_btn, lv_color_hex(0x2FB344), 0);
    lv_obj_set_style_bg_opa(replay_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(replay_btn, 8, 0);
    lv_obj_set_style_border_width(replay_btn, 0, 0);
    ui_add_accent_focus_style(replay_btn);
    {
        lv_obj_t *l = lv_label_create(replay_btn);
        lv_label_set_text(l, LV_SYMBOL_PLAY " Replay");
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(replay_btn, _ui_cc1101_obj_event, LV_EVENT_CLICKED, (void *)&replay_flag);
}

void ui_cc1101_tool_exit(lv_obj_t *parent)
{
    (void)parent;
}

app_t ui_cc1101_tool_main = {
    .setup_func_cb = ui_cc1101_tool_enter,
    .exit_func_cb = ui_cc1101_tool_exit,
    .user_data = nullptr,
};

#endif /* EXCLUDE_CC1101_TOOL */
