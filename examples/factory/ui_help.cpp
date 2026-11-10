/**
 * @file      ui_help.cpp
 * @brief     On-device Help app. Shows a short "what this does" blurb for each app, read from the
 *            shared registry /sd/web/help.txt — the very same file the web desktop's "?" button uses,
 *            so the two can never drift. Read-only; authoring the blurbs is editing that one file.
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* Reads /sd/web/help.txt (web_fs.cpp). */
extern bool web_help_read(std::string &out);

static lv_obj_t *page_container = NULL;
static lv_obj_t *list_cont = NULL;

/* Pull the tab-separated fields out of one line: id<TAB>title<TAB>blurb. */
static bool parse_line(const std::string &line, std::string &title, std::string &blurb)
{
    if (line.empty() || line[0] == '#') return false;
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) return false;
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) return false;
    title = line.substr(t1 + 1, t2 - t1 - 1);
    blurb = line.substr(t2 + 1);
    return !title.empty() && !blurb.empty();
}

static void rebuild_list(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);

    std::string text;
    if (!web_help_read(text)) {
        lv_obj_t *hint = lv_label_create(list_cont);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "Help is unavailable.\nInsert the SD card with the web desktop installed\n(it ships /sd/web/help.txt).");
        return;
    }

    size_t pos = 0;
    int shown = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        std::string title, blurb;
        if (!parse_line(line, title, blurb)) continue;

        lv_obj_t *card = lv_obj_create(list_cont);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 2, 0);

        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, title.c_str());

        lv_obj_t *desc = lv_label_create(card);
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc, lv_pct(100));
        lv_label_set_text(desc, blurb.c_str());
        shown++;
    }

    if (!shown) {
        lv_obj_t *hint = lv_label_create(list_cont);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "No help entries found.");
    }
}

static void back_event_handler(lv_event_t *e)
{
    LV_UNUSED(e);
    page_container = list_cont = NULL;
}

static void ui_help_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "Help", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);

    list_cont = lv_obj_create(page_container);
    lv_obj_set_width(list_cont, lv_pct(100));
    lv_obj_set_flex_grow(list_cont, 1);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_cont, 6, 0);

    rebuild_list();
}

static void ui_help_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    page_container = list_cont = NULL;
}

app_t ui_help_main = {
    .setup_func_cb = ui_help_enter,
    .exit_func_cb = ui_help_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
