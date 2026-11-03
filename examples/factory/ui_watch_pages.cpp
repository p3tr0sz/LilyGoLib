/**
 * @file      ui_watch_pages.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Nested pages for watch apps. ui_push_page() slides a sub-page in from the
 * right over the current page; swiping right slides it away again. Pickers
 * use a pushed list with a check mark instead of a dropdown popup, which is
 * cramped on a watch. Other boards keep the dropdown.
 */
#include "ui_define.h"

#if FACTORY_WATCH_UI

#define PAGE_PUSH_MS            200
#define PAGE_POP_MS             180
#define PICKER_POP_DELAY_MS     200     /* Long enough to see the check mark move */
#define LIST_ROW_H              72
#define LIST_ICON_DISC          52
#define PICKER_OPTION_MAX_LEN   64

static void page_set_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void page_slide(lv_obj_t *root, int32_t from, int32_t to, uint32_t duration,
                       uint32_t delay, lv_anim_completed_cb_t completed_cb)
{
    lv_anim_delete(root, page_set_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, page_set_x);
    if (completed_cb) {
        lv_anim_set_completed_cb(&a, completed_cb);
    }
    lv_anim_start(&a);
}

static void page_pop_root(lv_obj_t *root, uint32_t delay)
{
    if (!root) return;
    /* ui_create_app_page(): nav is child 0 and the back button is its child 0. */
    lv_obj_t *nav = lv_obj_get_child(root, 0);
    lv_obj_t *back = nav ? lv_obj_get_child(nav, 0) : NULL;
    /* A disabled back button marks a page that is already closing; swipe-back ignores it too. */
    if (!back || lv_obj_has_state(back, LV_STATE_DISABLED)) return;
    lv_obj_add_state(back, LV_STATE_DISABLED);

    /* Swallow taps while the page waits and slides away. */
    lv_obj_t *blocker = lv_obj_create(root);
    lv_obj_remove_style_all(blocker);
    lv_obj_set_size(blocker, LV_PCT(100), LV_PCT(100));

    page_slide(root, lv_obj_get_x(root), lv_display_get_horizontal_resolution(NULL),
               PAGE_POP_MS, delay, lv_obj_delete_anim_completed_cb);
}

static void page_back_cb(lv_event_t *e)
{
    lv_obj_t *back = lv_event_get_target_obj(e);
    page_pop_root(lv_obj_get_parent(lv_obj_get_parent(back)), 0);
}

lv_obj_t *ui_push_page(lv_obj_t *from, const char *title)
{
    lv_obj_t *parent = ui_swipe_back_root(from);
    if (!parent) parent = lv_obj_get_screen(from);

    lv_obj_t *content = ui_create_app_page(parent, title, page_back_cb);
    lv_obj_t *root = (lv_obj_t *)lv_obj_get_user_data(content);
    int32_t width = lv_display_get_horizontal_resolution(NULL);
    lv_obj_set_x(root, width);
    page_slide(root, width, 0, PAGE_PUSH_MS, 0, NULL);
    hw_feedback();
    return content;
}

void ui_pop_page(lv_obj_t *content)
{
    if (!content) return;
    page_pop_root((lv_obj_t *)lv_obj_get_user_data(content), 0);
}

/* Pill-shaped row like the app list's. */
static lv_obj_t *list_row_create(lv_obj_t *parent)
{
    ui_styles_init();
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_add_style(row, &ui_styles.card, 0);
    lv_obj_set_size(row, LV_PCT(100), LIST_ROW_H);
    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, LIST_ROW_H / 2, 0);
    lv_obj_set_style_pad_hor(row, (LIST_ROW_H - LIST_ICON_DISC) / 2, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *list_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(label, 1);
    return label;
}

lv_obj_t *ui_create_menu_row(lv_obj_t *parent, const char *symbol, const char *title,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = list_row_create(parent);

    if (symbol) {
        lv_obj_t *disc = lv_obj_create(row);
        lv_obj_add_style(disc, &ui_styles.accent_btn, 0);
        lv_obj_set_size(disc, LIST_ICON_DISC, LIST_ICON_DISC);
        lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(disc, 0, 0);
        lv_obj_remove_flag(disc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(disc, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *icon = lv_label_create(disc);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_center(icon);
    }

    list_row_label(row, title);

    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(chevron, UI_COLOR_TEXT_SECONDARY, 0);

    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

static void picker_option_click_cb(lv_event_t *e)
{
    lv_obj_t *option = lv_event_get_current_target_obj(e);
    lv_obj_t *dropdown = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *list = lv_obj_get_parent(option);
    uint32_t index = lv_obj_get_index(option);

    /* Each option row ends with its check mark. */
    uint32_t count = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *check = lv_obj_get_child(lv_obj_get_child(list, i), -1);
        if (i == index) {
            lv_obj_remove_flag(check, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lv_dropdown_get_selected(dropdown) != index) {
        lv_dropdown_set_selected(dropdown, index);
        lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    }
    hw_feedback();
    page_pop_root((lv_obj_t *)lv_obj_get_user_data(list), PICKER_POP_DELAY_MS);
}

static void picker_open_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_obj_t *title = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *dropdown = lv_obj_get_child(row, -1);
    if (!dropdown || lv_obj_has_state(dropdown, LV_STATE_DISABLED)) return;

    lv_obj_t *list = ui_push_page(row, title ? lv_label_get_text(title) : "");
    uint32_t selected = lv_dropdown_get_selected(dropdown);
    lv_obj_t *selected_row = NULL;
    const char *options = lv_dropdown_get_options(dropdown);

    for (uint32_t index = 0; options && *options; index++) {
        const char *end = strchr(options, '\n');
        size_t len = end ? (size_t)(end - options) : strlen(options);
        char text[PICKER_OPTION_MAX_LEN];
        len = LV_MIN(len, sizeof(text) - 1);
        memcpy(text, options, len);
        text[len] = '\0';

        lv_obj_t *option = list_row_create(list);
        list_row_label(option, text);
        lv_obj_t *check = lv_label_create(option);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_add_style(check, &ui_styles.accent_text, 0);
        lv_obj_set_style_text_font(check, &lv_font_montserrat_24, 0);
        if (index == selected) {
            selected_row = option;
        } else {
            lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_event_cb(option, picker_option_click_cb, LV_EVENT_CLICKED, dropdown);

        options = end ? end + 1 : options + strlen(options);
    }

    if (selected_row) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_view(selected_row, LV_ANIM_OFF);
    }
}

lv_obj_t *ui_create_card_picker(lv_obj_t *card, const char *icon, const char *title,
                                 const char *options, uint8_t sel, lv_event_cb_t cb)
{
    lv_obj_t *row = ui_create_card_item(card, icon, title, NULL);
    lv_obj_t *title_label = title ? lv_obj_get_child(row, icon ? 1 : 0) : NULL;

    /* The dropdown only shows the choice ("Auto >"); the row takes the tap. */
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_group_remove_obj(dd);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, sel);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_RIGHT);
    lv_obj_set_size(dd, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(dd, 1);
    lv_obj_remove_flag(dd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(dd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_outline_width(dd, 0, 0);
    lv_obj_set_style_pad_all(dd, 0, 0);
    lv_obj_set_style_pad_column(dd, 12, 0);
    lv_obj_set_style_text_align(dd, LV_TEXT_ALIGN_RIGHT, 0);
    if (title_label) {
        lv_obj_set_style_text_font(dd, lv_obj_get_style_text_font(title_label, LV_PART_MAIN), 0);
    }
    lv_obj_set_style_text_color(dd, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_color(dd, UI_COLOR_TEXT_SECONDARY, LV_PART_INDICATOR);
    if (cb) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_event_cb(row, picker_open_cb, LV_EVENT_CLICKED, title_label);
    return row;
}

#else

lv_obj_t *ui_create_card_picker(lv_obj_t *card, const char *icon, const char *title,
                                 const char *options, uint8_t sel, lv_event_cb_t cb)
{
    return ui_create_card_dropdown(card, icon, title, options, sel, cb);
}

#endif /* FACTORY_WATCH_UI */
