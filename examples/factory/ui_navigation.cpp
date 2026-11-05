/**
 * @file      ui_navigation.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-25
 * 
 */
#include "ui_define.h"

#if FACTORY_HAS_TOUCH_INPUT

static const int32_t EDGE_WIDTH_PX = 20;
static const uint32_t SWIPE_TIMEOUT_MS = 1000;
static const int32_t INDICATOR_MIN_DIAMETER_PX = 64;
static const int32_t INDICATOR_MAX_DIAMETER_PX = 88;
static const int32_t INDICATOR_MARGIN_PX = 8;
static const int32_t INDICATOR_ICON_Y_OFFSET_PX = 2;
static const int32_t INDICATOR_START_DISTANCE_PX = 10;
static const int32_t INDICATOR_VISIBLE_DIVISOR = 3;
static const uint32_t INDICATOR_HIDE_MS = 140;

struct edge_swipe_t {
    lv_obj_t *root;
    lv_obj_t *back_target;
    lv_event_code_t back_event;
    lv_indev_t *pointer;
    lv_point_t start;
    uint32_t started_at;
    int32_t min_distance;
    int direction;
    lv_obj_t *indicator;
    lv_obj_t *indicator_arrow;
    lv_timer_t *tracking_timer;
    bool threshold_feedback_sent;
#if FACTORY_WATCH_UI
    int32_t root_x0;        /* The page's x when the finger went down */
    bool following;         /* The page is moving with the finger */
    bool parent_scrollable; /* The parent's SCROLLABLE flag, off while following */
#endif
};

static void edge_swipe_delete_cb(lv_event_t *e);

static bool edge_swipe_contains(lv_obj_t *root, lv_obj_t *obj)
{
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == root) return true;
    }
    return false;
}

static bool edge_swipe_is_root(lv_obj_t *obj)
{
    uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_event_dsc_t *dsc = lv_obj_get_event_dsc(obj, i);
        if (dsc && lv_event_dsc_get_cb(dsc) == edge_swipe_delete_cb) return true;
    }
    return false;
}

#if FACTORY_WATCH_UI
/* The watch back swipe may start anywhere, so leave horizontal drags to controls that use them. */
static bool edge_swipe_starts_on_horizontal_control(lv_obj_t *root, lv_obj_t *obj)
{
    for (; obj && obj != root; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, UI_FLAG_SWIPE_BACK_OK)) return false;
        if (lv_obj_check_type(obj, &lv_slider_class) ||
                lv_obj_check_type(obj, &lv_textarea_class) ||
                lv_obj_has_class(obj, &lv_buttonmatrix_class)) {
            return true;
        }
        if (lv_obj_check_type(obj, &lv_arc_class) && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
            return true;
        }
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) &&
                (lv_obj_get_scroll_dir(obj) & LV_DIR_HOR) &&
                (lv_obj_get_scroll_left(obj) > 0 || lv_obj_get_scroll_right(obj) > 0)) {
            return true;
        }
    }
    return false;
}
#endif

static void edge_swipe_indicator_x_anim(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void edge_swipe_stop_tracking(edge_swipe_t *state)
{
    if (state->tracking_timer) {
        lv_timer_delete(state->tracking_timer);
        state->tracking_timer = NULL;
    }
}

static void edge_swipe_hide_indicator(edge_swipe_t *state, bool accepted)
{
    if (!state->indicator) return;

    lv_obj_t *indicator = state->indicator;
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    int32_t current_x = lv_obj_get_x(indicator);
    int32_t target_x = current_x + state->direction * 8;
    if (!accepted) {
        int32_t diameter = lv_obj_get_width(indicator);
        target_x = state->direction > 0 ? -diameter : width;
    }

    state->indicator = NULL;
    state->indicator_arrow = NULL;

    lv_anim_t indicator_anim;
    lv_anim_init(&indicator_anim);
    lv_anim_set_var(&indicator_anim, indicator);
    lv_anim_set_values(&indicator_anim, current_x, target_x);
    lv_anim_set_duration(&indicator_anim, INDICATOR_HIDE_MS);
    lv_anim_set_path_cb(&indicator_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&indicator_anim, edge_swipe_indicator_x_anim);
    lv_anim_start(&indicator_anim);
    lv_obj_fade_out(indicator, INDICATOR_HIDE_MS, 0);
    lv_obj_delete_delayed(indicator, INDICATOR_HIDE_MS + 20);
}

static void edge_swipe_create_indicator(edge_swipe_t *state, lv_display_t *display)
{
    if (state->indicator) lv_obj_delete(state->indicator);

    lv_obj_t *layer = lv_display_get_layer_top(display);
    state->indicator = lv_obj_create(layer);
    if (!state->indicator) return;
    lv_obj_set_size(state->indicator, INDICATOR_MIN_DIAMETER_PX,
                    INDICATOR_MIN_DIAMETER_PX);
    lv_obj_set_style_bg_color(state->indicator, UI_COLOR_ACCENT_DIM, 0);
    lv_obj_set_style_bg_opa(state->indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state->indicator, 0, 0);
    lv_obj_set_style_radius(state->indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(state->indicator, 0, 0);
    lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_CLICKABLE);

    state->indicator_arrow = lv_label_create(state->indicator);
    if (!state->indicator_arrow) {
        lv_obj_delete(state->indicator);
        state->indicator = NULL;
        return;
    }
    lv_label_set_text(state->indicator_arrow,
                      state->direction > 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(state->indicator_arrow, lv_color_white(), 0);
    lv_obj_set_style_text_font(state->indicator_arrow, &lv_font_montserrat_22, 0);
    lv_obj_center(state->indicator_arrow);
}

static bool edge_swipe_update_indicator(edge_swipe_t *state, lv_indev_t *indev,
        const lv_point_t &point)
{
    int32_t inward = (point.x - state->start.x) * state->direction;
    int32_t vertical = LV_ABS(point.y - state->start.y);
    inward = LV_MAX(inward, 0);

    int32_t progress = LV_CLAMP(0, inward * 256 / state->min_distance, 256);
    bool threshold_reached = progress == 256 && inward >= vertical * 2;
    if (threshold_reached && !state->threshold_feedback_sent) {
        state->threshold_feedback_sent = true;
        hw_feedback();
    }

#if FACTORY_WATCH_UI
    /* The page follows the finger (watch-ux-guidelines.md 10.1) once the drag is
     * clearly sideways; a vertical scroll never moves it. */
    if (!state->following && inward >= INDICATOR_START_DISTANCE_PX && inward > vertical * 2) {
        state->following = true;
        /* Moving the page past the edge makes its parent scrollable sideways,
         * and the same drag would scroll it too. */
        lv_obj_t *parent = lv_obj_get_parent(state->root);
        state->parent_scrollable = parent && lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        if (state->parent_scrollable) lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        /* A slow swipe must not turn into a long press (which would cancel it,
         * or lift an app list row); restored when the finger lets go. */
        lv_indev_set_long_press_time(indev, 0xFFFF);
    }
    if (state->following) {
        lv_obj_set_x(state->root, state->root_x0 + inward);
    }
    return threshold_reached;
#endif

    if (!state->indicator) return threshold_reached;

    lv_display_t *display = lv_indev_get_display(indev);
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    bool show = inward >= INDICATOR_START_DISTANCE_PX;
#if FACTORY_WATCH_UI
    /* Swipes start anywhere, so don't flash the indicator while scrolling a list. */
    show = show && inward > vertical;
#endif
    if (!show) {
        lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        return threshold_reached;
    }

    int32_t diameter = INDICATOR_MIN_DIAMETER_PX +
                       progress * (INDICATOR_MAX_DIAMETER_PX -
                                   INDICATOR_MIN_DIAMETER_PX) / 256;
    int32_t visible_width = diameter * progress / INDICATOR_VISIBLE_DIVISOR / 256;
    int32_t x = state->direction > 0 ? -(diameter - visible_width) :
                width - visible_width;
    int32_t y = LV_CLAMP(INDICATOR_MARGIN_PX,
                         point.y - diameter / 2,
                         height - diameter - INDICATOR_MARGIN_PX);

    lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(state->indicator, -1);
    lv_obj_set_size(state->indicator, diameter, diameter);
    lv_obj_set_pos(state->indicator, x, y);
    lv_obj_set_style_opa(state->indicator, progress * LV_OPA_90 / 256, 0);
    lv_obj_set_style_bg_opa(state->indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state->indicator,
                              threshold_reached ? UI_COLOR_ACCENT : UI_COLOR_ACCENT_DIM, 0);
    if (state->indicator_arrow) {
        lv_obj_update_layout(state->indicator_arrow);
        int32_t icon_offset = state->direction > 0 ? (diameter - visible_width) / 2 :
                              -(diameter - visible_width) / 2;
        lv_obj_set_style_transform_rotation(state->indicator_arrow, 0, 0);
        lv_obj_align(state->indicator_arrow, LV_ALIGN_CENTER,
                     icon_offset, INDICATOR_ICON_Y_OFFSET_PX);
    }
    return threshold_reached;
}

#if FACTORY_WATCH_UI
static const uint32_t FOLLOW_SETTLE_MS = 160;
static const uint16_t LONG_PRESS_DEFAULT_MS = 400;   /* LV_INDEV_DEF_LONG_PRESS_TIME */

static void follow_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void follow_out_done_cb(lv_anim_t *a)
{
    edge_swipe_t *state = (edge_swipe_t *)lv_anim_get_user_data(a);
    if (!lv_obj_is_valid(state->back_target)) return;
    lv_obj_send_event(state->back_target, state->back_event, NULL);
}

/* Let go: the page slides back, or out and then goes back. */
static void follow_settle(edge_swipe_t *state, bool accepted, lv_indev_t *indev)
{
    if (!state->following) return;
    state->following = false;
    lv_obj_t *parent = lv_obj_get_parent(state->root);
    if (parent) {
        lv_obj_scroll_to_x(parent, 0, LV_ANIM_OFF);
        if (state->parent_scrollable) lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    }
    /* The app never changes it, so back to LVGL's default (private to lv_indev.c). */
    if (indev) lv_indev_set_long_press_time(indev, LONG_PRESS_DEFAULT_MS);
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, state->root);
    lv_anim_set_values(&a, lv_obj_get_x(state->root), accepted ? width : state->root_x0);
    lv_anim_set_duration(&a, FOLLOW_SETTLE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, follow_set_x);
    if (accepted) {
        lv_anim_set_user_data(&a, state);
        lv_anim_set_completed_cb(&a, follow_out_done_cb);
    }
    lv_anim_start(&a);
}
#endif

static void edge_swipe_cancel(edge_swipe_t *state, lv_indev_t *indev)
{
    if (state->pointer != indev) return;
    state->pointer = NULL;
    edge_swipe_stop_tracking(state);
    edge_swipe_hide_indicator(state, false);
#if FACTORY_WATCH_UI
    follow_settle(state, false, indev);
#endif
}

static void edge_swipe_tracking_timer_cb(lv_timer_t *timer)
{
    edge_swipe_t *state = (edge_swipe_t *)lv_timer_get_user_data(timer);
    if (!state->pointer) {
        edge_swipe_stop_tracking(state);
        return;
    }
    bool timed_out = lv_tick_elaps(state->started_at) > SWIPE_TIMEOUT_MS;
    bool gone = !lv_obj_is_visible(state->root);
#if FACTORY_WATCH_UI
    /* A slow swipe still goes back, and a page moving with the finger is
     * meant to leave the screen. */
    timed_out = timed_out && !state->following;
    gone = gone && !state->following;
#endif
    if (timed_out || gone) {
        edge_swipe_cancel(state, state->pointer);
        return;
    }

    lv_point_t point;
    lv_indev_get_point(state->pointer, &point);
    edge_swipe_update_indicator(state, state->pointer, point);
}

static void edge_swipe_input_cb(lv_event_t *e)
{
    edge_swipe_t *state = (edge_swipe_t *)lv_event_get_user_data(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_DELETE || code == LV_EVENT_INDEV_RESET || code == LV_EVENT_LONG_PRESSED) {
        edge_swipe_cancel(state, indev);
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;

    lv_obj_t *target = (lv_obj_t *)lv_event_get_param(e);
    if (code == LV_EVENT_PRESSED) {
        edge_swipe_stop_tracking(state);
        state->pointer = NULL;
        /* Pages can be stacked (ui_push_page), so only the innermost one under the finger goes back. */
        if (!lv_obj_is_visible(state->root) || ui_swipe_back_root(target) != state->root) return;

        lv_point_t point;
        lv_indev_get_point(indev, &point);
        int32_t width = lv_display_get_horizontal_resolution(lv_indev_get_display(indev));
        if (point.x < 0 || point.x >= width) return;
#if FACTORY_WATCH_UI
        /* Watch: back is a left-to-right swipe started anywhere. */
        if (edge_swipe_starts_on_horizontal_control(state->root, target)) return;
        state->direction = 1;
#else
        if (point.x < EDGE_WIDTH_PX) state->direction = 1;
        else if (point.x >= width - EDGE_WIDTH_PX) state->direction = -1;
        else return;
#endif

        state->pointer = indev;
        state->start = point;
        state->started_at = lv_tick_get();
        state->min_distance = LV_CLAMP(40, width / 5, 80);
        state->threshold_feedback_sent = false;
#if FACTORY_WATCH_UI
        /* The page itself moves, so no bubble indicator. */
        lv_anim_delete(state->root, follow_set_x);
        state->root_x0 = lv_obj_get_x(state->root);
        state->following = false;
#else
        edge_swipe_create_indicator(state, lv_indev_get_display(indev));
#endif
        edge_swipe_update_indicator(state, indev, point);
        // Input-device callbacks do not receive PRESSING events; poll only during this gesture.
        state->tracking_timer = lv_timer_create(edge_swipe_tracking_timer_cb, 16, state);
        return;
    }

    if (state->pointer != indev) return;
    bool too_slow = lv_tick_elaps(state->started_at) > SWIPE_TIMEOUT_MS;
    bool gone = !lv_obj_is_visible(state->root);
#if FACTORY_WATCH_UI
    too_slow = too_slow && !state->following;
    gone = gone && !state->following;
#endif
    if (too_slow || gone || !edge_swipe_contains(state->root, target) ||
            !lv_obj_is_valid(state->back_target) || lv_obj_has_state(state->back_target, LV_STATE_DISABLED)) {
        edge_swipe_cancel(state, indev);
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    bool accepted = edge_swipe_update_indicator(state, indev, point);
    state->pointer = NULL;
    edge_swipe_stop_tracking(state);
    edge_swipe_hide_indicator(state, accepted);
#if FACTORY_WATCH_UI
    if (state->following) {
        /* Slide out from where the finger let go and then go back, or slide
         * back into place. Either way this release clicks nothing. */
        lv_indev_reset(indev, NULL);
        follow_settle(state, accepted, indev);
        return;
    }
#endif
    if (!accepted) return;

    // Reset before navigation: the handler may delete the page, and this release must not click a widget.
    lv_obj_t *back_target = state->back_target;
    lv_event_code_t back_event = state->back_event;
    lv_indev_reset(indev, NULL);
    lv_obj_send_event(back_target, back_event, indev);
}

static void edge_swipe_delete_cb(lv_event_t *e)
{
    edge_swipe_t *state = (edge_swipe_t *)lv_event_get_user_data(e);
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_indev_remove_event_cb_with_user_data(indev, edge_swipe_input_cb, state);
    }
    edge_swipe_stop_tracking(state);
    if (state->indicator) {
        lv_obj_delete(state->indicator);
        state->indicator = NULL;
    }
    lv_free(state);
}

#endif

void ui_enable_edge_swipe_back(lv_obj_t *root, lv_obj_t *back_target, lv_event_code_t back_event)
{
#if FACTORY_HAS_TOUCH_INPUT
    if (!root || !back_target) return;
    edge_swipe_t *state = (edge_swipe_t *)lv_malloc_zeroed(sizeof(edge_swipe_t));
    if (!state) return;
    state->root = root;
    state->back_target = back_target;
    state->back_event = back_event;
    lv_obj_add_event_cb(root, edge_swipe_delete_cb, LV_EVENT_DELETE, state);

    // Input-device events also cover scrollable children without changing their event bubbling flags.
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(indev, edge_swipe_input_cb, LV_EVENT_ALL, state);
        }
    }
#else
    LV_UNUSED(root);
    LV_UNUSED(back_target);
    LV_UNUSED(back_event);
#endif
}

lv_obj_t *ui_swipe_back_root(lv_obj_t *obj)
{
#if FACTORY_HAS_TOUCH_INPUT
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (edge_swipe_is_root(obj)) return obj;
    }
#else
    LV_UNUSED(obj);
#endif
    return NULL;
}
