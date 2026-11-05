/**
 * @file      ui_watch_shell.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Navigation for the smartwatch shell:
 *   watch face --swipe up--> app list --tap--> app
 *   back (swipe right) returns app -> list (or face, if launched from a face slot) -> face.
 * Idle time on the face or list turns the screen off; any wake source returns to the face.
 * With lock mode "Clock", idle first shows a clock page for one more timeout.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

#define WATCH_WAKE_GUARD_MS     500
#define WATCH_LIST_OPEN_MS      220
#define WATCH_LIST_CLOSE_MS     180
#define WATCH_UPDATE_PERIOD_MS  1000

bool get_enter_low_power_flag();

static lv_obj_t *home_tile;
static lv_obj_t *face;
static lv_obj_t *list;
static lv_obj_t *list_back_target;
static lv_timer_t *update_timer;
static watch_view_t view = WATCH_VIEW_FACE;
static watch_view_t app_return_view = WATCH_VIEW_LIST;
static uint32_t wake_tick;
static bool wake_guard;
#ifndef ARDUINO
static lv_obj_t *screen_off_overlay;
#endif

static int32_t screen_width(void)
{
    return lv_display_get_horizontal_resolution(NULL);
}

static int32_t screen_height(void)
{
    return lv_display_get_vertical_resolution(NULL);
}

static void anim_set_x(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void anim_set_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void list_reset_hidden(void)
{
    lv_anim_delete(list, NULL);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(list, 0, 0);
}

static void list_animate(lv_anim_exec_xcb_t exec_cb, int32_t from, int32_t to,
                         uint32_t duration, lv_anim_completed_cb_t ready_cb)
{
    lv_anim_delete(list, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, list);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, exec_cb);
    if (ready_cb) {
        lv_anim_set_completed_cb(&a, ready_cb);
    }
    lv_anim_start(&a);
}

static void list_hidden_ready_cb(lv_anim_t *a)
{
    LV_UNUSED(a);
    list_reset_hidden();
    /* Next time the list opens from the face it starts at the top. */
    app_list_scroll_to_top();
}

watch_view_t watch_shell_get_view(void)
{
    return view;
}

bool watch_shell_input_guard_active(void)
{
    if (!wake_guard) return false;
    if (lv_tick_elaps(wake_tick) >= WATCH_WAKE_GUARD_MS) {
        wake_guard = false;
        return false;
    }
    return true;
}

void watch_shell_show_face(bool anim)
{
    view = WATCH_VIEW_FACE;
    watch_face_update();
    if (!anim || lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN)) {
        list_reset_hidden();
        return;
    }
    /* Back is a rightward swipe, so the list leaves to the right. */
    list_animate(anim_set_x, lv_obj_get_x(list), screen_width(),
                 WATCH_LIST_CLOSE_MS, list_hidden_ready_cb);
}

void watch_shell_list_drag(int32_t dy)
{
    if (view != WATCH_VIEW_FACE) return;
    int32_t height = screen_height();
    dy = LV_CLAMP(0, dy, height);
    lv_anim_delete(list, NULL);
    if (lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN)) {
        app_list_update();
        lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_pos(list, 0, height - dy);
}

void watch_shell_list_drag_end(bool open)
{
    if (view != WATCH_VIEW_FACE || lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN)) return;
    if (open) {
        view = WATCH_VIEW_LIST;
        list_animate(anim_set_y, lv_obj_get_y(list), 0, WATCH_LIST_OPEN_MS, NULL);
    } else {
        list_animate(anim_set_y, lv_obj_get_y(list), screen_height(),
                     WATCH_LIST_CLOSE_MS, list_hidden_ready_cb);
    }
}

void watch_shell_launch(app_t *app, watch_view_t return_view)
{
    if (!app || watch_shell_input_guard_active()) return;
    app_return_view = return_view;
    view = WATCH_VIEW_APP;
    ui_launch_app(app);
}

void watch_shell_on_app_exit(void)
{
    if (view != WATCH_VIEW_APP) return;
    if (app_return_view == WATCH_VIEW_FACE) {
        list_reset_hidden();
        view = WATCH_VIEW_FACE;
        watch_face_update();
    } else {
        view = WATCH_VIEW_LIST;
        app_list_update();
    }
}

static void lock_show(void)
{
    list_reset_hidden();
    app_list_scroll_to_top();
    view = WATCH_VIEW_LOCK;
    lock_clock_show();
    /* The screen turns off after another full timeout. */
    lv_display_trigger_activity(NULL);
}

void watch_shell_unlock(void)
{
    if (view != WATCH_VIEW_LOCK) return;
    lock_clock_hide();
    view = WATCH_VIEW_FACE;
    watch_face_update();
    lv_display_trigger_activity(NULL);
}

static void screen_wake(uint8_t brightness)
{
    lock_clock_hide();
    list_reset_hidden();
    app_list_scroll_to_top();
    view = WATCH_VIEW_FACE;
    watch_face_update();
    lv_refr_now(NULL);
    hw_inc_brightness(brightness);
    hw_enable_feedback();
    /* The touch that woke the watch must not also press a face slot. */
    wake_tick = lv_tick_get();
    wake_guard = true;
    lv_indev_reset(NULL, NULL);
    lv_display_trigger_activity(NULL);
}

#ifndef ARDUINO
/* Desktop has no light sleep, so show the "off" screen until a click. */
static uint8_t screen_off_brightness;

static void screen_off_overlay_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_obj_delete_async(screen_off_overlay);
    screen_off_overlay = NULL;
    screen_wake(screen_off_brightness);
}
#endif

static void screen_off(void)
{
    uint8_t brightness = hw_get_disp_backlight();
    hw_disable_feedback();
#ifdef ARDUINO
    hw_dec_brightness(0);
    /* A due alarm or timer wakes the watch so it can ring. */
    hw_low_power_loop(watch_clock_next_wake_ms());
#ifdef NO_ENTER_LIGHT_SLEEP
    pinMode(0, INPUT_PULLUP);
    while (digitalRead(0) == HIGH) {
        delay(10);
    }
#endif
    screen_wake(brightness);
#else
    screen_off_brightness = brightness;
    screen_off_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(screen_off_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(screen_off_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_off_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen_off_overlay, 0, 0);
    lv_obj_set_style_radius(screen_off_overlay, 0, 0);
    lv_obj_remove_flag(screen_off_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen_off_overlay, screen_off_overlay_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(screen_off_overlay);
    lv_label_set_text(hint, "Screen off (simulator) - click to wake");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
#endif
}

void watch_shell_wake(void)
{
#ifndef ARDUINO
    if (screen_off_overlay) {
        lv_obj_delete(screen_off_overlay);
        screen_off_overlay = NULL;
        screen_wake(screen_off_brightness);
        return;
    }
#endif
    /* On hardware the screen is already on: screen_off() returns only after waking. */
    lv_display_trigger_activity(NULL);
}

void watch_shell_poll(void)
{
#ifndef ARDUINO
    if (screen_off_overlay) return;
#endif
    if (view == WATCH_VIEW_APP) return;

    /* Display timeout setting: 0 keeps the screen on. */
    uint32_t timeout = hw_get_disp_timeout_ms();
    if (timeout == 0 || !get_enter_low_power_flag()) return;
    if (lv_display_get_inactive_time(NULL) < timeout) return;
    if (view != WATCH_VIEW_LOCK && hw_get_watch_lock_mode() == WATCH_LOCK_CLOCK) {
        lock_show();
        return;
    }
    screen_off();
}

static void update_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (view == WATCH_VIEW_FACE) {
        watch_face_update();
    } else if (view == WATCH_VIEW_LIST) {
        app_list_update();
    }
}

static void list_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (view == WATCH_VIEW_LIST) {
        watch_shell_show_face(true);
    }
}

void watch_shell_setup(lv_obj_t *tile)
{
    home_tile = tile;
    lv_obj_remove_flag(home_tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(home_tile, 0, 0);

    face = watch_face_create(home_tile);

    list = app_list_create(home_tile);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);

    /* Swipe right on the list goes back to the face through this hidden target. */
    list_back_target = lv_obj_create(list);
    lv_obj_add_flag(list_back_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(list_back_target, list_back_cb, LV_EVENT_CLICKED, NULL);
    ui_enable_edge_swipe_back(list, list_back_target);

    update_timer = lv_timer_create(update_timer_cb, WATCH_UPDATE_PERIOD_MS, NULL);
    view = WATCH_VIEW_FACE;
    watch_face_update();
    watch_clock_init();
}

#endif /* FACTORY_WATCH_UI */
