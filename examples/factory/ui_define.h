/**
 * @file      ui_define.h
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_mac.h>
#else
#define RTC_DATA_ATTR
#endif
#include <lvgl.h>
#include <LVGL_Compat.h>
#ifndef ARDUINO
/* Arduino-compatible clock helper for native SDL builds. */
static inline uint32_t millis(void) { return lv_tick_get(); }
#endif
#include <stdio.h>
#include <vector>
#include <time.h>
#include <string.h>
#include "hal_interface.h"

#if defined(LILYGO_FACTORY_ENABLE_WALKIE) && LILYGO_FACTORY_ENABLE_WALKIE
#define FACTORY_HAS_WALKIE_DEPS 1
#elif defined(ARDUINO) && __has_include(<PCMFlow.h>) && __has_include(<PCMFlowG722.h>)
#define FACTORY_HAS_WALKIE_DEPS 1
#else
#define FACTORY_HAS_WALKIE_DEPS 0
#endif

#if !FACTORY_HAS_WALKIE_DEPS
#ifndef EXCLUDE_WALKIE
#define EXCLUDE_WALKIE
#endif
#endif

#if defined(LILYGO_FACTORY_ENABLE_INA219) && LILYGO_FACTORY_ENABLE_INA219
#define FACTORY_HAS_INA219_DEPS 1
#elif defined(ARDUINO) && __has_include(<Wire.h>) && __has_include(<Adafruit_INA219.h>)
#define FACTORY_HAS_INA219_DEPS 1
#else
#define FACTORY_HAS_INA219_DEPS 0
#endif

#if !FACTORY_HAS_INA219_DEPS
#ifndef EXCLUDE_INA219
#define EXCLUDE_INA219
#endif
#endif

#if !defined(EXCLUDE_BLE_SCANNER)
#include <string>
using std::string;
#endif
using std::vector;

#define DEFAULT_OPA          100

/* ── Theme color presets ── */
typedef struct {
    uint32_t accent;       /* Main accent color */
    uint32_t accent_dim;   /* Pressed state */
    uint32_t card_bg;      /* Card background */
    uint32_t track;        /* Slider/progress track */
    const char *name;      /* Display name */
} ui_theme_preset_t;

static const ui_theme_preset_t ui_theme_presets[] = {
    { 0x00D4AA, 0x009977, 0x1A1A1A, 0x333333, "Mint"     },  /* 0: Current default */
    { 0x0088FF, 0x0066CC, 0x1A1A2E, 0x333344, "Ocean"    },  /* 1: Blue */
    { 0xFF6B35, 0xCC5522, 0x2A1A1A, 0x443333, "Sunset"   },  /* 2: Orange */
#if FACTORY_WATCH_UI
    /* A step lighter on the watch: 0xAA55FF on this card is 4.4 : 1, under 4.5. */
    { 0xB577FF, 0x8833CC, 0x1E1A2A, 0x3A3344, "Violet"   },  /* 3: Purple */
#else
    { 0xAA55FF, 0x8833CC, 0x1E1A2A, 0x3A3344, "Violet"   },  /* 3: Purple */
#endif
    { 0xFF3366, 0xCC2255, 0x2A1A20, 0x443338, "Cherry"   },  /* 4: Pink/Red */
    { 0x88CC00, 0x669900, 0x1A2A1A, 0x334433, "Lime"     },  /* 5: Green */
    { 0xFFB800, 0xCC9200, 0x2A2A1A, 0x444433, "Amber"    },  /* 6: Gold */
    { 0xFFFFFF, 0xCCCCCC, 0x2A2A2A, 0x555555, "Mono"     },  /* 7: Monochrome */
};
#define UI_THEME_PRESET_COUNT (sizeof(ui_theme_presets) / sizeof(ui_theme_presets[0]))

/* ── Active color palette (runtime mutable) ── */
extern uint32_t ui_active_accent;
extern uint32_t ui_active_accent_dim;
extern uint32_t ui_active_card_bg;
extern uint32_t ui_active_track;

#define UI_COLOR_BG           lv_color_black()
#define UI_COLOR_CARD_BG      lv_color_hex(ui_active_card_bg)
#define UI_COLOR_CARD_FOCUS   lv_color_hex(0x2A2A2A)
#define UI_COLOR_ACCENT       lv_color_hex(ui_active_accent)
#define UI_COLOR_ACCENT_DIM   lv_color_hex(ui_active_accent_dim)
#define UI_COLOR_ACCENT_FOCUS_BORDER lv_color_black()
#define UI_COLOR_ACCENT_FOCUS_OUTLINE lv_color_white()
#define UI_COLOR_WARNING      lv_color_hex(0xFF6B35)
#define UI_COLOR_TEXT_PRIMARY  lv_color_white()
#if FACTORY_WATCH_UI
/* At least 5.5 : 1 on every preset's card (0x888888 was 4.0 on Mono). */
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0xA0A0A0)
#else
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0x888888)
#endif
#define UI_COLOR_DIVIDER      lv_color_hex(0x333333)
#define UI_COLOR_TRACK        lv_color_hex(ui_active_track)
#define UI_COLOR_KNOB         lv_color_white()

/* ── Accents ──
 * On the watch, every Montserrat size the app uses points at a font with the
 * accented Latin letters (src/font/font_latin.c, from tools/gen_latin_fonts.py).
 * Each holds only those glyphs and falls back to LVGL's built-in font, so plain
 * text is unchanged. Other boards keep the built-ins. */
#if FACTORY_WATCH_UI
/* Nothing that has to be read is under 22 px (watch-ux-guidelines.md 4.1,
 * Apple's 12 pt minimum), so the sizes apps chose for small screens map to the
 * smallest readable ones: 10 to 16 -> 22, 18 -> 24. 20 px stays for the few
 * places that ask for it directly (header clock). */
LV_FONT_DECLARE(font_latin_20)
LV_FONT_DECLARE(font_latin_22)
LV_FONT_DECLARE(font_latin_24)
#define lv_font_montserrat_10 font_latin_22
#define lv_font_montserrat_12 font_latin_22
#define lv_font_montserrat_14 font_latin_22
#define lv_font_montserrat_16 font_latin_22
#define lv_font_montserrat_18 font_latin_24
#define lv_font_montserrat_20 font_latin_20
LV_FONT_DECLARE(font_latin_22)
#define lv_font_montserrat_22 font_latin_22
LV_FONT_DECLARE(font_latin_24)
#define lv_font_montserrat_24 font_latin_24
LV_FONT_DECLARE(font_latin_28)
#define lv_font_montserrat_28 font_latin_28
LV_FONT_DECLARE(font_latin_32)
#define lv_font_montserrat_32 font_latin_32
LV_FONT_DECLARE(font_latin_48)
#define lv_font_montserrat_48 font_latin_48
#endif

/* ── Size tokens ──
 * The watch shell's screen is ~315 ppi, so it uses larger type and touch
 * targets. On other boards each font token is the size it replaces. */
#if FACTORY_WATCH_UI
/* watch-ux-guidelines.md 4.1: nothing that has to be read under 22 px (Apple's
 * 12 pt minimum); body text as close to 32 px (17 pt) as rows allow. */
#define UI_FONT_TINY          (&lv_font_montserrat_22)   /* was 10 px */
#define UI_FONT_CAPTION       (&lv_font_montserrat_22)   /* was 12 px */
#define UI_FONT_SMALL         (&lv_font_montserrat_24)   /* was 14 px */
#define UI_ROW_MIN_H          88   /* 3.1: 82 px (44 pt) is the comfortable target */
#define UI_APP_NAV_H          44
/* One side margin for every watch screen (app list, app pages, sheets): the
 * Ultra's display has rounded corners, so content stays this far from the edge. */
#define UI_SCREEN_MARGIN      24
#define UI_APP_PAD_HOR        UI_SCREEN_MARGIN
#define UI_CARD_PAD_HOR       16   /* Card edge to its caption, icons, values and dividers */
#define UI_ROW_ICON_W         30   /* A card row's icon column */
#else
#define UI_FONT_TINY          (&lv_font_montserrat_10)
#define UI_FONT_CAPTION       (&lv_font_montserrat_12)
#define UI_FONT_SMALL         (&lv_font_montserrat_14)
#define UI_ROW_MIN_H          0
#define UI_APP_NAV_H          32
#define UI_APP_PAD_HOR        8
#define UI_CARD_PAD_HOR       12
#endif
#if FACTORY_WATCH_UI
#define UI_FONT_BODY          (&lv_font_montserrat_28)
#else
#define UI_FONT_BODY          (MAIN_FONT)
#endif

/* ── Shared styles ── */
typedef struct {
    lv_style_t card;
    lv_style_t card_item;
    lv_style_t accent_btn;
    lv_style_t accent_focus_ring;
    lv_style_t focus_glow;
    lv_style_t text_secondary;
    lv_style_t accent_text;   /* Text + shadow color that follows theme accent */
    lv_style_t accent_shadow; /* Shadow-only style for glow effects */
    lv_style_t divider;
    lv_style_t slider_track;
    lv_style_t slider_knob;
    lv_style_t switch_on;
    lv_style_t switch_off;
} ui_styles_t;

extern ui_styles_t ui_styles;
void ui_styles_init(void);

typedef void (*app_func_t)(lv_obj_t *parent);

typedef struct {
    app_func_t setup_func_cb;
    app_func_t exit_func_cb;
    void *user_data;
} app_t;


enum {
    LV_MENU_ITEM_BUILDER_VARIANT_1,
    LV_MENU_ITEM_BUILDER_VARIANT_2
};
typedef uint8_t lv_menu_builder_variant_t;

#define MSG_MENU_NAME_CHANGED    100
#define MSG_LABEL_PARAM_CHANGE_1 200
#define MSG_LABEL_PARAM_CHANGE_2 201
#define MSG_TITLE_NAME_CHANGE    203
#define MSG_BLE_SEND_DATA_1      204
#define MSG_BLE_SEND_DATA_2      205
#define MSG_MUSIC_TIME_ID        300
#define MSG_MUSIC_TIME_END_ID    301
#define MSG_FFT_ID               400

extern lv_obj_t *main_screen;

lv_obj_t *ui_create_option(lv_obj_t *parent, const char *title, const char *symbol_txt, lv_obj_t *(*widget_create)(lv_obj_t *parent), lv_event_cb_t btn_event_cb);
lv_obj_t *create_text(lv_obj_t *parent, const char *icon, const char *txt,
                      lv_menu_builder_variant_t builder_variant);
lv_obj_t *create_slider(lv_obj_t *parent, const char *icon, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb, lv_event_code_t filter);
lv_obj_t *create_switch(lv_obj_t *parent, const char *icon, const char *txt, bool chk, lv_event_cb_t cb);
lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb);
lv_obj_t *create_label(lv_obj_t *parent, const char *icon, const char *txt, const char *default_text);
lv_obj_t *create_dropdown(lv_obj_t *parent, const char *icon, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb);
lv_obj_t *create_msgbox(lv_obj_t *parent, const char *title_txt,
                        const char *msg_txt, const char **btns,
                        lv_event_cb_t btns_event_cb, void *user_data);
void destroy_msgbox(lv_obj_t *msgbox);

lv_indev_t *lv_get_encoder_indev();
lv_indev_t *lv_get_keyboard_indev();
void menu_show();
void menu_hidden();
void set_default_group(lv_group_t *group);

lv_obj_t *ui_create_process_bar(lv_obj_t *parent, const char *title);

void theme_init();
void ui_styles_refresh(void);
void ui_theme_apply(void);
void ui_add_accent_focus_style(lv_obj_t *obj);

void disable_input_devices();
void enable_input_devices();

void set_low_power_mode_flag(bool enable);

void disable_keyboard();
void enable_keyboard();

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void* user_data);
lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb);
lv_obj_t *create_radius_button(lv_obj_t *parent, const void *image, lv_event_cb_t event_cb, void* user_data);

/* ── New card-style UI components ── */
lv_obj_t *ui_create_app_page(lv_obj_t *parent, const char *title, lv_event_cb_t back_cb);
void ui_enable_edge_swipe_back(lv_obj_t *root, lv_obj_t *back_target,
                               lv_event_code_t back_event = LV_EVENT_CLICKED);
/* Innermost object containing obj that has swipe-back (an app page or pushed page), or NULL. */
lv_obj_t *ui_swipe_back_root(lv_obj_t *obj);
/* Swipe-back normally ignores swipes that start on sliders, text fields and button
 * matrices. An object with this flag (and its children) lets them through; it must
 * then act on release, not press (e.g. LV_BUTTONMATRIX_CTRL_CLICK_TRIG). */
#define UI_FLAG_SWIPE_BACK_OK   LV_OBJ_FLAG_USER_2
void ui_destroy_app_page(lv_obj_t *content);
void ui_app_page_enable_nav_auto_hide(lv_obj_t *content, uint32_t timeout_ms);
void ui_app_page_disable_nav_auto_hide(lv_obj_t *content);
lv_obj_t *ui_create_card(lv_obj_t *parent, const char *title);
lv_obj_t *ui_create_card_item(lv_obj_t *card, const char *icon, const char *title, lv_obj_t *widget);
lv_obj_t *ui_create_card_slider(lv_obj_t *card, const char *icon, const char *title,
                                 int32_t min, int32_t max, int32_t val, lv_event_cb_t cb);
void ui_prepare_slider_for_encoder(lv_obj_t *slider);
void ui_style_textarea(lv_obj_t *textarea);
void ui_prepare_textarea_for_encoder(lv_obj_t *textarea);
typedef struct {
    lv_obj_t *lifted_obj;
    lv_obj_t *original_parent;
    int32_t original_index;
} ui_soft_keyboard_lift_t;
bool ui_soft_keyboard_should_open(void);
void ui_soft_keyboard_show(lv_obj_t *keyboard, lv_obj_t *textarea,
                           lv_obj_t *lift_obj, ui_soft_keyboard_lift_t *lift);
void ui_soft_keyboard_hide(lv_obj_t *keyboard, ui_soft_keyboard_lift_t *lift);
void ui_soft_keyboard_restore(ui_soft_keyboard_lift_t *lift);
lv_obj_t *ui_create_card_switch(lv_obj_t *card, const char *icon, const char *title,
                                 bool checked, lv_event_cb_t cb);
lv_obj_t *ui_create_card_dropdown(lv_obj_t *card, const char *icon, const char *title,
                                   const char *options, uint8_t sel, lv_event_cb_t cb);
/* Same arguments as ui_create_card_dropdown(), and the row's last child is still the
 * lv_dropdown that holds the choice and sends LV_EVENT_VALUE_CHANGED. On the watch,
 * tapping the row opens a list page instead of the dropdown popup. */
lv_obj_t *ui_create_card_picker(lv_obj_t *card, const char *icon, const char *title,
                                 const char *options, uint8_t sel, lv_event_cb_t cb);
lv_obj_t *ui_create_card_button(lv_obj_t *card, const char *icon, const char *title,
                                 const char *btn_text, lv_event_cb_t cb);
/* Hides or shows a card row together with the divider under it. */
void ui_card_row_set_hidden(lv_obj_t *row, bool hidden);
lv_obj_t *ui_create_card_info(lv_obj_t *card, const char *icon, const char *title,
                               const char *value);
lv_obj_t *ui_create_status_bar(lv_obj_t *parent);
void ui_status_bar_update(void);

#if FACTORY_WATCH_UI
/* ── Nested pages (ui_watch_pages.cpp) ──
 * ui_push_page() slides a sub-page, laid out like ui_create_app_page(), over the
 * page that holds `from` and returns its content. Swiping right pops it. */
lv_obj_t *ui_push_page(lv_obj_t *from, const char *title);
void ui_pop_page(lv_obj_t *content);
/* ui_create_card_item() calls this for a dropdown row: the dropdown only shows the
 * choice, and tapping the row opens a radio list. Apps don't need to call it. */
void ui_picker_adopt(lv_obj_t *row, lv_obj_t *title_label, lv_obj_t *dd);
/* ui_create_card_item() calls this for a text field row: the field is hidden, the row
 * shows its text (dots for a password, the placeholder when empty) and a tap opens
 * ui_open_text_page(). The field keeps the text, so apps read it as before. */
void ui_text_adopt(lv_obj_t *row, lv_obj_t *title_label, lv_obj_t *ta);
/* A full-screen keyboard page that edits a copy of `ta`'s text. The OK key writes it
 * back and sends LV_EVENT_VALUE_CHANGED and LV_EVENT_READY to `ta`; the keyboard
 * key or swiping right leaves `ta` as it was. Number-only fields get the number pad. */
void ui_open_text_page(lv_obj_t *from, const char *title, lv_obj_t *ta);
/* Settings > Keyboard: which language's accented letters the keyboard offers. */
void ui_keyboard_settings_build(lv_obj_t *content);
/* A tappable pill row: icon in an accent disc and the title. */
lv_obj_t *ui_create_menu_row(lv_obj_t *parent, const char *symbol, const char *title,
                             lv_event_cb_t cb, void *user_data);
/* ui_create_card_slider() on the watch: the row shows the value ("60% >") and opens
 * a full-screen −/+ page. Its last child is a hidden lv_slider that holds the value
 * and sends LV_EVENT_VALUE_CHANGED. */
lv_obj_t *ui_create_card_level(lv_obj_t *card, const char *icon, const char *title,
                                int32_t min, int32_t max, int32_t val, lv_event_cb_t cb);
/* ui_create_card_button() on the watch: one verb on a full-width pill. The row's last
 * child is the button, and the button's first child is the label. */
lv_obj_t *ui_create_card_action(lv_obj_t *card, const char *icon, const char *label,
                                lv_event_cb_t cb);
/* An action pill that shows only an icon (LV_SYMBOL_OK, …), for verbs the icon
 * already says, such as confirming the wheels on a "Set" page. */
lv_obj_t *ui_create_card_icon_action(lv_obj_t *card, const char *symbol, lv_event_cb_t cb);
/* A found network or device: the icon in the signal's colour, the name over a grey
 * detail line, and the signal ("-52 dBm") on the right. A card row like the others
 * (same height, edges and divider); a tap anywhere on it runs cb with user_data. */
lv_obj_t *ui_create_card_signal_row(lv_obj_t *card, const char *icon, const char *title,
                                    const char *detail, const char *value, lv_color_t color,
                                    lv_event_cb_t cb, void *user_data);
/* A short message at the bottom of the screen that goes away by itself. */
void ui_toast(const char *text);
/* Scroll wheels for picking numbers: ui_push_wheel_page() pushes a page with an
 * empty row for the wheels (returned in *wheels_row); add a "Set" pill to the
 * returned content after the wheels. */
lv_obj_t *ui_push_wheel_page(lv_obj_t *from, const char *title, lv_obj_t **wheels_row);
lv_obj_t *ui_create_wheel(lv_obj_t *parent, const char *options, uint32_t sel,
                          int32_t width, bool infinite);
/* "00\n01\n…" up to count - 1, for ui_create_wheel(). out needs 3 * count bytes. */
void ui_wheel_number_options(char *out, size_t out_size, int count, int first);
#endif
/* For actions that can't be undone: on the watch, the control's LV_EVENT_CLICKED
 * handlers run only after it is held for about a second; a tap shows "Hold to
 * confirm". Does nothing on other boards. */
void ui_require_hold(lv_obj_t *obj);
/* The same, with the toast saying what the hold will do ("Hold to restart"). */
void ui_require_hold_hint(lv_obj_t *obj, const char *hint);

bool is_screen_small(void);
void ui_set_font_size_pref(uint8_t pref);
uint8_t ui_get_font_size_pref(void);
void ui_apply_theme_preset(uint8_t preset_idx);
void update_page_indicator(void);

#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif
