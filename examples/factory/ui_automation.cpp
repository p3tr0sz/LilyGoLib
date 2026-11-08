/**
 * @file      ui_automation.cpp
 * @brief     On-device Automation app. Lists the headless rules in /sd/automation and lets you
 *            enable/disable or delete one — the rule engine (web_tool_automation.cpp) keeps running
 *            them with no client connected. Authoring (building a rule) is the web Automation app's
 *            job; the watch is where you see and arm them, mirroring the Scripts/Terminal split.
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* The rule store + live engine (web_tool_automation.cpp). */
extern void automation_list_rules(std::vector<std::string> &ids, std::vector<std::string> &ons,
                                   std::vector<bool> &enabled);
extern bool automation_set_enabled(const std::string &id, bool enabled);
extern bool automation_delete_rule(const std::string &id);

static lv_obj_t *page_container = NULL;
static lv_obj_t *list_cont = NULL;
static std::vector<std::string> rule_ids;
static std::vector<std::string> rule_ons;
static std::vector<bool> rule_enabled;

static void rebuild_list(void);

/* Tap a rule row: toggle its enabled flag. */
static void toggle_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (intptr_t)rule_ids.size()) return;
    automation_set_enabled(rule_ids[idx], !rule_enabled[idx]);
    rebuild_list();
}

/* The trash button on a row: delete that rule. */
static void delete_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (intptr_t)rule_ids.size()) return;
    automation_delete_rule(rule_ids[idx]);
    rebuild_list();
}

static void rebuild_list(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);
    automation_list_rules(rule_ids, rule_ons, rule_enabled);

    if (rule_ids.empty()) {
        lv_obj_t *hint = lv_label_create(list_cont);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "No rules in /sd/automation.\nCreate rules from the web Automation app;\nthey run here with nothing connected.");
        return;
    }
    for (size_t i = 0; i < rule_ids.size(); i++) {
        lv_obj_t *row = lv_obj_create(list_cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_pad_column(row, 8, 0);

        /* Tapping the row body toggles enabled. */
        lv_obj_t *body = lv_btn_create(row);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_add_event_cb(body, toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(body, 6, 0);

        lv_obj_t *name = lv_label_create(body);
        std::string title = (rule_enabled[i] ? LV_SYMBOL_OK " " : LV_SYMBOL_CLOSE " ") + rule_ids[i];
        lv_label_set_text(name, title.c_str());

        lv_obj_t *sub = lv_label_create(body);
        std::string on = "on " + (rule_ons[i].empty() ? std::string("?") : rule_ons[i]) +
                         (rule_enabled[i] ? "" : "  (disabled)");
        lv_label_set_text(sub, on.c_str());
        lv_obj_set_style_text_font(sub, LV_FONT_DEFAULT, 0);

        /* A trash button to delete. */
        lv_obj_t *del = lv_btn_create(row);
        lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
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
    page_container = list_cont = NULL;
    rule_ids.clear();
    rule_ons.clear();
    rule_enabled.clear();
}

static void ui_automation_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "Automation", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *reload = lv_btn_create(page_container);
    lv_obj_add_event_cb(reload, reload_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reload_lbl = lv_label_create(reload);
    lv_label_set_text(reload_lbl, LV_SYMBOL_REFRESH " Reload");

    list_cont = lv_obj_create(page_container);
    lv_obj_set_width(list_cont, lv_pct(100));
    lv_obj_set_flex_grow(list_cont, 1);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_cont, 6, 0);

    rebuild_list();
}

static void ui_automation_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    page_container = list_cont = NULL;
    rule_ids.clear();
    rule_ons.clear();
    rule_enabled.clear();
}

app_t ui_automation_main = {
    .setup_func_cb = ui_automation_enter,
    .exit_func_cb = ui_automation_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
