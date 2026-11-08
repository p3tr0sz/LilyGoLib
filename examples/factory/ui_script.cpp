/**
 * @file      ui_script.cpp
 * @brief     On-device Scripts app. Lists the .scr files in /sd/scripts and runs the one you tap
 *            through the same engine as the web Scripts app — script_run() in web_tool_script.cpp —
 *            so a script behaves identically on the watch and in the browser. Output fills a log.
 *
 *            Editing is done from the web Scripts app (an on-screen keyboard is a poor editor); the
 *            watch is the runner. This mirrors the Terminal split (ui_terminal.cpp / web_tool_term).
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* The shared engine and the script library on /sd (web_tool_script.cpp). */
extern void script_run(std::string &cwd, const std::string &src, std::string &out);
extern void script_list_names(std::vector<std::string> &out);
extern bool script_read_file(const std::string &name, std::string &src);

#define SCRIPT_LOG_MAX  8192

static lv_obj_t *page_container = NULL;
static lv_obj_t *list_cont = NULL;
static lv_obj_t *out_cont = NULL;
static lv_obj_t *out_label = NULL;
static std::string script_cwd;
static std::string script_log;
static std::vector<std::string> script_names;

static void log_scroll_bottom(void)
{
    if (!out_cont || !out_label) return;
    lv_obj_update_layout(out_label);
    lv_obj_scroll_to_y(out_cont, lv_obj_get_scroll_y(out_cont) + lv_obj_get_scroll_bottom(out_cont), LV_ANIM_OFF);
}

static void log_append(const std::string &text)
{
    script_log += text;
    if (script_log.size() > SCRIPT_LOG_MAX) {
        size_t cut = script_log.find('\n', script_log.size() - SCRIPT_LOG_MAX);
        script_log.erase(0, cut == std::string::npos ? script_log.size() - SCRIPT_LOG_MAX : cut + 1);
    }
    if (out_label) lv_label_set_text(out_label, script_log.c_str());
    log_scroll_bottom();
}

static void run_named(const std::string &name)
{
    std::string src;
    if (!script_read_file(name, src)) { log_append("script: cannot read " + name + "\n"); return; }
    log_append(std::string("$ run ") + name + "\n");
    std::string out;
    script_run(script_cwd, src, out);
    if (!out.empty()) log_append(out);
    if (out.empty() || out.back() != '\n') log_append("\n");
}

static void script_btn_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < (intptr_t)script_names.size()) run_named(script_names[idx]);
}

static void rebuild_list(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);
    script_list_names(script_names);

    if (script_names.empty()) {
        lv_obj_t *hint = lv_label_create(list_cont);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "No scripts in /sd/scripts.\nCreate .scr files from the web Scripts app.");
        return;
    }
    for (size_t i = 0; i < script_names.size(); i++) {
        lv_obj_t *btn = lv_btn_create(list_cont);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_add_event_cb(btn, script_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, script_names[i].c_str());
    }
}

static void reload_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    rebuild_list();
}

static void back_event_handler(lv_event_t *e)
{
    LV_UNUSED(e);
    page_container = list_cont = out_cont = out_label = NULL;
    script_log.clear();
    script_names.clear();
}

static void ui_script_enter(lv_obj_t *parent)
{
    if (script_cwd.empty()) script_cwd = "/sd";
    script_log = "Scripts. Tap a script to run it.\n";

    page_container = ui_create_app_page(parent, "Scripts", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);

    /* A reload button. */
    lv_obj_t *reload = lv_btn_create(page_container);
    lv_obj_add_event_cb(reload, reload_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reload_lbl = lv_label_create(reload);
    lv_label_set_text(reload_lbl, LV_SYMBOL_REFRESH " Reload");

    /* The list of scripts. */
    list_cont = lv_obj_create(page_container);
    lv_obj_set_width(list_cont, lv_pct(100));
    lv_obj_set_flex_grow(list_cont, 1);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_cont, 6, 0);

    /* The output log. */
    out_cont = lv_obj_create(page_container);
    lv_obj_set_width(out_cont, lv_pct(100));
    lv_obj_set_flex_grow(out_cont, 1);
    lv_obj_set_style_pad_all(out_cont, 8, 0);
    lv_obj_set_style_radius(out_cont, 8, 0);
    out_label = lv_label_create(out_cont);
    lv_label_set_long_mode(out_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(out_label, lv_pct(100));
    lv_obj_set_style_text_font(out_label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(out_label, script_log.c_str());

    rebuild_list();
    log_scroll_bottom();
}

static void ui_script_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    page_container = list_cont = out_cont = out_label = NULL;
    script_log.clear();
    script_names.clear();
}

app_t ui_script_main = {
    .setup_func_cb = ui_script_enter,
    .exit_func_cb = ui_script_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
