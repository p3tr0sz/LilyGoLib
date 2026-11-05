/**
 * @file      web_tool_hid.cpp
 * @brief     USB HID keyboard (docs/web-api.md, "USB HID"): type text and run DuckyScript, the
 *            same subset the watch's BadUSB app runs. The parser is here (portable); the keys are
 *            typed through web_hw_hid_*. A script runs on its own task after a 2 s arming wait.
 *
 *            For the computer the watch is plugged into, with permission.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

#define ARM_MS          2000
#define MAX_SCRIPT      16384
#define MAX_LINE        512

static const char *const APPS[] = { "BadUSB", NULL };

static std::mutex s_mx;
static std::string s_state = "idle";    /* idle, armed, running */
static std::string s_script_name;
static int s_line = 0, s_lines = 0;
static std::atomic<bool> s_stop { false };
static std::atomic<bool> s_busy { false };
static std::string s_text;              /* the script the task runs */

static std::string hid_json(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    std::string j = std::string("{\"available\":") + (web_hw_hid_available() ? "true" : "false") + ",\"state\":";
    web_json_str(j, s_state.c_str());
    j += ",\"script\":";
    if (s_script_name.empty()) j += "null"; else web_json_str(j, s_script_name.c_str());
    return j + ",\"line\":" + tools_int(s_line) + ",\"lines\":" + tools_int(s_lines) + "}";
}

static void set_state(const char *state, int line)
{
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_state = state;
        if (line >= 0) s_line = line;
    }
    web_events_publish("hid", hid_json().c_str());
}

/* ---------------------------------------------------------------- DuckyScript */

static int modifier_of(const char *word)
{
    if (!strcasecmp(word, "CTRL") || !strcasecmp(word, "CONTROL")) return WEB_HID_CTRL;
    if (!strcasecmp(word, "SHIFT")) return WEB_HID_SHIFT;
    if (!strcasecmp(word, "ALT")) return WEB_HID_ALT;
    if (!strcasecmp(word, "GUI") || !strcasecmp(word, "WINDOWS") || !strcasecmp(word, "WIN") || !strcasecmp(word, "COMMAND")) return WEB_HID_GUI;
    return 0;
}

/* A named key the hardware understands, or the single character to type with the modifiers. */
static void run_combo(char *line)
{
    int mods = 0;
    char *key = NULL;
    for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
        int m = modifier_of(tok);
        if (m) mods |= m;
        else key = tok;                 /* the last non-modifier word is the key */
    }
    if (key) web_hw_hid_key(key, mods);
}

/* One script line. Returns the delay to wait after it (ms). */
static uint32_t run_line(char *line, uint32_t *default_delay)
{
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || line[0] == '#' || !strncmp(line, "REM", 3)) return 0;

    char *sp = strpbrk(line, " \t");
    std::string cmd(line, sp ? (size_t)(sp - line) : strlen(line));
    char *arg = sp ? sp + 1 : (char *)"";

    if (!strcasecmp(cmd.c_str(), "STRING")) {
        web_hw_hid_text(arg);
    } else if (!strcasecmp(cmd.c_str(), "DELAY")) {
        return (uint32_t)strtoul(arg, NULL, 10);
    } else if (!strcasecmp(cmd.c_str(), "DEFAULT_DELAY") || !strcasecmp(cmd.c_str(), "DEFAULTDELAY")) {
        *default_delay = (uint32_t)strtoul(arg, NULL, 10);
    } else {
        run_combo(line);                /* ENTER, TAB, GUI r, CTRL ALT DELETE, ... */
    }
    return 0;
}

static void hid_task(void *arg)
{
    (void)arg;
    std::string text;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        text = s_text;
    }
    set_state("armed", 0);
    for (int i = 0; i < ARM_MS / 50 && !s_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));

    uint32_t default_delay = 0;
    int line_no = 0;
    size_t pos = 0;
    while (pos < text.size() && !s_stop) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ')) raw.pop_back();
        line_no++;
        set_state("running", line_no);
        char buf[MAX_LINE];
        snprintf(buf, sizeof(buf), "%s", raw.c_str());
        uint32_t d = run_line(buf, &default_delay);
        uint32_t wait = d ? d : default_delay;
        for (uint32_t w = 0; w < wait && !s_stop; w += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    web_hw_hid_release();
    set_state("idle", -1);
    s_busy = false;
}

/* ---------------------------------------------------------------- routes */

static void type_route(web_req_t *r)
{
    std::string body, text;
    if (!tools_read_json(r, body, 2048)) return;
    if (!web_json_get_str(body, "text", text) || text.empty() || text.size() > 1024) {
        web_resp_error(r, 400, "bad_request", "text: 1-1024 chars");
        return;
    }
    if (tools_busy(r, APPS)) return;
    bool ok = false;
    web_ui_run([&] { if (web_hw_hid_available()) ok = web_hw_hid_text(text.c_str()); });
    if (!ok) {
        web_resp_error(r, 503, "hid_unavailable", "plug the watch into a computer first");
        return;
    }
    web_resp_ok(r);
}

static int count_lines(const std::string &s)
{
    int n = s.empty() ? 0 : 1;
    for (char c : s) if (c == '\n') n++;
    return n;
}

static void run_route(web_req_t *r)
{
    std::string body, text, path, name;
    if (!tools_read_json(r, body, MAX_SCRIPT + 256)) return;
    if (web_json_get_str(body, "text", text)) {
        name = "(inline)";
    } else if (web_json_get_str(body, "path", path)) {
        std::string host;
        int st = web_fs_resolve(path.c_str(), host);
        if (st == 503) {
            web_resp_error(r, 503, "sd_missing", "SD card not mounted");
            return;
        }
        if (st) {
            web_resp_error(r, 400, "bad_request", "bad path");
            return;
        }
        web_plat_io_lock();
        FILE *f = fopen(host.c_str(), "rb");
        if (f) {
            char b[1024];
            size_t n;
            while ((n = fread(b, 1, sizeof(b), f)) && text.size() < MAX_SCRIPT) text.append(b, n);
            fclose(f);
        }
        web_plat_io_unlock();
        if (!f) {
            web_resp_error(r, 404, "not_found");
            return;
        }
        const char *slash = strrchr(path.c_str(), '/');
        name = slash ? slash + 1 : path;
    } else {
        web_resp_error(r, 400, "bad_request", "path or text");
        return;
    }
    if (text.empty() || text.size() > MAX_SCRIPT) {
        web_resp_error(r, 400, "bad_request", "empty or too large");
        return;
    }
    if (tools_busy(r, APPS)) return;
    bool avail = false;
    web_ui_run([&] { avail = web_hw_hid_available(); });
    if (!avail) {
        web_resp_error(r, 503, "hid_unavailable", "plug the watch into a computer first");
        return;
    }
    if (s_busy.exchange(true)) {
        web_resp_error(r, 409, "conflict", "a script is already running");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_text = text;
        s_script_name = name;
        s_line = 0;
        s_lines = count_lines(text);
    }
    s_stop = false;
    web_plat_spawn(hid_task, NULL);
    web_plat_log("[web] HID run %s (%d lines)", name.c_str(), count_lines(text));
    web_resp_json(r, 202, "{\"ok\":true}");
}

static void hid_stop_tool(void)
{
    s_stop = true;
}

#ifndef ARDUINO
static void dev_hid(web_req_t *r)
{
    std::string body;
    bool on = true;
    web_read_body(r, body, 128);
    web_json_get_bool(body, "available", &on);
    web_ui_run([&] { web_hw_sim_hid_available(on); });
    web_events_publish("hid", hid_json().c_str());
    web_resp_ok(r);
}
#endif

static bool hid_handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/hid")) return dev_hid(r), true;
#endif
    if (strcmp(r->path, "/api/hid") && strncmp(r->path, "/api/hid/", 9)) return false;
    if (tools_is(r, "GET", "/api/hid")) web_resp_json(r, 200, hid_json());
    else if (tools_is(r, "POST", "/api/hid/type")) type_route(r);
    else if (tools_is(r, "POST", "/api/hid/run")) run_route(r);
    else if (tools_is(r, "POST", "/api/hid/stop")) {
        hid_stop_tool();
        web_resp_ok(r);
    } else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "usb.hid", NULL };
const web_tool_t web_tool_hid = { CAPS, hid_handle, NULL, NULL, hid_stop_tool };

#endif /* FACTORY_WEB */
