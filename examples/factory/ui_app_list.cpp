/**
 * @file      ui_app_list.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Vertical app list opened by swiping up on the watch face. Rows are sized so
 * four to five fit on screen at once; swiping right returns to the face.
 */
#include "ui_define.h"
#include "ui_watch_shell.h"

#if FACTORY_WATCH_UI

#define APP_LIST_HEADER_H      40
#define APP_LIST_ROW_H         72
#define APP_LIST_ROW_GAP       8
#define APP_LIST_ICON_DISC     52
#define APP_LIST_ICON_SIZE     34
#define APP_LIST_PAD_HOR       24   /* Keeps rows clear of the rounded screen corners */

static struct {
    lv_obj_t *root;
    lv_obj_t *time;
    lv_obj_t *scroll;
} list_ui;

static void app_list_row_click_cb(lv_event_t *e)
{
    app_t *app = (app_t *)lv_event_get_user_data(e);
    watch_shell_launch(app, WATCH_VIEW_LIST);
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
    lv_obj_add_event_cb(row, app_list_row_click_cb, LV_EVENT_CLICKED, entry->app_func);

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
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
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

    uint32_t count = ui_app_registry_count();
    for (uint32_t i = 0; i < count; i++) {
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
    if (list_ui.scroll) {
        lv_obj_scroll_to_y(list_ui.scroll, 0, LV_ANIM_OFF);
    }
}

#endif /* FACTORY_WATCH_UI */
