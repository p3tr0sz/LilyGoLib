/**
 * @file      ui_watch_face.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Watch face (home screen): time, date and four corner slots. A slot shows an
 * app shortcut or Wi-Fi state as a round icon, or a value (battery, date,
 * steps, calories) as a pill; tapping it opens the related app. Slots are set in Settings > Watch face, or by
 * long-pressing the face. Swiping up anywhere on the face pulls up the app list.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

LV_FONT_DECLARE(font_alibaba_100);
LV_FONT_DECLARE(font_alibaba_24);
LV_IMG_DECLARE(img_gps_track);
LV_IMG_DECLARE(img_energy);

#define FACE_SLOT_W            196
#define FACE_SLOT_H            64
#define FACE_ROUND_SLOT_SIZE   84   /* Diameter of icon-only slots */
#define FACE_ROUND_ICON_SIZE   48
#define FACE_SLOT_MARGIN_X     28   /* Keeps slots clear of the rounded screen corners */
#define FACE_SLOT_MARGIN_Y     22
#define FACE_SLOT_ICON_SIZE    32
#define FACE_DRAG_START_PX     12
#define FACE_KCAL_PER_100_STEPS 4   /* About 0.04 kcal a step for an average adult */

typedef struct {
    uint8_t type;           /* watch_slot_type_t */
    const char *app_name;   /* For WATCH_SLOT_APP */
} face_slot_default_t;

/* Used while a slot is set to WATCH_SLOT_DEFAULT */
static const face_slot_default_t face_slot_defaults[WATCH_FACE_SLOT_COUNT] = {
    { WATCH_SLOT_BATTERY, NULL          },  /* Top left */
    { WATCH_SLOT_WIFI,    NULL          },  /* Top right */
    { WATCH_SLOT_APP,     "Alarm" },        /* Bottom left */
    { WATCH_SLOT_APP,     "Settings"    },  /* Bottom right */
};

static const lv_align_t face_slot_align[WATCH_FACE_SLOT_COUNT] = {
    LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT,
};

static const char *const face_week[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const face_month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                                        };

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *icon;
    lv_obj_t *value;
    uint8_t type;
} face_slot_t;

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *date;
    face_slot_t slots[WATCH_FACE_SLOT_COUNT];
    uint32_t accent;        /* Accent the slot icons were coloured with */
} face_ui;

static struct {
    lv_point_t start;
    bool tracking;
    bool dragging;
} face_drag;

watch_slot_setting_t watch_face_slot_resolved(uint8_t index)
{
    watch_slot_setting_t slot = hw_get_watch_face_slot(index);
    if (slot.type == WATCH_SLOT_DEFAULT && index < WATCH_FACE_SLOT_COUNT) {
        slot.type = face_slot_defaults[index].type;
        slot.app_hash = face_slot_defaults[index].app_name ?
                        watch_app_name_hash(face_slot_defaults[index].app_name) : 0;
    }
    return slot;
}

/* The app a value slot opens */
static const char *face_value_app_name(uint8_t type)
{
    switch (type) {
    case WATCH_SLOT_BATTERY: return "Power";
    case WATCH_SLOT_WIFI:    return "WiFi";
    case WATCH_SLOT_DATE:    return "Alarm";
    default:                 return NULL;
    }
}

static void face_slot_click_cb(lv_event_t *e)
{
    app_t *app = (app_t *)lv_event_get_user_data(e);
    watch_shell_launch(app, WATCH_VIEW_FACE);
}

/* Swipe up: the app list follows the finger, and opens past a fifth of the screen. */
static void face_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        face_drag.start = point;
        face_drag.tracking = true;
        face_drag.dragging = false;
    } else if (code == LV_EVENT_PRESSING && face_drag.tracking) {
        int32_t dy = face_drag.start.y - point.y;
        if (!face_drag.dragging && dy > FACE_DRAG_START_PX &&
                dy > LV_ABS(point.x - face_drag.start.x)) {
            face_drag.dragging = true;
        }
        if (face_drag.dragging) {
            watch_shell_list_drag(dy);
        }
    } else if (code == LV_EVENT_RELEASED && face_drag.tracking) {
        face_drag.tracking = false;
        if (!face_drag.dragging) return;
        face_drag.dragging = false;
        int32_t dy = face_drag.start.y - point.y;
        watch_shell_list_drag_end(dy > lv_display_get_vertical_resolution(NULL) / 5);
        /* A drag that started on a slot must not also open its app. */
        lv_indev_reset(indev, NULL);
    }
}

/* Long press: edit the face, starting at the slot under the finger. */
static void face_long_press_cb(lv_event_t *e)
{
    if (face_drag.dragging || watch_shell_get_view() != WATCH_VIEW_FACE) return;
    lv_obj_t *target = lv_event_get_target_obj(e);
    int slot = -1;
    for (int i = 0; i < WATCH_FACE_SLOT_COUNT; i++) {
        if (face_ui.slots[i].obj && face_ui.slots[i].obj == target) slot = i;
    }
    face_drag.tracking = false;
    /* The release must not also open the slot's app. */
    lv_indev_wait_release(lv_indev_active());
    watch_face_editor_open(slot);
}

static void face_slot_set_image(face_slot_t *slot, const lv_image_dsc_t *image, int32_t size)
{
    slot->icon = lv_image_create(slot->obj);
    lv_image_set_src(slot->icon, image);
    lv_obj_set_style_image_recolor(slot->icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_image_recolor_opa(slot->icon, LV_OPA_COVER, 0);
    uint32_t img_max = LV_MAX(image->header.w, image->header.h);
    if (img_max > 0) lv_image_set_scale(slot->icon, size * 256 / img_max);
    lv_obj_set_size(slot->icon, size, size);
    lv_image_set_inner_align(slot->icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(slot->icon, LV_OBJ_FLAG_SCROLLABLE);
}

static void face_slot_create(face_slot_t *slot, uint32_t index)
{
    watch_slot_setting_t config = watch_face_slot_resolved(index);
    const ui_app_entry_t *entry = NULL;
    if (config.type == WATCH_SLOT_APP) {
        entry = watch_app_find_by_hash(config.app_hash);
    } else if (face_value_app_name(config.type)) {
        entry = ui_app_registry_find(face_value_app_name(config.type));
    }
    slot->type = config.type;
    if (slot->type == WATCH_SLOT_NONE || (slot->type == WATCH_SLOT_APP && !entry)) {
        slot->type = WATCH_SLOT_NONE;
        return;
    }

    /* Shortcuts and Wi-Fi need no words; an app without an icon keeps its name. */
    bool icon_only = (slot->type == WATCH_SLOT_APP && entry->icon) ||
                     slot->type == WATCH_SLOT_WIFI;

    lv_obj_t *obj = lv_obj_create(face_ui.root);
    slot->obj = obj;
    if (icon_only) lv_obj_set_size(obj, FACE_ROUND_SLOT_SIZE, FACE_ROUND_SLOT_SIZE);
    else lv_obj_set_size(obj, FACE_SLOT_W, FACE_SLOT_H);
    lv_obj_add_style(obj, &ui_styles.card, 0);     /* Follows the theme */
    lv_obj_set_style_bg_color(obj, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(obj, icon_only ? 0 : 16, 0);
    lv_obj_set_style_pad_ver(obj, 0, 0);
    lv_obj_set_style_pad_column(obj, 10, 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, icon_only ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);   /* Lets a swipe up start on a slot */
    int32_t x = (index % 2 == 0) ? FACE_SLOT_MARGIN_X : -FACE_SLOT_MARGIN_X;
    /* Round slots share their centre line with a pill in the same row. */
    int32_t margin_y = FACE_SLOT_MARGIN_Y -
                       (icon_only ? (FACE_ROUND_SLOT_SIZE - FACE_SLOT_H) / 2 : 0);
    int32_t y = (index < 2) ? margin_y : -margin_y;
    lv_obj_align(obj, face_slot_align[index], x, y);
    /* Slots without an app stay clickable, so a long press can still pick them. */
    if (entry) {
        lv_obj_add_event_cb(obj, face_slot_click_cb, LV_EVENT_CLICKED, entry->app_func);
    }

    if (slot->type == WATCH_SLOT_APP && entry->icon) {
        face_slot_set_image(slot, entry->icon, FACE_ROUND_ICON_SIZE);
    } else if (slot->type == WATCH_SLOT_STEPS) {
        face_slot_set_image(slot, &img_gps_track, FACE_SLOT_ICON_SIZE);
    } else if (slot->type == WATCH_SLOT_CALORIES) {
        face_slot_set_image(slot, &img_energy, FACE_SLOT_ICON_SIZE);
    } else {
        slot->icon = lv_label_create(obj);
        lv_obj_set_style_text_font(slot->icon, icon_only ? &lv_font_montserrat_32 :
                                   &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_ACCENT, 0);
    }
    lv_obj_remove_flag(slot->icon, LV_OBJ_FLAG_CLICKABLE);
    if (icon_only) return;

    slot->value = lv_label_create(obj);
    lv_obj_set_style_text_font(slot->value, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(slot->value, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(slot->value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(slot->value, 1);
    lv_obj_set_flex_grow(slot->value, 1);
    lv_obj_set_height(slot->value, lv_font_get_line_height(&lv_font_montserrat_20));
    if (slot->type == WATCH_SLOT_APP) {
        lv_label_set_text(slot->value, entry->name);
    }
}

static void face_slot_update(face_slot_t *slot, const monitor_params_t &params,
                             const struct tm &timeinfo)
{
    switch (slot->type) {
    case WATCH_SLOT_BATTERY: {
        const char *symbol = LV_SYMBOL_BATTERY_FULL;
        if (params.charging) symbol = LV_SYMBOL_CHARGE;
        else if (params.battery_percent < 20) symbol = LV_SYMBOL_BATTERY_EMPTY;
        else if (params.battery_percent < 50) symbol = LV_SYMBOL_BATTERY_2;
        else if (params.battery_percent < 80) symbol = LV_SYMBOL_BATTERY_3;
        lv_label_set_text(slot->icon, symbol);
        lv_label_set_text_fmt(slot->value, "%d%%", params.battery_percent);
        break;
    }
    case WATCH_SLOT_WIFI: {
        bool connected = hw_get_wifi_connected();
        lv_label_set_text(slot->icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(slot->icon,
                                    connected ? UI_COLOR_ACCENT : UI_COLOR_TEXT_SECONDARY, 0);
        break;
    }
    case WATCH_SLOT_DATE:
        lv_label_set_text_fmt(slot->icon, "%d", timeinfo.tm_mday);
        lv_label_set_text_fmt(slot->value, "%s, %s", face_week[LV_CLAMP(0, timeinfo.tm_wday, 6)],
                              face_month[LV_CLAMP(0, timeinfo.tm_mon, 11)]);
        break;
    case WATCH_SLOT_STEPS:
    case WATCH_SLOT_CALORIES: {
        uint32_t steps = 0;
        if (!hw_get_step_count(steps)) {
            lv_label_set_text(slot->value, "--");
        } else if (slot->type == WATCH_SLOT_STEPS) {
            lv_label_set_text_fmt(slot->value, "%" LV_PRIu32 " steps", steps);
        } else {
            lv_label_set_text_fmt(slot->value, "%" LV_PRIu32 " kcal",
                                  steps * FACE_KCAL_PER_100_STEPS / 100);
        }
        break;
    }
    default:
        break;
    }
}

/* Slot icons are coloured inline, so recolour them after a theme change. */
static void face_slot_recolor(face_slot_t *slot)
{
    if (!slot->icon) return;
    if (lv_obj_check_type(slot->icon, &lv_image_class)) {
        lv_obj_set_style_image_recolor(slot->icon, UI_COLOR_ACCENT, 0);
    } else if (slot->type != WATCH_SLOT_WIFI) {     /* Wi-Fi sets its colour on update */
        lv_obj_set_style_text_color(slot->icon, UI_COLOR_ACCENT, 0);
    }
}

static void face_slots_create(void)
{
    ui_styles_init();
    bool pedometer = false;
    for (uint32_t i = 0; i < WATCH_FACE_SLOT_COUNT; i++) {
        face_slot_create(&face_ui.slots[i], i);
        pedometer = pedometer || face_ui.slots[i].type == WATCH_SLOT_STEPS ||
                    face_ui.slots[i].type == WATCH_SLOT_CALORIES;
    }
    /* Count steps only while the face shows them, to save power. */
    hw_set_step_counter_enabled(pedometer);
    face_ui.accent = ui_active_accent;
}

void watch_face_rebuild(void)
{
    if (!face_ui.root) return;
    for (uint32_t i = 0; i < WATCH_FACE_SLOT_COUNT; i++) {
        if (face_ui.slots[i].obj) {
            lv_obj_delete(face_ui.slots[i].obj);
        }
        face_ui.slots[i] = face_slot_t();
    }
    face_slots_create();
    watch_face_update();
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
    lv_obj_add_event_cb(root, face_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
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

    face_slots_create();

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

    struct tm timeinfo = {0};
    hw_get_date_time(timeinfo);
    lv_label_set_text_fmt(face_ui.time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    lv_label_set_text_fmt(face_ui.date, "%s %d %s",
                          face_week[LV_CLAMP(0, timeinfo.tm_wday, 6)], timeinfo.tm_mday,
                          face_month[LV_CLAMP(0, timeinfo.tm_mon, 11)]);

    bool recolor = face_ui.accent != ui_active_accent;
    face_ui.accent = ui_active_accent;
    monitor_params_t params;
    hw_get_monitor_params(params);
    for (uint32_t i = 0; i < WATCH_FACE_SLOT_COUNT; i++) {
        if (face_ui.slots[i].obj) {
            if (recolor) face_slot_recolor(&face_ui.slots[i]);
            face_slot_update(&face_ui.slots[i], params, timeinfo);
        }
    }
}

#endif /* FACTORY_WATCH_UI */
