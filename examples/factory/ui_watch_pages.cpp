/**
 * @file      ui_watch_pages.cpp
 * @license   MIT
 * @date      2026-11-03
 *
 * Nested pages for watch apps. ui_push_page() slides a sub-page in from the
 * right over the current page; swiping right slides it away again. Pickers
 * use a pushed list with a check mark instead of a dropdown popup, which is
 * cramped on a watch. Other boards keep the dropdown.
 *
 * One-gesture controls: level rows open a full-screen −/+ page instead of an
 * inline slider, and ui_require_hold() turns a tap into press-and-hold for
 * actions that can't be undone.
 */
#include "ui_define.h"

#if FACTORY_WATCH_UI

#define PAGE_PUSH_MS            200
#define PAGE_POP_MS             180
#define PICKER_POP_DELAY_MS     200     /* Long enough to see the check mark move */
#define LIST_ROW_H              72
#define LIST_ICON_DISC          52
#define PICKER_OPTION_MAX_LEN   64
#define LEVEL_STEP_BTN          96      /* Diameter of the − and + buttons */
#define LEVEL_STEPS             20      /* −/+ presses from one end to the other */
#define HOLD_CONFIRM_MS         800
#define TOAST_MS                1400
#define ACTION_PILL_H           64

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

    /* No chevron: a pill row is plainly a button. Value rows in cards keep theirs,
     * which is what tells them apart from read-only info rows. */
    list_row_label(row, title);

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

void ui_picker_adopt(lv_obj_t *row, lv_obj_t *title_label, lv_obj_t *dd)
{
    /* The dropdown only shows the choice ("Auto >"); the row takes the tap. Apps
     * that build their own dropdown may have styled it, so every style is reset. */
    lv_group_remove_obj(dd);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_RIGHT);
    lv_obj_set_size(dd, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(dd, 1);
    lv_obj_remove_flag(dd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dd, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    lv_obj_set_style_margin_left(dd, 0, 0);
    lv_obj_set_style_margin_right(dd, 0, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dd, 0, 0);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_outline_width(dd, 0, 0);
    lv_obj_set_style_pad_all(dd, 0, 0);
    lv_obj_set_style_pad_column(dd, 12, 0);
    lv_obj_set_style_text_align(dd, LV_TEXT_ALIGN_RIGHT, 0);
    const lv_font_t *font = title_label ? lv_obj_get_style_text_font(title_label, LV_PART_MAIN)
                            : lv_obj_get_style_text_font(row, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, font, 0);
    lv_obj_set_style_text_color(dd, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_color(dd, UI_COLOR_TEXT_SECONDARY, LV_PART_INDICATOR);
    lv_obj_set_style_text_opa(dd, LV_OPA_40, LV_STATE_DISABLED);

    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_event_cb(row, picker_open_cb, LV_EVENT_CLICKED, title_label);
}

lv_obj_t *ui_create_card_picker(lv_obj_t *card, const char *icon, const char *title,
                                 const char *options, uint8_t sel, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(lv_obj_create(card)); /* temp parent, re-parented */
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, sel);
    if (cb) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    /* ui_create_card_item() turns a dropdown row into a picker row on the watch. */
    return ui_create_card_item(card, icon, title, dd);
}

/* ── Action pills ──
 * One verb on a full-width pill; the row's last child is the button and the
 * button's first child its label, as with ui_create_card_button(). */

lv_obj_t *ui_create_card_action(lv_obj_t *card, const char *icon, const char *label,
                                lv_event_cb_t cb)
{
    LV_UNUSED(icon);    /* The verb says it; an icon would only crowd the pill. */
    ui_styles_init();
    lv_obj_t *btn = lv_button_create(lv_obj_create(card));  /* temp parent, re-parented */
    lv_obj_add_style(btn, &ui_styles.accent_btn, 0);
    lv_obj_set_size(btn, LV_PCT(100), ACTION_PILL_H);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 24, 0);
    /* Dark text reads on every accent, including Mono's white. */
    lv_obj_set_style_text_color(btn, lv_color_black(), 0);
    lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT_DIM, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_FOCUS, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, UI_COLOR_TEXT_SECONDARY, LV_STATE_DISABLED);

    lv_obj_t *text = lv_label_create(btn);
    lv_label_set_text(text, label ? label : "");
    lv_obj_set_style_text_font(text, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(text, LV_PCT(100), 0);
    lv_obj_center(text);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *row = ui_create_card_item(card, NULL, NULL, btn);
    /* Touch only: a focused pill would keep the theme's focus ring after a tap. */
    lv_group_remove_obj(btn);
    /* Pills sit between other rows; the divider under them is noise. */
    lv_obj_t *divider = lv_obj_get_child(card, -1);
    if (divider) lv_obj_add_flag(divider, LV_OBJ_FLAG_HIDDEN);
    return row;
}

/* ── Level rows ──
 * The row holds a hidden lv_slider as its last child: it keeps the value and
 * sends LV_EVENT_VALUE_CHANGED to the app, as an inline slider did. */

typedef struct {
    lv_obj_t *model;        /* Hidden slider in the row */
    lv_obj_t *row_value;    /* Value label in the row */
    lv_obj_t *bar;          /* Slider on the level page */
    lv_obj_t *page_value;   /* Big value label on the level page */
    bool repeated;          /* −/+ repeated while held, so skip the release click */
} level_page_t;

static void level_format(lv_obj_t *slider, char *buf, size_t len)
{
    int32_t value = lv_slider_get_value(slider);
    int32_t min = lv_slider_get_min_value(slider);
    int32_t max = lv_slider_get_max_value(slider);
    /* 0–100 and 0–255 are levels, shown as a share of full; other ranges are counts or IDs. */
    if (min >= 0 && (max == 100 || max == 255)) {
        lv_snprintf(buf, len, "%d%%", (int)((value * 100 + max / 2) / max));
    } else {
        lv_snprintf(buf, len, "%d", (int)value);
    }
}

static void level_row_refresh(lv_obj_t *model, lv_obj_t *row_value)
{
    char text[16];
    level_format(model, text, sizeof(text));
    lv_label_set_text(row_value, text);
}

/* Sets the app's value, then shows what the app kept (its callback may snap it). */
static void level_page_set(level_page_t *state, int32_t value)
{
    if (!lv_obj_is_valid(state->model)) return;
    int32_t min = lv_slider_get_min_value(state->model);
    int32_t max = lv_slider_get_max_value(state->model);
    value = LV_CLAMP(min, value, max);
    if (value != lv_slider_get_value(state->model)) {
        lv_slider_set_value(state->model, value, LV_ANIM_OFF);
        lv_obj_send_event(state->model, LV_EVENT_VALUE_CHANGED, NULL);
        if (!lv_obj_is_valid(state->model)) return;
    }
    level_row_refresh(state->model, state->row_value);
    level_row_refresh(state->model, state->page_value);
    lv_slider_set_value(state->bar, lv_slider_get_value(state->model), LV_ANIM_OFF);
}

static int32_t level_step(lv_obj_t *model)
{
    int32_t range = lv_slider_get_max_value(model) - lv_slider_get_min_value(model);
    return LV_MAX(1, (range + LEVEL_STEPS / 2) / LEVEL_STEPS);
}

static void level_step_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    level_page_t *state = (level_page_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        state->repeated = false;
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED_REPEAT) {
        state->repeated = true;
    } else if (state->repeated) {
        return;     /* The click that ends a held press */
    }
    if (!lv_obj_is_valid(state->model)) return;
    int32_t dir = (int32_t)(intptr_t)lv_obj_get_user_data(btn);
    level_page_set(state, lv_slider_get_value(state->model) + dir * level_step(state->model));
    if (code == LV_EVENT_CLICKED) hw_feedback();
}

static void level_bar_cb(lv_event_t *e)
{
    level_page_t *state = (level_page_t *)lv_event_get_user_data(e);
    level_page_set(state, lv_slider_get_value(lv_event_get_current_target_obj(e)));
}

static void level_page_delete_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static lv_obj_t *level_step_button(lv_obj_t *parent, const char *symbol, int32_t dir,
                                   level_page_t *state)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_add_style(btn, &ui_styles.card, 0);
    lv_obj_set_size(btn, LEVEL_STEP_BTN, LEVEL_STEP_BTN);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(btn, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(btn, level_step_cb, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(btn, level_step_cb, LV_EVENT_CLICKED, state);
    lv_obj_add_event_cb(btn, level_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, state);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_add_style(icon, &ui_styles.accent_text, 0);
    lv_obj_center(icon);
    return btn;
}

static void level_open_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_obj_t *title = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *model = lv_obj_get_child(row, -1);
    if (!model || lv_obj_has_state(model, LV_STATE_DISABLED)) return;

    level_page_t *state = (level_page_t *)lv_malloc_zeroed(sizeof(level_page_t));
    if (!state) return;
    state->model = model;
    state->row_value = lv_obj_get_child(row, -3);   /* value, chevron, model */

    lv_obj_t *content = ui_push_page(row, title ? lv_label_get_text(title) : "");
    lv_obj_add_event_cb(content, level_page_delete_cb, LV_EVENT_DELETE, state);
    /* Everything fits, so the page doesn't scroll and a drag only moves the bar. */
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *steps = lv_obj_create(content);
    lv_obj_remove_style_all(steps);
    lv_obj_set_size(steps, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(steps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(steps, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(steps, LV_OBJ_FLAG_SCROLLABLE);

    level_step_button(steps, LV_SYMBOL_MINUS, -1, state);
    state->page_value = lv_label_create(steps);
    lv_obj_set_style_text_font(state->page_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(state->page_value, UI_COLOR_TEXT_PRIMARY, 0);
    level_step_button(steps, LV_SYMBOL_PLUS, 1, state);

    state->bar = lv_slider_create(content);
    lv_obj_set_size(state->bar, LV_PCT(88), 28);
    lv_obj_set_style_pad_all(state->bar, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(state->bar, 24);
    lv_slider_set_range(state->bar, lv_slider_get_min_value(model), lv_slider_get_max_value(model));
    lv_slider_set_value(state->bar, lv_slider_get_value(model), LV_ANIM_OFF);
    lv_obj_add_event_cb(state->bar, level_bar_cb, LV_EVENT_VALUE_CHANGED, state);

    level_row_refresh(model, state->page_value);
}

lv_obj_t *ui_create_card_level(lv_obj_t *card, const char *icon, const char *title,
                                int32_t min, int32_t max, int32_t val, lv_event_cb_t cb)
{
    lv_obj_t *row = ui_create_card_item(card, icon, title, NULL);
    lv_obj_t *title_label = title ? lv_obj_get_child(row, icon ? 1 : 0) : NULL;
    const lv_font_t *font = title_label ? lv_obj_get_style_text_font(title_label, LV_PART_MAIN)
                            : lv_obj_get_style_text_font(row, LV_PART_MAIN);

    /* "60%  >", in the picker's colours */
    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_style_text_color(value, UI_COLOR_TEXT_SECONDARY, 0);

    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, font, 0);
    lv_obj_set_style_text_color(chevron, UI_COLOR_TEXT_SECONDARY, 0);

    lv_obj_t *model = lv_slider_create(row);
    lv_group_remove_obj(model);
    lv_obj_add_flag(model, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(model, LV_OBJ_FLAG_CLICKABLE);
    lv_slider_set_range(model, min, max);
    lv_slider_set_value(model, val, LV_ANIM_OFF);
    if (cb) {
        lv_obj_add_event_cb(model, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    level_row_refresh(model, value);

    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_event_cb(row, level_open_cb, LV_EVENT_CLICKED, title_label);
    return row;
}

/* ── Toast ── */

static lv_obj_t *toast_obj = NULL;

static void toast_delete_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == toast_obj) toast_obj = NULL;
}

void ui_toast(const char *text)
{
    if (toast_obj) lv_obj_delete(toast_obj);

    toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast_obj, UI_COLOR_CARD_FOCUS, 0);
    lv_obj_set_style_bg_opa(toast_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(toast_obj, 0, 0);
    lv_obj_set_style_radius(toast_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(toast_obj, 28, 0);
    lv_obj_set_style_pad_ver(toast_obj, 14, 0);
    lv_obj_remove_flag(toast_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(toast_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(toast_obj, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(toast_obj, toast_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *label = lv_label_create(toast_obj);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT_PRIMARY, 0);

    lv_obj_delete_delayed(toast_obj, TOAST_MS);
}

/* ── Hold to confirm ──
 * The control's own LV_EVENT_CLICKED handlers run only after a full hold. A
 * fill sweeps across the control while it is held. */

typedef struct {
    lv_obj_t *obj;
    lv_obj_t *fill;
    bool allow_click;
} hold_state_t;

static void hold_fill_set_width(void *obj, int32_t w)
{
    lv_obj_set_width((lv_obj_t *)obj, w);
}

static void hold_fill_remove(hold_state_t *state)
{
    if (!state->fill) return;
    lv_obj_delete(state->fill);     /* Also deletes its animation */
    state->fill = NULL;
}

static void hold_confirmed_cb(lv_anim_t *a)
{
    hold_state_t *state = (hold_state_t *)lv_anim_get_user_data(a);
    lv_obj_t *obj = state->obj;

    hw_feedback();
    state->allow_click = true;
    lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
    /* The action may have deleted the control, and its state with it. */
    if (!lv_obj_is_valid(obj)) return;
    state->allow_click = false;
}

static void hold_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    hold_state_t *state = (hold_state_t *)lv_event_get_user_data(e);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        /* Registered as a preprocess handler: block clicks that weren't a full hold. */
        if (!state->allow_click) lv_event_stop_processing(e);
        break;
    case LV_EVENT_PRESSED: {
        if (lv_obj_has_state(obj, LV_STATE_DISABLED)) break;
        hold_fill_remove(state);
        lv_obj_update_layout(obj);
        int32_t border = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
        state->fill = lv_obj_create(obj);
        lv_obj_remove_style_all(state->fill);
        lv_obj_add_flag(state->fill, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(state->fill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(state->fill, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(state->fill, LV_OPA_30, 0);
        /* Child positions start inside the padding; the fill covers the whole control. */
        lv_obj_set_pos(state->fill, -lv_obj_get_style_pad_left(obj, LV_PART_MAIN) - border,
                       -lv_obj_get_style_pad_top(obj, LV_PART_MAIN) - border);
        lv_obj_set_size(state->fill, 0, lv_obj_get_height(obj));
        lv_obj_set_style_clip_corner(obj, true, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, state->fill);
        lv_anim_set_values(&a, 0, lv_obj_get_width(obj));
        lv_anim_set_duration(&a, HOLD_CONFIRM_MS);
        lv_anim_set_exec_cb(&a, hold_fill_set_width);
        lv_anim_set_user_data(&a, state);
        lv_anim_set_completed_cb(&a, hold_confirmed_cb);
        lv_anim_start(&a);
        break;
    }
    case LV_EVENT_PRESSING: {
        /* A scroll that started on the control cancels the hold. */
        lv_indev_t *indev = lv_indev_active();
        if (indev && lv_indev_get_scroll_obj(indev)) hold_fill_remove(state);
        break;
    }
    case LV_EVENT_RELEASED: {
        bool was_holding = state->fill && lv_obj_get_width(state->fill) < lv_obj_get_width(obj);
        hold_fill_remove(state);
        if (was_holding) ui_toast("Hold to confirm");
        break;
    }
    case LV_EVENT_PRESS_LOST:
        hold_fill_remove(state);
        break;
    case LV_EVENT_DELETE:
        lv_free(state);
        break;
    default:
        break;
    }
}

void ui_require_hold(lv_obj_t *obj)
{
    if (!obj) return;
    hold_state_t *state = (hold_state_t *)lv_malloc_zeroed(sizeof(hold_state_t));
    if (!state) return;
    state->obj = obj;
    lv_obj_add_event_cb(obj, hold_event_cb,
                        (lv_event_code_t)(LV_EVENT_CLICKED | LV_EVENT_PREPROCESS), state);
    lv_obj_add_event_cb(obj, hold_event_cb, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(obj, hold_event_cb, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(obj, hold_event_cb, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(obj, hold_event_cb, LV_EVENT_PRESS_LOST, state);
    lv_obj_add_event_cb(obj, hold_event_cb, LV_EVENT_DELETE, state);
}

#else

lv_obj_t *ui_create_card_picker(lv_obj_t *card, const char *icon, const char *title,
                                 const char *options, uint8_t sel, lv_event_cb_t cb)
{
    return ui_create_card_dropdown(card, icon, title, options, sel, cb);
}

void ui_require_hold(lv_obj_t *obj)
{
    LV_UNUSED(obj);
}

#endif /* FACTORY_WATCH_UI */
