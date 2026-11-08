/**
 * @file      web_tool_term.cpp
 * @brief     A small command shell shared by the web Terminal app (POST /api/term) and the native
 *            on-device Terminal (ui_terminal.cpp). One command set, two thin front-ends.
 *
 * The dispatcher is a pure function: term_exec(cwd, line) -> output text, with cwd carried by the
 * caller (the web app keeps it per browser; the native app keeps it in the page). Commands read the
 * filesystem through web_fs_resolve() so they are sandboxed to /sd and /flash exactly like the Files
 * app. Nothing here changes radio or attack state; it is a status/filesystem console.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "web_internal.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Arduino.h>
#endif

/* Recent console lines, exposed by web_tool_system.cpp for the `logs` command. */
void web_log_tail(int n, std::string &out);

#define TERM_OUT_MAX   (32 * 1024)      /* cap one command's output */
#define TERM_CAT_MAX   (16 * 1024)      /* cap a single `cat` */

static void out_line(std::string &out, const std::string &s)
{
    if (out.size() < TERM_OUT_MAX) { out += s; out += '\n'; }
}

/* Collapse "." and ".." and empty segments; `arg` is absolute if it starts with '/', else relative
 * to cwd. Result is always rooted ("/", "/sd", "/sd/dir", ...). */
static std::string norm_path(const std::string &cwd, const std::string &arg)
{
    std::string p = arg.empty() ? cwd : (arg[0] == '/' ? arg : cwd + "/" + arg);
    std::vector<std::string> stack;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == ".") { /* skip */ }
        else if (seg == "..") { if (!stack.empty()) stack.pop_back(); }
        else stack.push_back(seg);
        i = j + 1;
    }
    std::string r = "/";
    for (size_t k = 0; k < stack.size(); k++) { r += stack[k]; if (k + 1 < stack.size()) r += "/"; }
    return r;
}

/* The two filesystem roots the watch mounts, shown when listing "/". */
static void list_roots(std::string &out)
{
    out_line(out, "flash/");
    out_line(out, "sd/");
}

static void cmd_ls(const std::string &cwd, const std::string &arg, std::string &out)
{
    std::string api = norm_path(cwd, arg);
    if (api == "/") { list_roots(out); return; }
    std::string host;
    int e = web_fs_resolve(api.c_str(), host);
    if (e) { out_line(out, "ls: " + api + ": " + (e == 503 ? "not mounted" : "no such path")); return; }
    struct stat st;
    if (stat(host.c_str(), &st) != 0) { out_line(out, "ls: " + api + ": no such path"); return; }
    if (!S_ISDIR(st.st_mode)) { out_line(out, api); return; }         /* a file: echo it, like ls of a file */
    DIR *d = opendir(host.c_str());
    if (!d) { out_line(out, "ls: " + api + ": cannot open"); return; }
    std::vector<std::string> names;
    while (struct dirent *ent = readdir(d)) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        std::string full = host + "/" + ent->d_name;
        struct stat es;
        bool dir = (stat(full.c_str(), &es) == 0) && S_ISDIR(es.st_mode);
        long sz = dir ? 0 : ((stat(full.c_str(), &es) == 0) ? (long)es.st_size : 0);
        names.push_back(dir ? (std::string(ent->d_name) + "/")
                            : (std::string(ent->d_name) + "\t" + tools_int(sz) + " B"));
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    if (names.empty()) out_line(out, "(empty)");
    for (const std::string &n : names) out_line(out, n);
}

static void cmd_cat(const std::string &cwd, const std::string &arg, std::string &out)
{
    if (arg.empty()) { out_line(out, "cat: missing file"); return; }
    std::string api = norm_path(cwd, arg), host;
    int e = web_fs_resolve(api.c_str(), host);
    if (e) { out_line(out, "cat: " + api + ": " + (e == 503 ? "not mounted" : "no such file")); return; }
    struct stat st;
    if (stat(host.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) { out_line(out, "cat: " + api + ": not a file"); return; }
    FILE *f = fopen(host.c_str(), "rb");
    if (!f) { out_line(out, "cat: " + api + ": cannot open"); return; }
    std::string buf;
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.append(chunk, n);
        if (buf.size() >= TERM_CAT_MAX) { buf.resize(TERM_CAT_MAX); break; }
    }
    long total = st.st_size;
    fclose(f);
    if (out.size() + buf.size() < TERM_OUT_MAX) out += buf;
    if (!buf.empty() && buf.back() != '\n') out += '\n';
    if (total > (long)TERM_CAT_MAX) out_line(out, "… (truncated at " + tools_int(TERM_CAT_MAX) + " B of " + tools_int(total) + ")");
}

static void cmd_status(std::string &out)
{
    web_dev_info_t d;
    web_hw_monitor_t m;
    web_ui_run([&] { web_plat_dev_info(&d); web_hw_monitor(&m); });
    const char *app = tools_current_app();
    out_line(out, "uptime   " + tools_int(tools_now_ms() / 1000) + " s");
    out_line(out, "cpu      " + tools_int(m.cpu_mhz) + " MHz");
    out_line(out, std::string("app      ") + (app && *app ? app : "(home)"));
    out_line(out, "battery  " + tools_int(d.percent) + "%  " + (d.charging ? "charging" : "discharging") + "  " + tools_int(d.mv) + " mV");
    out_line(out, "wifi     " + std::string(d.mode) + (d.ssid[0] ? std::string("  ") + d.ssid + "  " + tools_int(d.rssi) + " dBm" : ""));
    out_line(out, "heap     " + tools_int(d.heap_free) + " free / " + tools_int(m.heap_total) + " B");
}

static void cmd_free(std::string &out)
{
    web_dev_info_t d;
    web_hw_monitor_t m;
    web_ui_run([&] { web_plat_dev_info(&d); web_hw_monitor(&m); });
    out_line(out, "heap   " + tools_int(d.heap_free) + " free  " + tools_int(m.heap_min) + " min  " + tools_int(m.heap_total) + " total");
    out_line(out, "psram  " + tools_int(d.psram_free) + " free  " + tools_int(m.psram_total) + " total");
}

static const char *const TERM_HELP[] = {
    "help            this list",
    "echo TEXT       print TEXT",
    "clear           clear the screen",
    "pwd             working directory",
    "cd [PATH]       change directory (/sd, /flash)",
    "ls [PATH]       list a directory",
    "cat FILE        print a file (first 16 KB)",
    "status          uptime, cpu, app, battery, wifi, heap",
    "free            heap and psram",
    "uptime          seconds since boot",
    "date            device clock (UTC epoch)",
    "ver             firmware version and board",
    "logs [N]        last N console lines (default 20)",
    "reboot          restart the watch",
    NULL,
};

/* Run one command line. cwd is updated by `cd`. Returns the output text. clear is handled by the
 * front-ends, so it never reaches here. */
void term_exec(std::string &cwd, const std::string &line, std::string &out)
{
    if (cwd.empty()) cwd = "/sd";
    /* split into command + rest */
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos) return;
    size_t b = line.find_first_of(" \t", a);
    std::string cmd = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
    std::string rest = (b == std::string::npos) ? "" : line.substr(line.find_first_not_of(" \t", b) == std::string::npos ? line.size() : line.find_first_not_of(" \t", b));

    if (cmd == "help" || cmd == "?") { for (const char *const *h = TERM_HELP; *h; h++) out_line(out, *h); }
    else if (cmd == "echo") out_line(out, rest);
    else if (cmd == "pwd") out_line(out, cwd);
    else if (cmd == "cd") {
        std::string api = norm_path(cwd, rest);
        if (api == "/") { cwd = "/"; return; }
        std::string host;
        int e = web_fs_resolve(api.c_str(), host);
        struct stat st;
        if (e || stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) out_line(out, "cd: " + api + ": no such directory");
        else cwd = api;
    }
    else if (cmd == "ls" || cmd == "dir") cmd_ls(cwd, rest, out);
    else if (cmd == "cat" || cmd == "type") cmd_cat(cwd, rest, out);
    else if (cmd == "status") cmd_status(out);
    else if (cmd == "free") cmd_free(out);
    else if (cmd == "uptime") out_line(out, tools_int(tools_now_ms() / 1000) + " s");
    else if (cmd == "date") out_line(out, tools_int(tools_epoch()) + " (UTC epoch)");
    else if (cmd == "ver" || cmd == "version") out_line(out, std::string(web_plat_fw_version()) + "  twatch-ultra");
    else if (cmd == "logs") {
        int n = 20;
        if (!rest.empty()) { int v = atoi(rest.c_str()); if (v > 0 && v <= 200) n = v; }
        web_log_tail(n, out);
    }
    else if (cmd == "reboot") {
        out_line(out, "Rebooting…");
        web_ui_run([] { web_plat_reboot(); });
    }
    else if (cmd == "clear") { /* front-ends handle this; ignore if it slips through */ }
    else out_line(out, cmd + ": command not found (try help)");
}

/* ---------------------------------------------------------------- web endpoint */

static bool term_handle(web_req_t *r)
{
    if (!tools_is(r, "POST", "/api/term")) return false;
    std::string body;
    if (!tools_read_json(r, body, 1024)) return true;
    std::string line, cwd;
    web_json_get_str(body, "line", line);
    web_json_get_str(body, "cwd", cwd);
    if (cwd.empty()) cwd = "/sd";
    std::string out;
    term_exec(cwd, line, out);
    std::string j = "{\"cwd\":";
    web_json_str(j, cwd.c_str());
    j += ",\"output\":";
    web_json_str(j, out.c_str());
    j += "}";
    web_resp_json(r, 200, j);
    return true;
}

static const char *const TERM_CAPS[] = { "term", NULL };
const web_tool_t web_tool_term = { TERM_CAPS, term_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
