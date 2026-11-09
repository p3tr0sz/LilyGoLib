/**
 * @file      ui_totp.cpp
 * @brief     On-device Authenticator (TOTP) app. Shows each account's current RFC 6238 code and a
 *            seconds-remaining countdown, refreshed once a second, and lets you delete one. Codes are
 *            computed in the firmware (web_tool_totp.cpp) from the watch's clock; adding an account
 *            (entering a secret) is the web Authenticator app's job — the watch is where you read the
 *            code, mirroring the Scripts/Automation split.
 */
#include "ui_define.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* The account store + code computation (web_tool_totp.cpp). */
extern void totp_list(std::vector<std::string> &labels, std::vector<std::string> &codes, std::vector<int> &remaining);
extern bool totp_delete_index(int id);

static lv_obj_t *page_container = NULL;
static lv_obj_t *list_cont = NULL;
static lv_timer_t *refresh_timer = NULL;
static std::vector<std::string> acc_labels;
static std::vector<std::string> acc_codes;
static std::vector<int> acc_remaining;

static void rebuild_list(void);

static void delete_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (intptr_t)acc_labels.size()) return;
    totp_delete_index((int)idx);
    rebuild_list();
}

/* Group a code for readability: "123456" -> "123 456", "12345678" -> "1234 5678". */
static std::string group_code(const std::string &c)
{
    if (c.size() == 6) return c.substr(0, 3) + " " + c.substr(3);
    if (c.size() == 8) return c.substr(0, 4) + " " + c.substr(4);
    return c;
}

static void rebuild_list(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);
    totp_list(acc_labels, acc_codes, acc_remaining);

    if (acc_labels.empty()) {
        lv_obj_t *hint = lv_label_create(list_cont);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "No accounts yet.\nAdd one from the web Authenticator app;\ncodes then appear here.");
        return;
    }
    for (size_t i = 0; i < acc_labels.size(); i++) {
        lv_obj_t *row = lv_obj_create(list_cont);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_pad_column(row, 8, 0);

        lv_obj_t *body = lv_obj_create(row);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_height(body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(body, 4, 0);
        lv_obj_set_style_border_width(body, 0, 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_0, 0);

        lv_obj_t *name = lv_label_create(body);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, lv_pct(100));
        lv_label_set_text(name, acc_labels[i].c_str());

        lv_obj_t *code = lv_label_create(body);
        std::string line = group_code(acc_codes[i]) + "   " + std::to_string(acc_remaining[i]) + "s";
        lv_label_set_text(code, line.c_str());

        lv_obj_t *del = lv_btn_create(row);
        lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *del_lbl = lv_label_create(del);
        lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    rebuild_list();
}

static void cleanup(void)
{
    if (refresh_timer) { lv_timer_delete(refresh_timer); refresh_timer = NULL; }
    page_container = list_cont = NULL;
    acc_labels.clear();
    acc_codes.clear();
    acc_remaining.clear();
}

static void back_event_handler(lv_event_t *e)
{
    LV_UNUSED(e);
    cleanup();
}

static void ui_totp_enter(lv_obj_t *parent)
{
    page_container = ui_create_app_page(parent, "Authenticator", back_event_handler);
    lv_obj_set_flex_flow(page_container, LV_FLEX_FLOW_COLUMN);

    list_cont = lv_obj_create(page_container);
    lv_obj_set_width(list_cont, lv_pct(100));
    lv_obj_set_flex_grow(list_cont, 1);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_cont, 6, 0);

    rebuild_list();
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
}

static void ui_totp_exit(lv_obj_t *parent)
{
    LV_UNUSED(parent);
    cleanup();
}

app_t ui_totp_main = {
    .setup_func_cb = ui_totp_enter,
    .exit_func_cb = ui_totp_exit,
    .user_data = NULL,
};

#endif /* FACTORY_WEB */
