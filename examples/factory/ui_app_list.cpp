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
#define APP_LIST_CHIP_BAR_H    48    /* Category filter chips, under the clock */
#define APP_LIST_ROW_H         88
#define APP_LIST_ROW_GAP       8
#define APP_LIST_ICON_DISC     52
#define APP_LIST_ICON_SIZE     34
#define APP_LIST_PAD_HOR       UI_SCREEN_MARGIN

/* "All" plus the categories ui_app_category_for() can return, in chip order.
 * A chip appears only when at least one registered app has that category. */
#define APP_CATEGORY_ALL       "All"
static const char *const APP_CATEGORY_ORDER[] = {
    APP_CATEGORY_ALL, "WiFi", "Bluetooth", "RF", "IR", "NFC", "USB",
    "GNSS", "Sensors", "Media", "Clock", "Security", "Tools",
};

/* Drag to reorder: a long press lifts a row, it follows the finger, and a gap
 * shows where it will land; the list scrolls when the finger nears its top or
 * bottom. The order is saved by app name hash (hw_set_watch_app_order()), like
 * the face's corner shortcuts, so it survives power off and new apps. */
#define APP_LIST_DRAG_EDGE     70    /* Finger this close to the list's edge scrolls it */
#define APP_LIST_DRAG_STEP     12    /* px scrolled per input read */

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *chips;      /* Horizontal category filter bar */
    lv_obj_t *scroll;
} list_ui;

/* The selected filter chip; a pointer from APP_CATEGORY_ORDER (never freed). */
static const char *active_filter = APP_CATEGORY_ALL;

#define APP_LIST_HANDLE_W      110   /* Arrange mode: the row's right end that drags */

static struct {
    lv_obj_t *arrange_row;    /* Last row: "Arrange apps" / "Done" */
    lv_obj_t *arrange_label;
    bool arranging;           /* Handles shown; dragging a handle moves its row */
} arrange;

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
        if (child == row || child == drag.gap || !lv_obj_get_user_data(child)) continue;
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
        if (arrange.arranging) {
            ui_toast("Drag the handle to move");
            break;
        }
        watch_shell_launch((app_t *)((const ui_app_entry_t *)lv_obj_get_user_data(row))->app_func,
                           WATCH_VIEW_LIST);
        break;
    case LV_EVENT_PRESSED:
        drag.swallow_click = false;
        /* Arrange mode: the handle lifts the row at once; the rest scrolls. */
        if (arrange.arranging && !drag.row && indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            if (point.x >= row->coords.x2 - APP_LIST_HANDLE_W) drag_lift(row, indev);
        }
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

    /* Drag handle, shown in arrange mode (the row's last child) */
    lv_obj_t *handle = lv_label_create(row);
    lv_label_set_text(handle, LV_SYMBOL_BARS);
    lv_obj_set_style_text_font(handle, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(handle, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_pad_hor(handle, 12, 0);
    lv_obj_add_flag(handle, LV_OBJ_FLAG_HIDDEN);
    if (arrange.arranging) lv_obj_remove_flag(handle, LV_OBJ_FLAG_HIDDEN);
}

static void app_list_set_arranging(bool on)
{
    if (!list_ui.scroll || !arrange.arrange_row) return;
    arrange.arranging = on;
    uint32_t children = lv_obj_get_child_count(list_ui.scroll);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *row = lv_obj_get_child(list_ui.scroll, i);
        if (!lv_obj_get_user_data(row)) continue;
        lv_obj_t *handle = lv_obj_get_child(row, -1);
        if (on) lv_obj_remove_flag(handle, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(handle, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(arrange.arrange_label, on ? "Done" : "Arrange apps");
    lv_obj_set_style_bg_color(arrange.arrange_row, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_text_color(arrange.arrange_label, on ? lv_color_black() : UI_COLOR_TEXT_PRIMARY, 0);
}

static void arrange_row_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    app_list_set_arranging(!arrange.arranging);
    hw_feedback();
}

/* The visible way into reordering (a long press is a shortcut, never the only
 * way: watch-ux-guidelines.md 2.4). */
static void app_list_add_arrange_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), APP_LIST_ROW_H);
    lv_obj_add_style(row, &ui_styles.card, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, APP_LIST_ROW_H / 2, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, arrange_row_cb, LV_EVENT_CLICKED, NULL);
    arrange.arrange_label = lv_label_create(row);
    lv_obj_set_style_text_font(arrange.arrange_label, &lv_font_montserrat_28, 0);
    lv_obj_center(arrange.arrange_label);
    arrange.arrange_row = row;
    app_list_set_arranging(false);
}

uint32_t watch_app_list_order(const ui_app_entry_t **out, uint32_t max)
{
    /* Saved order first (apps that no longer exist are skipped), then any app
     * not in it, in registry order: new apps join at the end. */
    uint32_t count = ui_app_registry_count();
    uint32_t n = 0;
    bool placed[WATCH_APP_ORDER_MAX * 2] = {false};
    uint32_t order[WATCH_APP_ORDER_MAX];
    uint8_t saved = hw_get_watch_app_order(order, WATCH_APP_ORDER_MAX);
    for (uint8_t k = 0; k < saved && n < max; k++) {
        const ui_app_entry_t *entry = watch_app_find_by_hash(order[k]);
        for (uint32_t i = 0; entry && i < count && i < sizeof(placed); i++) {
            if (ui_app_registry_get(i) == entry && !placed[i]) {
                placed[i] = true;
                out[n++] = entry;
            }
        }
    }
    for (uint32_t i = 0; i < count && n < max; i++) {
        if (i < sizeof(placed) && placed[i]) continue;
        out[n++] = ui_app_registry_get(i);
    }
    return n;
}

static void chip_set_selected(lv_obj_t *chip, bool on)
{
    lv_obj_set_style_bg_color(chip, on ? UI_COLOR_ACCENT : UI_COLOR_CARD_BG, 0);
    lv_obj_t *label = lv_obj_get_child(chip, 0);
    if (label) {
        lv_obj_set_style_text_color(label, on ? lv_color_black() : UI_COLOR_TEXT_SECONDARY, 0);
    }
}

/* Show only the rows in the active category (all rows under "All"); the
 * "Arrange apps" row and reordering are offered only under "All". */
static void app_list_apply_filter(void)
{
    if (!list_ui.scroll) return;
    bool all = strcmp(active_filter, APP_CATEGORY_ALL) == 0;
    uint32_t children = lv_obj_get_child_count(list_ui.scroll);
    for (uint32_t i = 0; i < children; i++) {
        lv_obj_t *row = lv_obj_get_child(list_ui.scroll, i);
        const ui_app_entry_t *entry = (const ui_app_entry_t *)lv_obj_get_user_data(row);
        bool show = entry ? (all || (entry->category && strcmp(entry->category, active_filter) == 0))
                          : all;   /* The Arrange row has no entry: All only */
        if (show) lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    if (list_ui.chips) {
        uint32_t n = lv_obj_get_child_count(list_ui.chips);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *chip = lv_obj_get_child(list_ui.chips, i);
            const char *cat = (const char *)lv_obj_get_user_data(chip);
            chip_set_selected(chip, cat && strcmp(cat, active_filter) == 0);
        }
    }
    lv_obj_scroll_to_y(list_ui.scroll, 0, LV_ANIM_OFF);
}

static void chip_event_cb(lv_event_t *e)
{
    lv_obj_t *chip = lv_event_get_current_target_obj(e);
    const char *cat = (const char *)lv_obj_get_user_data(chip);
    if (!cat || cat == active_filter) return;
    if (arrange.arranging) app_list_set_arranging(false);
    active_filter = cat;
    app_list_apply_filter();
    hw_feedback();
}

/* A chip per category that at least one app uses, in APP_CATEGORY_ORDER order,
 * "All" always first. */
static void app_list_build_chips(lv_obj_t *bar)
{
    const size_t total = sizeof(APP_CATEGORY_ORDER) / sizeof(APP_CATEGORY_ORDER[0]);
    bool present[sizeof(APP_CATEGORY_ORDER) / sizeof(APP_CATEGORY_ORDER[0])] = {false};
    uint32_t count = ui_app_registry_count();
    for (uint32_t i = 0; i < count; i++) {
        const ui_app_entry_t *entry = ui_app_registry_get(i);
        if (!entry || !entry->category) continue;
        for (size_t c = 1; c < total; c++) {
            if (strcmp(entry->category, APP_CATEGORY_ORDER[c]) == 0) { present[c] = true; break; }
        }
    }
    for (size_t c = 0; c < total; c++) {
        if (c != 0 && !present[c]) continue;
        lv_obj_t *chip = lv_obj_create(bar);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, LV_PCT(100));
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(chip, 18, 0);
        lv_obj_set_style_pad_ver(chip, 0, 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(chip, (void *)APP_CATEGORY_ORDER[c]);
        lv_obj_add_event_cb(chip, chip_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *label = lv_label_create(chip);
        lv_label_set_text(label, APP_CATEGORY_ORDER[c]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_center(label);
        chip_set_selected(chip, c == 0);
    }
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

    /* Category filter chips, scrolling sideways under the clock. A horizontally
     * scrollable object is exempt from the swipe-right-to-go-back gesture
     * (ui_navigation.cpp), so the bar and the back gesture don't fight. */
    lv_obj_t *chips = lv_obj_create(root);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, LV_PCT(100), APP_LIST_CHIP_BAR_H);
    lv_obj_align(chips, LV_ALIGN_TOP_MID, 0, APP_LIST_HEADER_H);
    lv_obj_set_style_pad_hor(chips, APP_LIST_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(chips, 3, 0);
    lv_obj_set_style_pad_column(chips, 8, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    list_ui.chips = chips;
    app_list_build_chips(chips);

    lv_obj_t *scroll = lv_obj_create(root);
    lv_obj_set_size(scroll, LV_PCT(100),
                    lv_display_get_vertical_resolution(NULL) - APP_LIST_HEADER_H - APP_LIST_CHIP_BAR_H);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, APP_LIST_HEADER_H + APP_LIST_CHIP_BAR_H);
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

    const ui_app_entry_t *ordered[WATCH_APP_ORDER_MAX * 2];
    uint32_t count = watch_app_list_order(ordered, sizeof(ordered) / sizeof(ordered[0]));
    for (uint32_t i = 0; i < count; i++) app_list_add_row(scroll, ordered[i]);
    app_list_add_arrange_row(scroll);

    app_list_apply_filter();

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
    app_list_set_arranging(false);
    /* Reopen on the full list, chips scrolled back to "All". */
    active_filter = APP_CATEGORY_ALL;
    if (list_ui.chips) lv_obj_scroll_to_x(list_ui.chips, 0, LV_ANIM_OFF);
    app_list_apply_filter();
    if (list_ui.scroll) {
        lv_obj_scroll_to_y(list_ui.scroll, 0, LV_ANIM_OFF);
    }
}

#endif /* FACTORY_WATCH_UI */
