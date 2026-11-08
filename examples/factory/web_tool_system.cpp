/**
 * @file      web_tool_system.cpp
 * @brief     Monitor, logs and notifications (docs/web-api.md, "Monitor", "Notifications").
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdlib.h>
#include <deque>
#include <mutex>

#define LOG_LINES       200
#define NOTIFY_KEEP     20

/* ---------------------------------------------------------------- logs */

struct log_entry_t { uint32_t n, ms; std::string text; };
static std::mutex s_log_mx;
static std::deque<log_entry_t> s_log;
static uint32_t s_log_n = 0;

void web_log_add(const char *line)
{
    std::lock_guard<std::mutex> lk(s_log_mx);
    s_log.push_back({ ++s_log_n, tools_now_ms(), line ? line : "" });
    while (s_log.size() > LOG_LINES) s_log.pop_front();
}

/* The last n console lines, oldest first, one per line — for the Terminal `logs` command. */
void web_log_tail(int n, std::string &out)
{
    std::lock_guard<std::mutex> lk(s_log_mx);
    size_t start = ((int)s_log.size() > n) ? s_log.size() - n : 0;
    for (size_t i = start; i < s_log.size(); i++) { out += s_log[i].text; out += '\n'; }
    if (s_log.empty()) out += "(no log lines yet)\n";
}

static void get_logs(web_req_t *r)
{
    char b[24];
    unsigned long after = 0;
    if (web_query_get(r, "after", b, sizeof(b))) after = strtoul(b, NULL, 10);
    std::string j = "{\"lines\":[";
    std::lock_guard<std::mutex> lk(s_log_mx);
    bool first = true;
    for (const log_entry_t &l : s_log) {
        if (l.n <= after) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"n\":" + tools_int(l.n) + ",\"ms\":" + tools_int(l.ms) + ",\"text\":";
        web_json_str(j, l.text.c_str());
        j += "}";
    }
    web_resp_json(r, 200, j + "],\"next\":" + tools_int(s_log_n) + "}");
}

/* ---------------------------------------------------------------- monitor */

static void get_monitor(web_req_t *r)
{
    web_dev_info_t d;
    web_hw_monitor_t m;
    web_ui_run([&] {
        web_plat_dev_info(&d);
        web_hw_monitor(&m);
    });
    std::string j = "{\"uptime_s\":" + tools_int(tools_now_ms() / 1000) + ",\"cpu_mhz\":" + tools_int(m.cpu_mhz) + ",\"app\":";
    web_json_str(j, tools_current_app());
    j += ",\"battery\":{\"percent\":" + tools_int(d.percent) + ",\"charging\":" + (d.charging ? "true" : "false") +
         ",\"mv\":" + tools_int(d.mv) + ",\"ma\":" + (m.has_ma ? tools_int(m.ma) : "null") +
         ",\"temp_c\":" + (m.has_temp ? tools_num(m.temp_c, 1) : "null") +
         ",\"vbus_mv\":" + (m.has_vbus ? tools_int(m.vbus_mv) : "null") + "}";
    j += ",\"mem\":{\"heap_free\":" + tools_int(d.heap_free) + ",\"heap_min\":" + tools_int(m.heap_min) +
         ",\"heap_total\":" + tools_int(m.heap_total) + ",\"psram_free\":" + tools_int(d.psram_free) +
         ",\"psram_total\":" + tools_int(m.psram_total) + "}}";
    web_resp_json(r, 200, j);
}

static bool monitor_handle(web_req_t *r)
{
    if (tools_is(r, "GET", "/api/monitor")) return get_monitor(r), true;
    if (tools_is(r, "GET", "/api/logs")) return get_logs(r), true;
    return false;
}

static const char *const MONITOR_CAPS[] = { "monitor", NULL };
const web_tool_t web_tool_monitor = { MONITOR_CAPS, monitor_handle, NULL, NULL, NULL };

/* ---------------------------------------------------------------- notifications */

struct notification_t { std::string id, title, body, level; long long ts; };
static std::mutex s_note_mx;
static std::deque<notification_t> s_notes;      /* newest first */
static uint32_t s_note_seq = 0;

static std::string note_json(const notification_t &n)
{
    std::string j = "{\"id\":";
    web_json_str(j, n.id.c_str());
    j += ",\"title\":";
    web_json_str(j, n.title.c_str());
    j += ",\"body\":";
    web_json_str(j, n.body.c_str());
    j += ",\"level\":";
    web_json_str(j, n.level.c_str());
    return j + ",\"ts\":" + tools_int(n.ts) + "}";
}

/* Check a POST /api/notify body into n. Answers 400 and returns false when it is wrong. */
static bool parse_note(web_req_t *r, const std::string &body, notification_t &n, bool &vibrate)
{
    if (!web_json_get_str(body, "title", n.title) || n.title.empty() || n.title.size() > 64) {
        web_resp_error(r, 400, "bad_request", "title: 1-64 bytes");
        return false;
    }
    if (web_json_has(body, "body") && (!web_json_get_str(body, "body", n.body) || n.body.size() > 512)) {
        web_resp_error(r, 400, "bad_request", "body: up to 512 bytes");
        return false;
    }
    n.level = "info";
    if (web_json_has(body, "level") &&
            (!web_json_get_str(body, "level", n.level) || (n.level != "info" && n.level != "warn" && n.level != "alert"))) {
        web_resp_error(r, 400, "bad_request", "level: info, warn or alert");
        return false;
    }
    if (web_json_has(body, "id") && (!web_json_get_str(body, "id", n.id) || n.id.empty() || n.id.size() > 32)) {
        web_resp_error(r, 400, "bad_request", "id: 1-32 bytes");
        return false;
    }
    vibrate = true;
    if (web_json_has(body, "vibrate") && !web_json_get_bool(body, "vibrate", &vibrate)) {
        web_resp_error(r, 400, "bad_request", "vibrate");
        return false;
    }
    return true;
}

static void post_notify(web_req_t *r)
{
    std::string body;
    notification_t n;
    bool vibrate;
    if (!tools_read_json(r, body, 4096) || !parse_note(r, body, n, vibrate)) return;
    n.ts = tools_epoch();
    {
        std::lock_guard<std::mutex> lk(s_note_mx);
        if (n.id.empty()) n.id = "n" + tools_int(++s_note_seq);
        for (auto it = s_notes.begin(); it != s_notes.end(); ++it) {
            if (it->id == n.id) {
                s_notes.erase(it);          /* the same id replaces */
                break;
            }
        }
        s_notes.push_front(n);
        while (s_notes.size() > NOTIFY_KEEP) s_notes.pop_back();
    }
    int level = n.level == "alert" ? 2 : n.level == "warn" ? 1 : 0;
    web_ui_run([&] { web_hw_notify(n.title.c_str(), n.body.c_str(), level, vibrate); });
    web_plat_log("[web] notification: %s", n.title.c_str());
    web_events_publish("notify", note_json(n).c_str());
    std::string extra = "\"id\":";
    web_json_str(extra, n.id.c_str());
    web_resp_ok(r, extra.c_str());
}

static void get_notify(web_req_t *r)
{
    std::string j = "{\"items\":[";
    std::lock_guard<std::mutex> lk(s_note_mx);
    for (size_t i = 0; i < s_notes.size(); i++) {
        if (i) j += ",";
        j += note_json(s_notes[i]);
    }
    web_resp_json(r, 200, j + "]}");
}

static bool notify_handle(web_req_t *r)
{
    if (tools_is(r, "POST", "/api/notify")) return post_notify(r), true;
    if (tools_is(r, "GET", "/api/notify")) return get_notify(r), true;
    return false;
}

static const char *const NOTIFY_CAPS[] = { "notify", NULL };
const web_tool_t web_tool_notify = { NOTIFY_CAPS, notify_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
