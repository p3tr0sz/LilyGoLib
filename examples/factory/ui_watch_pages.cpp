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
#define LIST_ROW_H              88
#define LIST_ICON_DISC          52
#define PICKER_OPTION_MAX_LEN   64
#define LEVEL_STEP_BTN          96      /* Diameter of the − and + buttons */
#define LEVEL_STEPS             20      /* −/+ presses from one end to the other */
#define HOLD_CONFIRM_MS         800
#define TOAST_MS                1400
#define ACTION_PILL_H           82

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
    /* No buzz: the slide shows it, and taps buzz sparingly (8.2). */
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
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
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
    lv_obj_set_style_text_font(text, UI_FONT_BODY, 0);
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
    if (divider) {
        lv_obj_add_flag(divider, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(divider, LV_OBJ_FLAG_USER_3);   /* Stays hidden when a row follows */
    }
    return row;
}

/* ── Text fields ──
 * Typing stays only where nothing else can do: passwords, names, messages. The
 * keyboard then fills the screen, so its keys are as big as the watch allows. */

#define TEXT_FIELD_H        60
#define TEXT_KEY_GAP        6
#define TEXT_VALUE_MAX      24      /* Characters shown in a row before the dots */

/* ── Two-tap keyboard ──
 * A QWERTY row of ten keys is about 40 px wide here, too small for a finger.
 * Instead the first tap picks a group of letters, laid out as on a phone keypad
 * (.,?! abc def / ghi jkl mno / pqrs tuv wxyz), and the second tap picks one of
 * them, shown in lower and upper case on keys a third of the screen wide. So no
 * shift key is needed. "123" opens the
 * digits; "#+=" and "123" switch between digits and symbols, which stay open
 * for runs until "abc". Number-only fields get a number pad.
 *
 * One lv_buttonmatrix; each view swaps its map. */

typedef enum {
    KB_GROUPS,      /* .,?! abc def … plus 123, space and backspace */
    KB_LETTERS,     /* The letters of one group */
    KB_DIGITS,
    KB_SYMBOLS,
    KB_NUMPAD,      /* Number-only fields */
} kb_view_t;

#define KB_BACK     LV_SYMBOL_LEFT
#define KB_SPACE    "space"
#define KB_DEL      LV_SYMBOL_BACKSPACE
#define KB_ABC      "abc"   /* From digits or symbols back to the letter groups */
#define KB_OK_SIZE  60      /* The round ✓ beside the field; every key grid is three wide */
#define KB_MAP_MAX  32

/* Group 0 is the keypad's "1": punctuation, in two rows instead of two cases. */
#define KB_PUNCT    ".,?!"
static const char *const kb_groups[] = {KB_PUNCT, "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
static const char kb_group_digit[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static const char kb_punct_rows[2][5] = {".,?!", "'-@:"};

/* ── Accented letters ──
 * Settings > Keyboard picks a language; its letters join their base letter's
 * group, after it (abc → a ą b c ć for Polish). Each entry lists, per base
 * letter, the base followed by its accented forms. Capitals come from
 * kb_upper(). The fonts hold all of Latin-1 and Latin Extended-A (font_latin.c). */
typedef struct {
    const char *name;
    const char *letters;
} kb_language_t;

static const kb_language_t kb_languages[] = {
    {"None", ""},
    {"Polish", "aą cć eę lł nń oó sś zźż"},
    {"Czech & Slovak", "aáä cč dď eéě ií lĺľ nň oóô rřŕ sš tť uúů yý zž"},
    {"German", "aä oö sß uü"},
    {"French", "aàâæ cç eéèêë iîï oôœ uùûü yÿ"},
    {"Spanish", "aá eé ií nñ oó uúü"},
    {"Portuguese", "aáâãà cç eéê ií oóôõ uú"},
    {"Italian", "aà eèé iì oò uù"},
    {"Nordic", "aåäæ oöø"},
    {"Hungarian", "aá eé ií oóöő uúüű"},
    {"Turkish", "cç gğ iı oö sş uü"},
};
#define KB_LANGUAGE_COUNT   (sizeof(kb_languages) / sizeof(kb_languages[0]))
#define KB_GROUP_MAX        10      /* Letters a group page can hold, accents included */

/* Decodes one UTF-8 character (up to 3 bytes); returns its length. */
static size_t kb_utf8_decode(const char *s, uint32_t *out)
{
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) { *out = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *out = ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *out = ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        return 3;
    }
    *out = '?';
    return 1;
}

static void kb_utf8_encode(uint32_t c, char out[4])
{
    if (c < 0x80) {
        out[0] = (char)c; out[1] = 0;
    } else if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); out[2] = 0;
    } else {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F)); out[3] = 0;
    }
}

/* Capital of a Latin-1 or Latin Extended-A letter. */
static uint32_t kb_upper(uint32_t c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c == 0xDF) return c;                            /* ß has no common capital */
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c == 0x131) return 'I';                         /* Dotless ı */
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x138 || c == 0x149 || c == 0x17F) return c;
        /* Two runs pair odd capitals with even small letters; elsewhere it's even, odd. */
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 0) ? c - 1 : c;
        return (c % 2 == 1) ? c - 1 : c;
    }
    return c;
}

/* The accented forms of `base` in the chosen language, or "". */
static const char *kb_accents_for(char base)
{
    uint8_t lang = hw_get_watch_keyboard_accents();
    if (lang >= KB_LANGUAGE_COUNT) return "";
    for (const char *p = kb_languages[lang].letters; *p; ) {
        if (*p == ' ') { ++p; continue; }
        const char *end = strchr(p, ' ');
        if (!end) end = p + strlen(p);
        if (*p == base) {
            static char buf[24];
            size_t len = LV_MIN((size_t)(end - p - 1), sizeof(buf) - 1);
            memcpy(buf, p + 1, len);
            buf[len] = 0;
            return buf;
        }
        p = end;
    }
    return "";
}

static const char *const kb_map_groups[] = {
    KB_PUNCT, "abc", "def", "\n",
    "ghi", "jkl", "mno", "\n",
    "pqrs", "tuv", "wxyz", "\n",
    "123", KB_SPACE, KB_DEL, "",
};
static const char *const kb_map_digits[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    KB_ABC, "0", "#+=", "",
};
static const char *const kb_map_symbols[] = {
    ".", ",", "?", "!", ":", "\n",
    "@", "-", "_", "/", ";", "\n",
    "#", "&", "%", "$", "*", "\n",
    "+", "=", "(", ")", "'", "\n",
    KB_ABC, "123", "",
};
static const char *const kb_map_numpad[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", KB_DEL, "",
};

typedef struct {
    lv_obj_t *target;       /* The app's field */
    lv_obj_t *edit;         /* The copy on the page */
    lv_obj_t *keys;         /* The button matrix */
    lv_obj_t *eye_icon;     /* Password fields: the show/hide toggle's icon */
    kb_view_t view;
    bool numeric;
    const char *map[KB_MAP_MAX];
    lv_buttonmatrix_ctrl_t ctrl[KB_MAP_MAX];
    char letters[2][KB_GROUP_MAX][4];   /* KB_LETTERS: lower and upper case, UTF-8 */
    char digit[2];
    uint8_t letter_count;   /* KB_LETTERS: keys in each of the two letter rows */
} text_page_t;

static bool text_is_numeric(lv_obj_t *ta)
{
    const char *accepted = lv_textarea_get_accepted_chars(ta);
    if (!accepted || !accepted[0]) return false;
    for (const char *c = accepted; *c; ++c) {
        if (!strchr("0123456789.+-", *c)) return false;
    }
    return true;
}

static void text_page_delete_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static bool kb_is(const char *text, const char *key)
{
    return text && strcmp(text, key) == 0;
}

/* Maps end with ""; the symbols map has an empty slot after its last row. */
static void kb_set_map(text_page_t *st, const char *const *map)
{
    size_t n = 0;
    while (n + 1 < KB_MAP_MAX && map[n][0]) {
        st->map[n] = map[n];
        ++n;
    }
    st->map[n] = "";
    lv_buttonmatrix_set_map(st->keys, st->map);
}

/* Every key's flags are set again for each view: lv_buttonmatrix keeps the old
 * ones when a new map has as many keys. */
static void kb_style_keys(text_page_t *st, uint32_t wide_key)
{
    lv_obj_t *keys = st->keys;
    for (uint32_t i = 0;; ++i) {
        const char *text = lv_buttonmatrix_get_button_text(keys, i);
        if (!text) break;
        /* Act on release, so a swipe across the keys goes back instead of typing. */
        uint32_t ctrl = LV_BUTTONMATRIX_CTRL_CLICK_TRIG;   /* An enum in C++ */
        if (!kb_is(text, KB_DEL)) ctrl |= LV_BUTTONMATRIX_CTRL_NO_REPEAT;   /* Only ⌫ repeats */
        /* Keys that change view or edit are "checked": accent text on lighter grey. */
        if (kb_is(text, KB_BACK) || kb_is(text, "123") || kb_is(text, "#+=") ||
                (st->view != KB_GROUPS && kb_is(text, KB_ABC)) ||
                kb_is(text, KB_DEL) || kb_is(text, KB_SPACE)) {
            ctrl |= LV_BUTTONMATRIX_CTRL_CHECKED;
        }
        ctrl |= i == wide_key ? 2 : 1;   /* Width */
        st->ctrl[i] = (lv_buttonmatrix_ctrl_t)ctrl;
    }
    lv_buttonmatrix_set_ctrl_map(keys, st->ctrl);
}

static void kb_show(text_page_t *st, kb_view_t view, int group)
{
    st->view = view;
    switch (view) {
    case KB_GROUPS:  kb_set_map(st, kb_map_groups); break;
    case KB_DIGITS:  kb_set_map(st, kb_map_digits); break;
    case KB_SYMBOLS: kb_set_map(st, kb_map_symbols); break;
    case KB_NUMPAD:  kb_set_map(st, kb_map_numpad); break;
    case KB_LETTERS: {
        /* Small letters, each followed by its accents, then the same in capitals. */
        uint32_t codes[KB_GROUP_MAX];
        size_t count = 0;
        for (const char *b = kb_groups[group]; *b && count < KB_GROUP_MAX; ++b) {
            if (group == 0) break;
            codes[count++] = (uint8_t)*b;
            for (const char *a = kb_accents_for(*b); *a && count < KB_GROUP_MAX; ) {
                a += kb_utf8_decode(a, &codes[count++]);
            }
        }
        size_t n = 0;
        for (int row = 0; row < 2; ++row) {
            size_t cols = group == 0 ? strlen(kb_punct_rows[row]) : count;
            for (size_t i = 0; i < cols; ++i) {
                uint32_t c = group == 0 ? (uint8_t)kb_punct_rows[row][i] : (row ? kb_upper(codes[i]) : codes[i]);
                kb_utf8_encode(c, st->letters[row][i]);
                st->map[n++] = st->letters[row][i];
            }
            st->map[n++] = "\n";
        }
        st->letter_count = (uint8_t)(group == 0 ? strlen(kb_punct_rows[0]) : count);
        st->digit[0] = kb_group_digit[group];
        st->digit[1] = 0;
        st->map[n++] = KB_BACK;
        st->map[n++] = st->digit;
        st->map[n] = "";
        lv_buttonmatrix_set_map(st->keys, st->map);
        break;
    }
    }
    /* In a group the back key is wide: it's what a mis-tap needs. */
    kb_style_keys(st, view == KB_LETTERS ? (uint32_t)(2 * st->letter_count) : UINT32_MAX);
}

/* LVGL animates the field's scroll to the cursor on every key, so long text
 * slid back and forth while typing. Jump there instead. */
static void kb_snap_scroll(lv_obj_t *edit)
{
    lv_anim_delete(edit, NULL);
    lv_obj_t *label = lv_textarea_get_label(edit);
    lv_obj_update_layout(edit);
    lv_point_t pos;
    lv_label_get_letter_pos(label, lv_textarea_get_cursor_pos(edit), &pos);
    /* As LVGL does: keep a letter's height free right of the cursor. */
    int32_t room = lv_font_get_line_height(lv_obj_get_style_text_font(edit, LV_PART_MAIN));
    int32_t w = lv_obj_get_content_width(edit);
    lv_obj_scroll_to_x(edit, pos.x + room > w ? pos.x + room - w : 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(edit, 0, LV_ANIM_OFF);
}

static void kb_commit(text_page_t *st)
{
    lv_obj_t *content = lv_obj_get_parent(st->keys);
    lv_textarea_set_text(st->target, lv_textarea_get_text(st->edit));
    lv_obj_send_event(st->target, LV_EVENT_READY, NULL);
    ui_pop_page(content);
}

static void kb_ok_cb(lv_event_t *e)
{
    hw_feedback();
    kb_commit((text_page_t *)lv_event_get_user_data(e));
}

static void kb_event_cb(lv_event_t *e)
{
    text_page_t *st = (text_page_t *)lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(st->keys);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *text = lv_buttonmatrix_get_button_text(st->keys, id);
    if (!text || !text[0]) return;
    hw_feedback();

    if (kb_is(text, KB_DEL)) {
        lv_textarea_delete_char(st->edit);
    } else if (kb_is(text, KB_SPACE)) {
        lv_textarea_add_char(st->edit, ' ');
    } else if (kb_is(text, KB_BACK) || (st->view != KB_GROUPS && kb_is(text, KB_ABC))) {
        kb_show(st, KB_GROUPS, 0);
    } else if (kb_is(text, "123")) {
        kb_show(st, KB_DIGITS, 0);
    } else if (kb_is(text, "#+=")) {
        kb_show(st, KB_SYMBOLS, 0);
    } else if (st->view == KB_GROUPS) {
        for (int g = 0; g < (int)(sizeof(kb_groups) / sizeof(kb_groups[0])); ++g) {
            if (kb_is(text, kb_groups[g])) {
                kb_show(st, KB_LETTERS, g);
                break;
            }
        }
    } else {
        lv_textarea_add_text(st->edit, text);
        /* A letter is one pick; digits and symbols often come in runs. */
        if (st->view == KB_LETTERS) kb_show(st, KB_GROUPS, 0);
    }
    kb_snap_scroll(st->edit);
}

/* Dots (spaced so they can be counted) or the text itself. */
static void kb_set_password_shown(text_page_t *st, bool shown)
{
    lv_textarea_set_password_mode(st->edit, !shown);
    lv_obj_set_style_text_letter_space(st->edit, shown ? 0 : 8, 0);
    if (st->eye_icon) lv_label_set_text(st->eye_icon, shown ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    kb_snap_scroll(st->edit);
}

static void kb_eye_cb(lv_event_t *e)
{
    text_page_t *st = (text_page_t *)lv_event_get_user_data(e);
    kb_set_password_shown(st, lv_textarea_get_password_mode(st->edit));
    hw_feedback();
}

void ui_open_text_page(lv_obj_t *from, const char *title, lv_obj_t *ta)
{
    if (!ta) return;
    ui_styles_init();
    lv_obj_t *content = ui_push_page(from, title && title[0] ? title : "Text");
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    /* Wider than other pages: ten keys share the row. */
    lv_obj_set_style_pad_hor(content, 12, 0);
    lv_obj_set_style_pad_bottom(content, 14, 0);
    lv_obj_set_style_pad_row(content, 10, 0);

    text_page_t *state = (text_page_t *)lv_malloc_zeroed(sizeof(text_page_t));
    if (!state) return;
    state->target = ta;
    lv_obj_add_event_cb(content, text_page_delete_cb, LV_EVENT_DELETE, state);

    /* The field and its round ✓, so the key grid below stays three columns. */
    lv_obj_t *field_row = lv_obj_create(content);
    lv_obj_remove_style_all(field_row);
    lv_obj_set_size(field_row, LV_PCT(100), TEXT_FIELD_H);
    lv_obj_set_flex_flow(field_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(field_row, TEXT_KEY_GAP, 0);
    lv_obj_remove_flag(field_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *edit = lv_textarea_create(field_row);
    state->edit = edit;
    lv_textarea_set_one_line(edit, true);
    const bool password = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(edit, password);
    if (password) {
        /* LVGL shows a new letter for 1.5 s, then swaps it for a much narrower
         * dot, so the text and cursor jumped back after every key. Dots at once
         * (the key just pressed shows the letter), spaced so they can be counted. */
        lv_textarea_set_password_show_time(edit, 0);
        lv_obj_set_style_text_letter_space(edit, 8, 0);
    }
    uint32_t max_len = lv_textarea_get_max_length(ta);
    if (max_len) lv_textarea_set_max_length(edit, max_len);
    const char *accepted = lv_textarea_get_accepted_chars(ta);
    if (accepted) lv_textarea_set_accepted_chars(edit, accepted);
    lv_textarea_set_placeholder_text(edit, lv_textarea_get_placeholder_text(ta));
    lv_textarea_set_text(edit, lv_textarea_get_text(ta));
    lv_obj_set_size(edit, 1, TEXT_FIELD_H);
    lv_obj_set_flex_grow(edit, 1);
    lv_obj_set_style_text_font(edit, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(edit, UI_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_color(edit, UI_COLOR_TEXT_SECONDARY, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(edit, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_bg_opa(edit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(edit, 0, 0);
    lv_obj_set_style_radius(edit, TEXT_FIELD_H / 2, 0);
    lv_obj_set_style_pad_hor(edit, 24, 0);
    lv_obj_set_style_pad_ver(edit, (TEXT_FIELD_H - 36) / 2, 0);
    /* A thin steady bar: a block cursor changes width with the letter under it,
     * and blinking made it look jumpy. */
    /* The theme styles the cursor of a focused field, so set it for that state. */
    const lv_style_selector_t cursor = (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED;
    lv_obj_set_style_bg_opa(edit, LV_OPA_TRANSP, cursor);
    lv_obj_set_style_border_side(edit, LV_BORDER_SIDE_LEFT, cursor);
    lv_obj_set_style_border_width(edit, 3, cursor);
    lv_obj_set_style_border_color(edit, UI_COLOR_ACCENT, cursor);
    lv_obj_set_style_border_opa(edit, LV_OPA_COVER, cursor);
    lv_obj_set_style_anim_duration(edit, 0, cursor);     /* No blinking */
    lv_obj_set_scrollbar_mode(edit, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_state(edit, LV_STATE_FOCUSED);   /* Shows the cursor */
    lv_obj_remove_flag(edit, LV_OBJ_FLAG_CLICKABLE);
    lv_group_remove_obj(edit);

    if (password) {
        /* Show or hide the password: a slip is easy on a small screen. The
         * page always opens hidden (watch-ux-guidelines.md 9.5). */
        lv_obj_t *eye = lv_button_create(field_row);
        lv_obj_set_size(eye, KB_OK_SIZE, KB_OK_SIZE);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_shadow_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_style_bg_color(eye, UI_COLOR_CARD_BG, 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(eye, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
        /* A tap leaves no focus ring (touch only). */
        lv_obj_set_style_outline_width(eye, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(eye, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(eye, 0, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(eye, 0, LV_STATE_FOCUS_KEY);
        lv_group_remove_obj(eye);
        state->eye_icon = lv_label_create(eye);
        lv_label_set_text(state->eye_icon, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(state->eye_icon, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(state->eye_icon, UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_center(state->eye_icon);
        lv_obj_add_event_cb(eye, kb_eye_cb, LV_EVENT_CLICKED, state);
    }

    lv_obj_t *ok = lv_button_create(field_row);
    lv_obj_add_style(ok, &ui_styles.accent_btn, 0);
    lv_obj_set_size(ok, KB_OK_SIZE, KB_OK_SIZE);
    lv_obj_set_style_radius(ok, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_set_style_pad_all(ok, 0, 0);
    lv_obj_set_style_bg_color(ok, UI_COLOR_ACCENT_DIM, LV_STATE_PRESSED);
    lv_group_remove_obj(ok);
    lv_obj_t *ok_icon = lv_label_create(ok);
    lv_label_set_text(ok_icon, LV_SYMBOL_OK);
    lv_obj_set_style_text_font(ok_icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ok_icon, lv_color_black(), 0);
    lv_obj_center(ok_icon);
    lv_obj_add_event_cb(ok, kb_ok_cb, LV_EVENT_CLICKED, state);

    lv_obj_t *kb = lv_buttonmatrix_create(content);
    state->keys = kb;
    state->numeric = text_is_numeric(ta);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_gap(kb, TEXT_KEY_GAP, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COLOR_TEXT_PRIMARY, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COLOR_CARD_BG, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COLOR_CARD_FOCUS, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, UI_COLOR_CARD_FOCUS, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, UI_COLOR_ACCENT, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, UI_COLOR_CARD_BG,
                              (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_group_remove_obj(kb);
    lv_obj_add_flag(kb, UI_FLAG_SWIPE_BACK_OK);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, state);
    kb_show(state, state->numeric ? KB_NUMPAD : KB_GROUPS, 0);
    kb_snap_scroll(edit);
}

static void text_value_update(lv_obj_t *value, lv_obj_t *ta)
{
    const char *text = lv_textarea_get_text(ta);
    char buf[TEXT_VALUE_MAX * 3 + 8];
    size_t len = text ? strlen(text) : 0;
    if (len == 0) {
        const char *placeholder = lv_textarea_get_placeholder_text(ta);
        lv_label_set_text_fmt(value, "%s  " LV_SYMBOL_RIGHT, placeholder && placeholder[0] ? placeholder : "Add");
        return;
    }
    if (lv_textarea_get_password_mode(ta)) {
        size_t dots = LV_MIN(len, (size_t)12);
        buf[0] = 0;
        for (size_t i = 0; i < dots; ++i) lv_strcat(buf, LV_SYMBOL_BULLET);
    } else {
        lv_snprintf(buf, TEXT_VALUE_MAX + 1, "%s", text);
    }
    lv_label_set_text_fmt(value, "%s  " LV_SYMBOL_RIGHT, buf);
}

static void text_value_cb(lv_event_t *e)
{
    text_value_update((lv_obj_t *)lv_event_get_user_data(e), lv_event_get_target_obj(e));
}

static void text_row_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_obj_t *title_label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_obj_get_child(row, -1);
    ui_open_text_page(row, title_label ? lv_label_get_text(title_label) : NULL, ta);
}

void ui_text_adopt(lv_obj_t *row, lv_obj_t *title_label, lv_obj_t *ta)
{
    /* The field stays as the row's last child, hidden; it holds the text. */
    lv_group_remove_obj(ta);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);

    const lv_font_t *font = title_label ? lv_obj_get_style_text_font(title_label, LV_PART_MAIN)
                            : lv_obj_get_style_text_font(row, LV_PART_MAIN);
    lv_obj_t *value = lv_label_create(row);
    lv_obj_move_to_index(value, (int32_t)lv_obj_get_index(ta));
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_style_text_color(value, UI_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    text_value_update(value, ta);
    /* lv_textarea_set_text() sends VALUE_CHANGED, so text the app sets shows too. */
    lv_obj_add_event_cb(ta, text_value_cb, LV_EVENT_VALUE_CHANGED, value);

    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, text_row_click_cb, LV_EVENT_CLICKED, title_label);
}

static void kb_language_cb(lv_event_t *e)
{
    hw_set_watch_keyboard_accents((uint8_t)lv_dropdown_get_selected(lv_event_get_target_obj(e)));
}

void ui_keyboard_settings_build(lv_obj_t *content)
{
    char options[KB_LANGUAGE_COUNT * 18] = {0};
    for (size_t i = 0; i < KB_LANGUAGE_COUNT; ++i) {
        if (i) lv_strcat(options, "\n");
        lv_strcat(options, kb_languages[i].name);
    }
    uint8_t lang = hw_get_watch_keyboard_accents();
    lv_obj_t *card = ui_create_card(content, NULL);
    ui_create_card_picker(card, NULL, "Accents", options, lang < KB_LANGUAGE_COUNT ? lang : 0,
                          kb_language_cb);

    lv_obj_t *caption = lv_label_create(content);
    lv_label_set_text(caption, "Accented letters join their letter's page on the keyboard: "
                      "with Polish, abc shows a ą b c ć.");
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(caption, LV_PCT(100));
    lv_obj_set_style_pad_hor(caption, 12, 0);
    lv_obj_set_style_text_font(caption, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(caption, UI_COLOR_TEXT_SECONDARY, 0);
}

/* ── Scroll wheels ── */

lv_obj_t *ui_create_wheel(lv_obj_t *parent, const char *options, uint32_t sel,
                          int32_t width, bool infinite)
{
    lv_obj_t *roller = lv_roller_create(parent);
    lv_roller_set_options(roller, options, infinite ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 3);
    lv_roller_set_selected(roller, sel, LV_ANIM_OFF);
    lv_obj_set_width(roller, width);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_line_space(roller, 18, 0);
    lv_obj_set_style_text_color(roller, UI_COLOR_TEXT_SECONDARY, 0);
    /* Only the chosen row has a band; its neighbours fade into the page. */
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_text_color(roller, UI_COLOR_TEXT_PRIMARY, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(roller, UI_COLOR_CARD_FOCUS, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 16, LV_PART_SELECTED);
    lv_group_remove_obj(roller);
    return roller;
}

/* A page of wheels in one row; the caller adds them and a "Set" pill under them. */
lv_obj_t *ui_push_wheel_page(lv_obj_t *from, const char *title, lv_obj_t **wheels_row)
{
    lv_obj_t *content = ui_push_page(from, title);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    *wheels_row = row;
    return content;
}

void ui_wheel_number_options(char *out, size_t out_size, int count, int first)
{
    size_t len = 0;
    if (out_size) out[0] = 0;
    for (int i = first; i < first + count && len + 4 <= out_size; ++i) {
        len += (size_t)lv_snprintf(out + len, out_size - len, "%s%02d", i == first ? "" : "\n", i);
    }
}

lv_obj_t *ui_create_card_signal_row(lv_obj_t *card, const char *icon, const char *title,
                                    const char *detail, const char *value, lv_color_t color,
                                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = ui_create_card_item(card, icon, NULL, NULL);
    if (icon) lv_obj_set_style_text_color(lv_obj_get_child(row, 0), color, 0);

    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);   /* Taps go to the row */
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, title ? title : "");
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    /* One line each: a fixed height makes LVGL end a long name in dots. */
    const int32_t name_h = lv_font_get_line_height(lv_obj_get_style_text_font(row, LV_PART_MAIN));
    lv_obj_set_size(name, LV_PCT(100), name_h);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT_PRIMARY, 0);

    if (detail && detail[0]) {
        lv_obj_t *sub = lv_label_create(col);
        lv_label_set_text(sub, detail);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_size(sub, LV_PCT(100), lv_font_get_line_height(UI_FONT_CAPTION));
        lv_obj_set_style_text_font(sub, UI_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(sub, UI_COLOR_TEXT_SECONDARY, 0);
    }

    if (value) {
        lv_obj_t *val = lv_label_create(row);
        lv_label_set_text(val, value);
        lv_obj_set_style_text_color(val, color, 0);
    }

    /* Two lines in the row's usual height. */
    int32_t content_h = name_h;
    if (detail && detail[0]) content_h += 2 + lv_font_get_line_height(UI_FONT_CAPTION);
    int32_t pad = LV_MAX(8, (UI_ROW_MIN_H - content_h) / 2);
    lv_obj_set_style_pad_ver(row, pad, 0);

    lv_obj_set_style_bg_color(row, UI_COLOR_CARD_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

lv_obj_t *ui_create_card_icon_action(lv_obj_t *card, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *row = ui_create_card_action(card, symbol, symbol, cb);
    lv_obj_t *label = lv_obj_get_child(lv_obj_get_child(row, -1), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
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
    const char *hint;   /* Shown on a short tap; static text */
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

    hw_haptic(HW_HAPTIC_SUCCESS);
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
        if (was_holding) ui_toast(state->hint ? state->hint : "Hold to confirm");
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
    ui_require_hold_hint(obj, NULL);
}

void ui_require_hold_hint(lv_obj_t *obj, const char *hint)
{
    if (!obj) return;
    hold_state_t *state = (hold_state_t *)lv_malloc_zeroed(sizeof(hold_state_t));
    if (!state) return;
    state->obj = obj;
    state->hint = hint;
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

void ui_require_hold_hint(lv_obj_t *obj, const char *hint)
{
    LV_UNUSED(obj);
    LV_UNUSED(hint);
}

#endif /* FACTORY_WATCH_UI */
