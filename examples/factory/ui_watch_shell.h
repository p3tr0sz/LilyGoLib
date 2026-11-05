/**
 * @file      ui_watch_shell.h
 * @license   MIT
 * @date      2026-11-03
 *
 * Smartwatch-style shell: the watch face is home, swiping up opens a
 * vertical app list, and swiping right goes back. Include after ui_define.h.
 */
#pragma once

/* ── App registry (filled by register_app() in ui_main.cpp) ── */
typedef struct {
    const char *name;
    const lv_img_dsc_t *icon;
    app_t *app_func;
} ui_app_entry_t;

uint32_t ui_app_registry_count(void);
const ui_app_entry_t *ui_app_registry_get(uint32_t index);
const ui_app_entry_t *ui_app_registry_find(const char *name);

/* Build the app on the app tile and switch to it. */
void ui_launch_app(app_t *app);

#if FACTORY_WATCH_UI

typedef enum {
    WATCH_VIEW_FACE,
    WATCH_VIEW_LIST,
    WATCH_VIEW_APP,
    WATCH_VIEW_LOCK,    /* Lock mode "Clock": idle clock page before the screen turns off */
} watch_view_t;

/* Shell (ui_watch_shell.cpp) */
void watch_shell_setup(lv_obj_t *home_tile);
void watch_shell_launch(app_t *app, watch_view_t return_view);
void watch_shell_on_app_exit(void);
void watch_shell_poll(void);
void watch_shell_show_face(bool anim);
void watch_shell_list_drag(int32_t dy);
void watch_shell_list_drag_end(bool open);
bool watch_shell_input_guard_active(void);
watch_view_t watch_shell_get_view(void);
void watch_shell_unlock(void);

/* Lock clock page (setupClock() in ui_main.cpp) */
void lock_clock_show(void);
void lock_clock_hide(void);

/* Watch face (ui_watch_face.cpp) */
lv_obj_t *watch_face_create(lv_obj_t *parent);
void watch_face_update(void);
void watch_face_rebuild(void);   /* After the slot settings change */
/* What a slot shows, with the built-in default resolved. */
watch_slot_setting_t watch_face_slot_resolved(uint8_t index);

/* Face editor and lock screen pages (ui_watch_face_edit.cpp) */
uint32_t watch_app_name_hash(const char *name);
const ui_app_entry_t *watch_app_find_by_hash(uint32_t hash);
/* Apps in the app list's order (the person's saved order, then the rest);
 * returns how many it wrote to out. */
uint32_t watch_app_list_order(const ui_app_entry_t **out, uint32_t max);
void watch_face_editor_build(lv_obj_t *content);
void watch_lock_screen_build(lv_obj_t *content);
/* Open the face editor as an app; slot >= 0 also opens that slot's choices. */
void watch_face_editor_open(int slot);

/* Stopwatch, Timer and Alarm apps (ui_watch_clock.cpp: ui_stopwatch_main, …).
 * The engine runs while the apps are closed and rings over any screen. */
void watch_clock_init(void);
/* Milliseconds until the next timer or alarm is due, for the light-sleep wake-up;
 * 0 when nothing is due. */
uint32_t watch_clock_next_wake_ms(void);
/* Turns the screen back on if it is off (simulator overlay) and resets the idle timer. */
void watch_shell_wake(void);
/* Settings > Date & time (ui_clock_tools.cpp) */
void ui_clock_settings_build(lv_obj_t *content);

/* App list (ui_app_list.cpp) */
lv_obj_t *app_list_create(lv_obj_t *parent);
void app_list_update(void);
void app_list_scroll_to_top(void);

#endif /* FACTORY_WATCH_UI */
