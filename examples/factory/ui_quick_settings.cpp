/**
 * @file      ui_quick_settings.cpp
 * @license   MIT
 * @date      2026-11-27
 *
 * Quick settings shade, pulled down from the watch face (swipe down, the way a
 * swipe up opens the app list). One screen of the controls reached most often:
 * a brightness slider, a Wi-Fi toggle, a Torch toggle and a shortcut into
 * Settings. Every control follows the one-gesture rule - a whole-tile tap or a
 * single slider - and a swipe right closes the shade like any other page.
 *
 * Deliberately NOT here: a Bluetooth toggle. This build has no global BLE radio
 * state - NimBLE is brought up and torn down per feature (HID, FIDO, scanner),
 * so a shade toggle would either read a meaningless state or tear down a stack
 * an app is using. Left for when a global BLE owner exists (see the roadmap).
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

#define QS_HEADER_H     56
#define QS_TILE_H       120
#define QS_PAD_HOR      UI_SCREEN_MARGIN

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *bright_slider;
    lv_obj_t *bright_value;
    lv_obj_t *wifi_tile;
    lv_obj_t *torch_tile;
    bool wifi_on;       /* Session intent; hardware is the source of truth when it reports connected */
    bool torch_on;
} qs;

#ifndef ARDUINO
static lv_obj_t *torch_overlay;   /* The simulator has no backlight, so show the white light */
#endif
static uint8_t torch_saved_brightness;

/* The persisted brightness, what hw_disp_enable_backlight() restores. */
static uint8_t qs_saved_brightness(void)
{
    user_setting_params_t s;
    hw_get_user_setting(s);
    return s.brightness_level;
}

/* ── Tile look ── */
static void tile_set_state(lv_obj_t *tile, bool on)
{
    lv_obj_t *icon = lv_obj_get_child(tile, 0);
    lv_obj_t *name = lv_obj_get_child(tile, 1);
    lv_obj_set_style_bg_color(tile, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_BG, 0);
    lv_color_t fg = on ? lv_color_black() : UI_COLOR_TEXT_PRIMARY;
    if (icon) lv_obj_set_style_text_color(icon, on ? lv_color_black() : UI_COLOR_ACCENT, 0);
    if (name) lv_obj_set_style_text_color(name, fg, 0);
}

static lv_obj_t *tile_create(lv_obj_t *parent, const char *symbol, const char *name,
                             lv_event_cb_t cb)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_add_style(tile, &ui_styles.card, 0);
    lv_obj_set_height(tile, QS_TILE_H);
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 28, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_pad_row(tile, 10, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);   /* Base objects aren't tappable by default */
    lv_obj_set_style_bg_color(tile, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);

    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
    return tile;
}

/* ── Brightness ── */
static void brightness_persist(uint8_t level)
{
    user_setting_params_t s;
    hw_get_user_setting(s);
    if (s.brightness_level == level) return;
    s.brightness_level = level;
    hw_set_user_setting(s);
}

static void brightness_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(slider);
    if (code == LV_EVENT_VALUE_CHANGED) {
        if (!qs.torch_on) hw_set_disp_backlight(val);   /* Torch owns the backlight while on */
        int32_t min = lv_slider_get_min_value(slider);
        int32_t max = lv_slider_get_max_value(slider);
        int32_t pct = (max > min) ? (val - min) * 100 / (max - min) : 0;
        if (qs.bright_value) lv_label_set_text_fmt(qs.bright_value, "%d%%", (int)pct);
    } else if (code == LV_EVENT_RELEASED) {
        brightness_persist(val);   /* One flash write when the finger lifts, not per step */
    }
}

/* ── Wi-Fi ── */
static void wifi_apply(bool on)
{
    qs.wifi_on = on;
    tile_set_state(qs.wifi_tile, on);
    if (on) hw_wifi_autoconnect();
    else hw_wifi_disconnect();
}

static void wifi_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hw_feedback();
    wifi_apply(!qs.wifi_on);
}

/* ── Torch ── */
static void torch_apply(bool on)
{
    if (on == qs.torch_on) return;
    qs.torch_on = on;
    tile_set_state(qs.torch_tile, on);
    if (on) {
        torch_saved_brightness = hw_get_disp_backlight();
        hw_set_disp_backlight(hw_get_disp_max_brightness());
#ifndef ARDUINO
        if (!torch_overlay) {
            torch_overlay = lv_obj_create(lv_layer_top());
            lv_obj_set_size(torch_overlay, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_bg_color(torch_overlay, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(torch_overlay, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(torch_overlay, 0, 0);
            lv_obj_set_style_radius(torch_overlay, 0, 0);
            lv_obj_remove_flag(torch_overlay, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(torch_overlay, [](lv_event_t *) { torch_apply(false); },
                                LV_EVENT_CLICKED, NULL);
            lv_obj_t *hint = lv_label_create(torch_overlay);
            lv_label_set_text(hint, "Torch - tap to turn off");
            lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
            lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
        }
#endif
    } else {
        hw_set_disp_backlight(torch_saved_brightness ? torch_saved_brightness
                              : qs_saved_brightness());
#ifndef ARDUINO
        if (torch_overlay) {
            lv_obj_delete_async(torch_overlay);
            torch_overlay = NULL;
        }
#endif
    }
}

static void torch_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hw_feedback();
    torch_apply(!qs.torch_on);
}

/* ── Settings shortcut ── */
static void settings_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hw_feedback();
    const ui_app_entry_t *entry = ui_app_registry_find("Settings");
    if (entry) watch_shell_launch(entry->app_func, WATCH_VIEW_FACE);
}

/* ── Build ── */
lv_obj_t *quick_settings_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    qs.root = root;

    qs.time = lv_label_create(root);
    lv_obj_set_style_text_font(qs.time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(qs.time, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(qs.time, "--:--");
    lv_obj_align(qs.time, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *col = lv_obj_create(root);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), lv_display_get_vertical_resolution(NULL) - QS_HEADER_H);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, QS_HEADER_H);
    lv_obj_set_style_pad_hor(col, QS_PAD_HOR, 0);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_NONE);   /* A second drag on the open shade must not shift it */
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    /* Brightness card */
    lv_obj_t *bcard = lv_obj_create(col);
    lv_obj_add_style(bcard, &ui_styles.card, 0);
    lv_obj_set_width(bcard, LV_PCT(100));
    lv_obj_set_height(bcard, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(bcard, 0, 0);
    lv_obj_set_style_radius(bcard, 28, 0);
    lv_obj_set_style_pad_all(bcard, 16, 0);
    lv_obj_set_style_pad_row(bcard, 12, 0);
    lv_obj_set_flex_flow(bcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bcard, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bhead = lv_obj_create(bcard);
    lv_obj_remove_style_all(bhead);
    lv_obj_set_width(bhead, LV_PCT(100));
    lv_obj_set_height(bhead, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bhead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bhead, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bhead, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *btitle = lv_label_create(bhead);
    lv_label_set_text(btitle, "Brightness");
    lv_obj_set_style_text_font(btitle, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(btitle, UI_COLOR_TEXT_PRIMARY, 0);
    qs.bright_value = lv_label_create(bhead);
    lv_obj_set_style_text_font(qs.bright_value, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(qs.bright_value, UI_COLOR_ACCENT, 0);
    lv_label_set_text(qs.bright_value, "--%");

    qs.bright_slider = lv_slider_create(bcard);
    lv_obj_set_width(qs.bright_slider, LV_PCT(100));
    lv_obj_set_height(qs.bright_slider, 28);
    lv_obj_add_style(qs.bright_slider, &ui_styles.slider_track, LV_PART_MAIN);
    lv_obj_add_style(qs.bright_slider, &ui_styles.slider_knob, LV_PART_KNOB);
    lv_slider_set_range(qs.bright_slider, hw_get_disp_min_brightness(),
                        hw_get_disp_max_brightness());
    lv_obj_add_event_cb(qs.bright_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(qs.bright_slider, brightness_cb, LV_EVENT_RELEASED, NULL);

    /* Tile row: Wi-Fi + Torch */
    lv_obj_t *tiles = lv_obj_create(col);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_width(tiles, LV_PCT(100));
    lv_obj_set_height(tiles, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tiles, 16, 0);
    lv_obj_remove_flag(tiles, LV_OBJ_FLAG_SCROLLABLE);
    qs.wifi_tile = tile_create(tiles, LV_SYMBOL_WIFI, "Wi-Fi", wifi_cb);
    qs.torch_tile = tile_create(tiles, LV_SYMBOL_CHARGE, "Torch", torch_cb);

    /* Settings shortcut */
    ui_create_menu_row(col, LV_SYMBOL_SETTINGS, "Settings", settings_cb, NULL);

    return root;
}

void quick_settings_prepare(void)
{
    if (!qs.root) return;
    quick_settings_update();

    uint8_t level = qs_saved_brightness();
    if (qs.bright_slider) {
        lv_slider_set_value(qs.bright_slider, level, LV_ANIM_OFF);
        lv_obj_send_event(qs.bright_slider, LV_EVENT_VALUE_CHANGED, NULL);
    }
    qs.wifi_on = hw_get_wifi_connected() || qs.wifi_on;
    tile_set_state(qs.wifi_tile, qs.wifi_on);
    tile_set_state(qs.torch_tile, qs.torch_on);
}

void quick_settings_update(void)
{
    if (!qs.root) return;
    struct tm timeinfo = {0};
    hw_get_date_time(timeinfo);
    lv_label_set_text_fmt(qs.time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    /* Reflect a connection that landed on its own. */
    if (hw_get_wifi_connected() && !qs.wifi_on) {
        qs.wifi_on = true;
        tile_set_state(qs.wifi_tile, true);
    }
}

#endif /* FACTORY_WATCH_UI */
