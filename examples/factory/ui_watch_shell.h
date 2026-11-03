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

/* Watch face (ui_watch_face.cpp) */
lv_obj_t *watch_face_create(lv_obj_t *parent);
void watch_face_update(void);

/* App list (ui_app_list.cpp) */
lv_obj_t *app_list_create(lv_obj_t *parent);
void app_list_update(void);
void app_list_scroll_to_top(void);

#endif /* FACTORY_WATCH_UI */
