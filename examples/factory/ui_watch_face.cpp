/**
 * @file      ui_watch_face.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Watch face (home screen): time, date and four corner slots. A slot shows a
 * value (battery, Wi-Fi) or an app shortcut; tapping it opens the related app.
 * Swiping up anywhere on the face pulls up the app list.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

LV_FONT_DECLARE(font_alibaba_100);
LV_FONT_DECLARE(font_alibaba_24);

#define FACE_SLOT_COUNT        4
#define FACE_SLOT_W            168
#define FACE_SLOT_H            64
#define FACE_SLOT_MARGIN_X     28   /* Keeps slots clear of the rounded screen corners */
#define FACE_SLOT_MARGIN_Y     22
#define FACE_SLOT_ICON_SIZE    32
#define FACE_DRAG_START_PX     12

typedef enum {
    FACE_SLOT_EMPTY,
    FACE_SLOT_BATTERY,
    FACE_SLOT_WIFI,
    FACE_SLOT_APP,
} face_slot_type_t;

typedef struct {
    face_slot_type_t type;
    const char *app_name;   /* App shortcut, or the app a value slot opens */
} face_slot_config_t;

/* Fixed defaults until the face becomes configurable */
static const face_slot_config_t face_slot_defaults[FACE_SLOT_COUNT] = {
    { FACE_SLOT_BATTERY, "Power"       },   /* Top left */
    { FACE_SLOT_WIFI,    "WiFi"        },   /* Top right */
    { FACE_SLOT_APP,     "Clock Tools" },   /* Bottom left */
    { FACE_SLOT_APP,     "Setting"     },   /* Bottom right */
};

static const lv_align_t face_slot_align[FACE_SLOT_COUNT] = {
    LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT,
};

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *icon;
    lv_obj_t *value;
    face_slot_type_t type;
} face_slot_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *date;
    face_slot_t slots[FACE_SLOT_COUNT];
} face_ui;

static void face_slot_click_cb(lv_event_t *e)
{
    app_t *app = (app_t *)lv_event_get_user_data(e);
    watch_shell_launch(app, WATCH_VIEW_FACE);
}

/* Swipe up: the app list follows the finger, and opens past a fifth of the screen. */
static void face_touch_cb(lv_event_t *e)
{
    static lv_point_t start;
    static bool tracking = false;
    static bool dragging = false;

    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        start = point;
        tracking = true;
        dragging = false;
    } else if (code == LV_EVENT_PRESSING && tracking) {
        int32_t dy = start.y - point.y;
        if (!dragging && dy > FACE_DRAG_START_PX && dy > LV_ABS(point.x - start.x)) {
            dragging = true;
        }
        if (dragging) {
            watch_shell_list_drag(dy);
        }
    } else if (code == LV_EVENT_RELEASED && tracking) {
        tracking = false;
        if (!dragging) return;
        dragging = false;
        int32_t dy = start.y - point.y;
        watch_shell_list_drag_end(dy > lv_display_get_vertical_resolution(NULL) / 5);
        /* A drag that started on a slot must not also open its app. */
        lv_indev_reset(indev, NULL);
    }
}

static void face_slot_create(face_slot_t *slot, uint32_t index, const face_slot_config_t *config)
{
    const ui_app_entry_t *entry = ui_app_registry_find(config->app_name);
    slot->type = config->type;
    if (slot->type == FACE_SLOT_EMPTY || (slot->type == FACE_SLOT_APP && !entry)) {
        slot->type = FACE_SLOT_EMPTY;
        return;
    }

    lv_obj_t *obj = lv_obj_create(face_ui.root);
    lv_obj_set_size(obj, FACE_SLOT_W, FACE_SLOT_H);
    lv_obj_set_style_bg_color(obj, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_bg_color(obj, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(obj, 16, 0);
    lv_obj_set_style_pad_ver(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 10, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Lets a swipe up start on a slot */
    int32_t x = (index % 2 == 0) ? FACE_SLOT_MARGIN_X : -FACE_SLOT_MARGIN_X;
    int32_t y = (index < 2) ? FACE_SLOT_MARGIN_Y : -FACE_SLOT_MARGIN_Y;
    lv_obj_align(obj, face_slot_align[index], x, y);
    if (entry) {
        lv_obj_add_event_cb(obj, face_slot_click_cb, LV_EVENT_CLICKED, entry->app_func);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }

    if (slot->type == FACE_SLOT_APP && entry->icon) {
        slot->icon = lv_image_create(obj);
        lv_image_set_src(slot->icon, entry->icon);
        lv_obj_set_style_image_recolor(slot->icon, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_image_recolor_opa(slot->icon, LV_OPA_COVER, 0);
        uint32_t img_max = LV_MAX(entry->icon->header.w, entry->icon->header.h);
        if (img_max > 0) lv_image_set_scale(slot->icon, FACE_SLOT_ICON_SIZE * 256 / img_max);
        lv_obj_set_size(slot->icon, FACE_SLOT_ICON_SIZE, FACE_SLOT_ICON_SIZE);
        lv_image_set_inner_align(slot->icon, LV_IMAGE_ALIGN_CENTER);
    } else {
        slot->icon = lv_label_create(obj);
        lv_obj_set_style_text_font(slot->icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_ACCENT, 0);
    }
    lv_obj_remove_flag(slot->icon, LV_OBJ_FLAG_CLICKABLE);

    slot->value = lv_label_create(obj);
    lv_obj_set_style_text_font(slot->value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(slot->value, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(slot->value, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(slot->value, 1);
    if (slot->type == FACE_SLOT_APP) {
        lv_label_set_text(slot->value, entry->name);
    }

    slot->obj = obj;
}

static void face_slot_update(face_slot_t *slot, const monitor_params_t &params)
{
    switch (slot->type) {
    case FACE_SLOT_BATTERY: {
        const char *symbol = LV_SYMBOL_BATTERY_FULL;
        if (params.charging) symbol = LV_SYMBOL_CHARGE;
        else if (params.battery_percent < 20) symbol = LV_SYMBOL_BATTERY_EMPTY;
        else if (params.battery_percent < 50) symbol = LV_SYMBOL_BATTERY_2;
        else if (params.battery_percent < 80) symbol = LV_SYMBOL_BATTERY_3;
        lv_label_set_text(slot->icon, symbol);
        lv_label_set_text_fmt(slot->value, "%d%%", params.battery_percent);
        break;
    }
    case FACE_SLOT_WIFI: {
        bool connected = hw_get_wifi_connected();
        lv_label_set_text(slot->icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(slot->icon,
                                    connected ? UI_COLOR_ACCENT : UI_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(slot->value, connected ? "Online" : "Offline");
        break;
    }
    default:
        break;
    }
}

lv_obj_t *watch_face_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, face_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, face_touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(root, face_touch_cb, LV_EVENT_RELEASED, NULL);
    face_ui.root = root;

    face_ui.time = lv_label_create(root);
    lv_obj_set_style_text_font(face_ui.time, &font_alibaba_100, 0);
    lv_obj_set_style_text_color(face_ui.time, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(face_ui.time, "00:00");
    lv_obj_align(face_ui.time, LV_ALIGN_CENTER, 0, -18);

    face_ui.date = lv_label_create(root);
    lv_obj_set_style_text_font(face_ui.date, &font_alibaba_24, 0);
    lv_obj_add_style(face_ui.date, &ui_styles.accent_text, 0);
    lv_label_set_text(face_ui.date, "---");
    lv_obj_align(face_ui.date, LV_ALIGN_CENTER, 0, 46);

    for (uint32_t i = 0; i < FACE_SLOT_COUNT; i++) {
        face_slot_create(&face_ui.slots[i], i, &face_slot_defaults[i]);
    }

    /* Swipe-up handle, like a phone's home indicator */
    lv_obj_t *handle = lv_obj_create(root);
    lv_obj_set_size(handle, 56, 5);
    lv_obj_set_style_bg_color(handle, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_60, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_radius(handle, 3, 0);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -12);

    return root;
}

void watch_face_update(void)
{
    if (!face_ui.root) return;

    static const char *week[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                 };
    struct tm timeinfo = {0};
    hw_get_date_time(timeinfo);
    lv_label_set_text_fmt(face_ui.time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    lv_label_set_text_fmt(face_ui.date, "%s %d %s",
                          week[LV_CLAMP(0, timeinfo.tm_wday, 6)], timeinfo.tm_mday,
                          month[LV_CLAMP(0, timeinfo.tm_mon, 11)]);

    monitor_params_t params;
    hw_get_monitor_params(params);
    for (uint32_t i = 0; i < FACE_SLOT_COUNT; i++) {
        if (face_ui.slots[i].obj) {
            face_slot_update(&face_ui.slots[i], params);
        }
    }
}

#endif /* FACTORY_WATCH_UI */
