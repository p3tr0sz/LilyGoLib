/**
 * @file      ui_watch_face_edit.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Watch face editor and lock screen settings. Settings > Watch face lists the
 * four corners, each a picker of values and apps. Long-pressing the face opens
 * the same page on its own, and going back returns to the face.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

static const char *const slot_names[WATCH_FACE_SLOT_COUNT] = {
    "Top left", "Top right", "Bottom left", "Bottom right",
};

/* A corner's picker lists these values first, then the apps in app-list order. */
static const uint8_t slot_value_types[] = {
    WATCH_SLOT_NONE, WATCH_SLOT_BATTERY, WATCH_SLOT_WIFI,
    WATCH_SLOT_DATE, WATCH_SLOT_STEPS, WATCH_SLOT_CALORIES,
};
static const char *const slot_value_names[] = {
    "None", "Battery", "Wi-Fi", "Date", "Steps", "Calories",
};
#define SLOT_VALUE_COUNT (sizeof(slot_value_types) / sizeof(slot_value_types[0]))

static lv_obj_t *editor_slot_rows[WATCH_FACE_SLOT_COUNT];
static lv_obj_t *editor_page;
/* Opened by a long press on one corner: picking goes straight back to the face. */
static bool editor_direct;

#define DIRECT_CLOSE_DELAY_MS   250     /* Long enough to see the check mark move */

static void editor_close(void)
{
    if (!editor_page) return;
    lv_obj_t *page = editor_page;
    editor_page = NULL;
    editor_direct = false;
    ui_destroy_app_page(page);
    menu_show();
}

static void editor_close_async(void *user_data)
{
    LV_UNUSED(user_data);
    editor_close();
}

static void editor_close_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    editor_close();
}

/* FNV-1a, so a saved slot keeps its app however the app list is ordered. */
uint32_t watch_app_name_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    for (; name && *name; name++) {
        hash ^= (uint8_t)*name;
        hash *= 16777619u;
    }
    return hash;
}

/* Apps that were renamed, so slots saved under the old name still find them */
static const struct {
    const char *old_name;
    const char *name;
} app_renames[] = {
    { "Setting", "Settings" },
    { "Clock Tools", "Alarm" },     /* Split into Stopwatch, Timer and Alarm */
};

static bool app_matches_hash(const ui_app_entry_t *entry, uint32_t hash)
{
    if (!entry) return false;
    if (watch_app_name_hash(entry->name) == hash) return true;
    for (size_t i = 0; i < sizeof(app_renames) / sizeof(app_renames[0]); i++) {
        if (strcmp(entry->name, app_renames[i].name) == 0 &&
                watch_app_name_hash(app_renames[i].old_name) == hash) {
            return true;
        }
    }
    return false;
}

const ui_app_entry_t *watch_app_find_by_hash(uint32_t hash)
{
    uint32_t count = ui_app_registry_count();
    for (uint32_t i = 0; i < count; i++) {
        const ui_app_entry_t *entry = ui_app_registry_get(i);
        if (app_matches_hash(entry, hash)) return entry;
    }
    return NULL;
}

static uint16_t slot_option_index(const watch_slot_setting_t &slot)
{
    if (slot.type == WATCH_SLOT_APP) {
        uint32_t count = ui_app_registry_count();
        for (uint32_t i = 0; i < count; i++) {
            if (app_matches_hash(ui_app_registry_get(i), slot.app_hash)) {
                return SLOT_VALUE_COUNT + i;
            }
        }
        return 0;   /* The app is gone, so the face shows nothing there */
    }
    for (uint16_t i = 0; i < SLOT_VALUE_COUNT; i++) {
        if (slot_value_types[i] == slot.type) return i;
    }
    return 0;
}

static void slot_picker_cb(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target_obj(e);
    uint8_t index = (uint8_t)(uintptr_t)lv_obj_get_user_data(dropdown);
    uint32_t option = lv_dropdown_get_selected(dropdown);

    watch_slot_setting_t slot = {WATCH_SLOT_NONE, 0};
    if (option < SLOT_VALUE_COUNT) {
        slot.type = slot_value_types[option];
    } else if (option - SLOT_VALUE_COUNT < ui_app_registry_count()) {
        slot.type = WATCH_SLOT_APP;
        slot.app_hash = watch_app_name_hash(ui_app_registry_get(option - SLOT_VALUE_COUNT)->name);
    }
    hw_set_watch_face_slot(index, slot);
    watch_face_rebuild();
    if (editor_direct) {
        lv_timer_create(editor_close_timer_cb, DIRECT_CLOSE_DELAY_MS, NULL);
    }
}

static lv_obj_t *add_caption(lv_obj_t *content, const char *text)
{
    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, 12, 0);
    lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_SECONDARY, 0);
    return label;
}

void watch_face_editor_build(lv_obj_t *content)
{
    /* Values, then app names, one per line */
    size_t size = 1;
    uint32_t app_count = ui_app_registry_count();
    for (uint32_t i = 0; i < SLOT_VALUE_COUNT; i++) size += strlen(slot_value_names[i]) + 1;
    for (uint32_t i = 0; i < app_count; i++) size += strlen(ui_app_registry_get(i)->name) + 1;
    char *options = (char *)lv_malloc(size);
    if (!options) return;
    options[0] = '\0';
    for (uint32_t i = 0; i < SLOT_VALUE_COUNT + app_count; i++) {
        if (i > 0) lv_strcat(options, "\n");
        lv_strcat(options, i < SLOT_VALUE_COUNT ? slot_value_names[i] :
                  ui_app_registry_get(i - SLOT_VALUE_COUNT)->name);
    }

    lv_obj_t *card = ui_create_card(content, NULL);
    for (uint8_t i = 0; i < WATCH_FACE_SLOT_COUNT; i++) {
        lv_obj_t *row = ui_create_card_picker(card, NULL, slot_names[i], options,
                                              slot_option_index(watch_face_slot_resolved(i)),
                                              slot_picker_cb);
        lv_obj_set_user_data(lv_obj_get_child(row, -1), (void *)(uintptr_t)i);
        editor_slot_rows[i] = row;
    }
    lv_free(options);

    add_caption(content, "Tip: long-press the watch face to change a corner.");
}

static void lock_mode_cb(lv_event_t *e)
{
    hw_set_watch_lock_mode((uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(e)));
}

void watch_lock_screen_build(lv_obj_t *content)
{
    lv_obj_t *card = ui_create_card(content, NULL);
    ui_create_card_picker(card, NULL, "When idle", "Screen off\nClock",
                          hw_get_watch_lock_mode(), lock_mode_cb);
    add_caption(content, "Clock stays on for one more screen timeout, then the screen turns off. "
                "Tap or swipe up to return to the watch face.");
}

/* The editor as an app of its own, for the long press on the face */
static void editor_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    editor_close();
}

/* Direct edit: swiping back from the choices also returns to the face, not the list. */
static void direct_picker_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (editor_direct) lv_async_call(editor_close_async, NULL);
}

static void editor_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    editor_page = NULL;
    editor_direct = false;
}

static void editor_enter(lv_obj_t *parent)
{
    editor_direct = false;
    editor_page = ui_create_app_page(parent, "Watch face", editor_back_cb);
    lv_obj_add_event_cb(editor_page, editor_page_delete_cb, LV_EVENT_DELETE, NULL);
    watch_face_editor_build(editor_page);
}

static void editor_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
}

static app_t editor_app = {
    .setup_func_cb = editor_enter,
    .exit_func_cb = editor_exit,
    .user_data = nullptr,
};

void watch_face_editor_open(int slot)
{
    watch_shell_launch(&editor_app, WATCH_VIEW_FACE);
    if (slot >= 0 && slot < WATCH_FACE_SLOT_COUNT && editor_page &&
            watch_shell_get_view() == WATCH_VIEW_APP) {
        /* Open that corner's choices straight away. */
        lv_obj_t *root = (lv_obj_t *)lv_obj_get_user_data(editor_page);
        uint32_t pages = lv_obj_get_child_count(root);
        lv_obj_send_event(editor_slot_rows[slot], LV_EVENT_CLICKED, NULL);
        if (lv_obj_get_child_count(root) > pages) {
            editor_direct = true;
            lv_obj_add_event_cb(lv_obj_get_child(root, -1), direct_picker_delete_cb, LV_EVENT_DELETE, NULL);
        }
    }
}

#endif /* FACTORY_WATCH_UI */
