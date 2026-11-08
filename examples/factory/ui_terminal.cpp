/**
 * @file      ui_terminal.cpp
 * @brief     On-device command console. Runs the same shell as the web Terminal — term_exec() in
 *            web_tool_term.cpp — so `ls`, `cat`, `status`, `cd`, `logs`, etc. behave identically.
 *            An output log fills the page; a one-line input drives the on-screen keyboard.
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>

/* The shared shell (web_tool_term.cpp). cwd is carried here between commands. It returns an exit
 * status the on-device Terminal does not display; the Scripts engine uses it. */
extern int term_exec(std::string &cwd, const std::string &line, std::string &out);

#define TERM_LOG_MAX   8192         /* trim the on-screen log to roughly this many bytes */

static lv_obj_t *page_container = NULL;
static lv_obj_t *out_cont = NULL;
static lv_obj_t *out_label = NULL;
static lv_obj_t *input_ta = NULL;
static lv_obj_t *keyboard = NULL;
static std::string term_cwd;
static std::string term_log;

static void log_scroll_bottom(void)
{
    if (!out_cont || !out_label) return;
    lv_obj_update_layout(out_label);
    lv_obj_scroll_to_y(out_cont, lv_obj_get_scroll_y(out_cont) + lv_obj_get_scroll_bottom(out_cont), LV_ANIM_OFF);
}

static void log_append(const std::string &text)
{
    term_log += text;
    if (term_log.size() > TERM_LOG_MAX) {
        size_t cut = term_log.find('\n', term_log.size() - TERM_LOG_MAX);
        term_log.erase(0, cut == std::string::npos ? term_log.size() - TERM_LOG_MAX : cut + 1);
    }
    if (out_label) lv_label_set_text(out_label, term_log.c_str());
    log_scroll_bottom();
}

static void run_line(const std::string &line)
{
    log_append(term_cwd + " $ " + line + "\n");
    /* first token */
    size_t a = line.find_first_not_of(" \t");
    std::string cmd = (a == std::string::npos) ? "" : line.substr(a, line.find_first_of(" \t", a) - a);
    if (cmd == "clear") { term_log.clear(); if (out_label) lv_label_set_text(out_label, ""); return; }
    if (cmd.empty()) return;
    std::string out;
    term_exec(term_cwd, line, out);
    if (!out.empty()) log_append(out);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {                       /* Enter / checkmark */
        const char *txt = lv_textarea_get_text(input_ta);
        std::string line = txt ? txt : "";
        lv_textarea_set_text(input_ta, "");
        run_line(line);
    }
}

static void back_event_handler(lv_event_t *e)
{
    LV_UNUSED(e);
    /* Objects are children of the page and are freed by the UI framework; just drop our handles.
     * term_cwd is kept so the directory survives re-opening; the visible log is reset. */
    out_cont = out_label = input_ta = keyboard = page_container = NULL;
    term_log.clear();
}

static void ui_terminal_enter(lv_obj_t *parent)
{
    if (term_cwd.empty()) term_cwd = "/sd";
    term_log = "T-Watch Ultra terminal. Type \"help\".\n";

    page_container = ui_create_app_page(parent, "Terminal", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);

    /* Scrollable output log. */
    out_cont = lv_obj_create(page_container);
    lv_obj_set_width(out_cont, lv_pct(100));
    lv_obj_set_flex_grow(out_cont, 1);
    lv_obj_set_style_pad_all(out_cont, 8, 0);
    lv_obj_set_style_radius(out_cont, 8, 0);
    out_label = lv_label_create(out_cont);
    lv_label_set_long_mode(out_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(out_label, lv_pct(100));
    lv_obj_set_style_text_font(out_label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(out_label, term_log.c_str());

    /* One-line input. */
    input_ta = lv_textarea_create(page_container);
    lv_obj_set_width(input_ta, lv_pct(100));
    lv_textarea_set_one_line(input_ta, true);
    lv_textarea_set_placeholder_text(input_ta, "command — try help");

    /* On-screen keyboard bound to the input. */
    keyboard = lv_keyboard_create(page_container);
    lv_obj_set_width(keyboard, lv_pct(100));
    lv_keyboard_set_textarea(keyboard, input_ta);
    lv_obj_add_event_cb(keyboard, kb_event_cb, LV_EVENT_READY, NULL);

    log_scroll_bottom();
}

static void ui_terminal_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    out_cont = out_label = input_ta = keyboard = page_container = NULL;
    term_log.clear();
}

app_t ui_terminal_main = {
    .setup_func_cb = ui_terminal_enter,
    .exit_func_cb = ui_terminal_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
