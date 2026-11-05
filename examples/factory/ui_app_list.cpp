/**
 * @file      ui_app_list.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Vertical app list opened by swiping up on the watch face. Rows are sized so
 * four to five fit on screen at once; swiping right returns to the face.
 * Long-press a row and drag it to change the order, which is saved.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

#define APP_LIST_HEADER_H      40
#define APP_LIST_ROW_H         88
#define APP_LIST_ROW_GAP       8
#define APP_LIST_ICON_DISC     52
#define APP_LIST_ICON_SIZE     34
#define APP_LIST_PAD_HOR       UI_SCREEN_MARGIN

/* Drag to reorder: a long press lifts a row, it follows the finger, and a gap
 * shows where it will land; the list scrolls when the finger nears its top or
 * bottom. The order is saved by app name hash (hw_set_watch_app_order()), like
 * the face's corner shortcuts, so it survives power off and new apps. */
#define APP_LIST_DRAG_EDGE     70    /* Finger this close to the list's edge scrolls it */
#define APP_LIST_DRAG_STEP     12    /* px scrolled per input read */

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *scroll;
} list_ui;

static struct {
    lv_obj_t *row;            /* Lifted row, floating above the list */
    lv_obj_t *gap;            /* Placeholder where it will land */
    int32_t grab_dy;          /* Finger y minus the row's top when lifted */
    bool swallow_click;       /* The release after a drag must not open the app */
} drag;

static void app_list_save_order(void)
{
    uint32_t hashes[WATCH_APP_ORDER_MAX];
    uint8_t count = 0;
    uint32_t children = lv_obj_get_child_count(list_ui.scroll);
    for (uint32_t i = 0; i < children && count < WATCH_APP_ORDER_MAX; i++) {
        const ui_app_entry_t *entry =
            (const ui_app_entry_t *)lv_obj_get_user_data(lv_obj_get_child(list_ui.scroll, i));
        if (entry) hashes[count++] = watch_app_name_hash(entry->name);
    }
    hw_set_watch_app_order(hashes, count);
}

static void drag_place_row(lv_obj_t *row, int32_t finger_y)
{
    int32_t top = finger_y - drag.grab_dy;
    lv_obj_set_y(row, top - list_ui.scroll->coords.y1 -
                 lv_obj_get_style_pad_top(list_ui.scroll, LV_PART_MAIN));

    /* The gap goes before the first row whose centre is below the lifted one's. */
    int32_t centre = top + APP_LIST_ROW_H / 2;
    uint32_t target = 0;
    uint32_t children = lv_obj_get_child_count(list_ui.scroll);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *child = lv_obj_get_child(list_ui.scroll, i);
        if (child == row || child == drag.gap) continue;
        if ((child->coords.y1 + child->coords.y2) / 2 < centre) target++;
    }
    if (lv_obj_get_index(drag.gap) != (int32_t)target) lv_obj_move_to_index(drag.gap, target);
}

static void drag_lift(lv_obj_t *row, lv_indev_t *indev)
{
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    drag.row = row;
    drag.grab_dy = point.y - row->coords.y1;

    /* A gap the size of the row keeps the others in place. */
    drag.gap = lv_obj_create(list_ui.scroll);
    lv_obj_remove_style_all(drag.gap);
    lv_obj_set_size(drag.gap, LV_PCT(100), APP_LIST_ROW_H);
    lv_obj_set_style_radius(drag.gap, APP_LIST_ROW_H / 2, 0);
    lv_obj_set_style_border_width(drag.gap, 2, 0);
    lv_obj_set_style_border_color(drag.gap, UI_COLOR_DIVIDER, 0);
    lv_obj_remove_flag(drag.gap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(drag.gap, lv_obj_get_index(row));

    /* The row floats above the others (last child draws on top). It doesn't
     * pass vertical drags on, so the list doesn't scroll under the finger. */
    lv_obj_add_flag(row, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_move_to_index(row, -1);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_shadow_width(row, 24, 0);
    lv_obj_set_style_shadow_color(row, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, 0);
    drag_place_row(row, point.y);
    hw_feedback();
}

static void drag_drop(void)
{
    lv_obj_t *row = drag.row;
    int32_t index = lv_obj_get_index(drag.gap);
    lv_obj_delete(drag.gap);
    drag.gap = NULL;
    drag.row = NULL;

    lv_obj_remove_flag(row, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_move_to_index(row, index);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_BG, 0);
    lv_obj_remove_state(row, LV_STATE_PRESSED);
    app_list_save_order();
}

static void app_list_row_event_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_indev_t *indev = lv_indev_active();

    switch (lv_event_get_code(e)) {
    case LV_EVENT_LONG_PRESSED:
        if (!drag.row && indev) drag_lift(row, indev);
        break;
    case LV_EVENT_PRESSING: {
        if (drag.row != row || !indev) break;
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        /* Near the list's top or bottom edge, scroll it towards the finger. */
        lv_area_t area;
        lv_obj_get_coords(list_ui.scroll, &area);
        int32_t step = 0;
        if (point.y < area.y1 + APP_LIST_DRAG_EDGE) {
            step = LV_MIN(APP_LIST_DRAG_STEP, lv_obj_get_scroll_top(list_ui.scroll));
        } else if (point.y > area.y2 - APP_LIST_DRAG_EDGE) {
            step = -LV_MIN(APP_LIST_DRAG_STEP, lv_obj_get_scroll_bottom(list_ui.scroll));
        }
        if (step) {
            lv_obj_scroll_by(list_ui.scroll, 0, step, LV_ANIM_OFF);
            lv_obj_update_layout(list_ui.scroll);
        }
        drag_place_row(row, point.y);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (drag.row == row) {
            drag_drop();
            drag.swallow_click = true;
        }
        break;
    case LV_EVENT_CLICKED:
        if (drag.swallow_click) {
            drag.swallow_click = false;
            break;
        }
        watch_shell_launch((app_t *)((const ui_app_entry_t *)lv_obj_get_user_data(row))->app_func,
                           WATCH_VIEW_LIST);
        break;
    case LV_EVENT_PRESSED:
        drag.swallow_click = false;
        break;
    default:
        break;
    }
}

static void app_list_add_row(lv_obj_t *parent, const ui_app_entry_t *entry)
{
    /* Shared styles, so a theme change in Settings recolours the list too. */
    ui_styles_init();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), APP_LIST_ROW_H);
    lv_obj_add_style(row, &ui_styles.card, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, APP_LIST_ROW_H / 2, 0);
    lv_obj_set_style_pad_hor(row, (APP_LIST_ROW_H - APP_LIST_ICON_DISC) / 2, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(row, (void *)entry);
    lv_obj_add_event_cb(row, app_list_row_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *disc = lv_obj_create(row);
    lv_obj_set_size(disc, APP_LIST_ICON_DISC, APP_LIST_ICON_DISC);
    lv_obj_add_style(disc, &ui_styles.accent_btn, 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(disc, 0, 0);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);

    if (entry->icon) {
        lv_obj_t *icon = lv_image_create(disc);
        lv_image_set_src(icon, entry->icon);
        lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        uint32_t img_max = LV_MAX(entry->icon->header.w, entry->icon->header.h);
        if (img_max > 0) lv_image_set_scale(icon, APP_LIST_ICON_SIZE * 256 / img_max);
        lv_obj_set_size(icon, APP_LIST_ICON_SIZE, APP_LIST_ICON_SIZE);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);
    }

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, entry->name);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
}

lv_obj_t *app_list_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    list_ui.root = root;

    list_ui.time = lv_label_create(root);
    lv_obj_set_style_text_font(list_ui.time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(list_ui.time, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(list_ui.time, "--:--");
    lv_obj_align(list_ui.time, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *scroll = lv_obj_create(root);
    lv_obj_set_size(scroll, LV_PCT(100),
                    lv_display_get_vertical_resolution(NULL) - APP_LIST_HEADER_H);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, APP_LIST_HEADER_H);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_radius(scroll, 0, 0);
    lv_obj_set_style_pad_hor(scroll, APP_LIST_PAD_HOR, 0);
    lv_obj_set_style_pad_top(scroll, 4, 0);
    lv_obj_set_style_pad_bottom(scroll, 24, 0);
    lv_obj_set_style_pad_row(scroll, APP_LIST_ROW_GAP, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_style_bg_color(scroll, UI_COLOR_ACCENT, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, 2, LV_PART_SCROLLBAR);
    list_ui.scroll = scroll;

    /* Saved order first (apps that no longer exist are skipped), then any app
     * not in it, in registry order: new apps join at the end. */
    uint32_t count = ui_app_registry_count();
    bool placed[WATCH_APP_ORDER_MAX * 2] = {false};
    uint32_t order[WATCH_APP_ORDER_MAX];
    uint8_t saved = hw_get_watch_app_order(order, WATCH_APP_ORDER_MAX);
    for (uint8_t k = 0; k < saved; k++) {
        const ui_app_entry_t *entry = watch_app_find_by_hash(order[k]);
        if (!entry) continue;
        for (uint32_t i = 0; i < count && i < sizeof(placed); i++) {
            if (ui_app_registry_get(i) == entry && !placed[i]) {
                placed[i] = true;
                app_list_add_row(scroll, entry);
            }
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        if (i < sizeof(placed) && placed[i]) continue;
        app_list_add_row(scroll, ui_app_registry_get(i));
    }

    return root;
}

void app_list_update(void)
{
    if (!list_ui.time) return;
    struct tm timeinfo = {0};
    hw_get_date_time(timeinfo);
    lv_label_set_text_fmt(list_ui.time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
}

void app_list_scroll_to_top(void)
{
    if (drag.row) drag_drop();   /* The screen went off mid-drag */
    if (list_ui.scroll) {
        lv_obj_scroll_to_y(list_ui.scroll, 0, LV_ANIM_OFF);
    }
}

#endif /* FACTORY_WATCH_UI */
