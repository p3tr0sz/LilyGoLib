/**
 * @file      web_tool_automation.cpp
 * @brief     The Automation app: a small headless rule engine (trigger -> condition -> action).
 *
 * Replaces the earlier Berry interpreter (disconnected in a7541d7; Berry source kept in-tree for now).
 * A watch automation must run with nothing connected -- "buzz when the battery is low", "launch an app
 * on a button hold" -- so the executor lives in firmware, not in the browser's JS. What that needs is
 * not a Turing-complete language but trigger -> condition -> action, and we already have the event bus
 * (web_events_publish) and the verbs (term_exec, script_run, notify, the cap-gated API). See
 * docs/automation-rule-engine.md.
 *
 * Rules are small JSON objects stored one per file in /sd/automation/<id>.json. This slice implements
 * the CRUD + storage plane (list/read/save/delete), mirrored by the JS mock. The matcher -- an
 * in-process sink on the event tap (web_events_subscribe), drained on the UI tick, matching rules and
 * firing bounded action chains -- lands in the next slice, with /api/automation/test. Because the
 * matcher is pure logic (unlike the native Berry VM), the mock can mirror it, so dual-plane testing is
 * back.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "web_internal.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#define AUTO_DIR       "/sd/automation"
#define AUTO_RULE_MAX  (8 * 1024)      /* largest rule JSON accepted / stored */

/* A rule id is the file stem (<id>.json): a short, filesystem-safe token. */
static bool auto_id_ok(const std::string &id)
{
    if (id.size() < 1 || id.size() > 48) return false;
    for (char c : id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    if (id.find("..") != std::string::npos) return false;
    return true;
}

/* Read a rule file's whole text (sandboxed to AUTO_DIR). false if the id is bad or unreadable. */
static bool auto_read_file(const std::string &id, std::string &text)
{
    text.clear();
    if (!auto_id_ok(id)) return false;
    std::string host;
    if (web_fs_resolve(std::string(AUTO_DIR "/" + id + ".json").c_str(), host) != 0) return false;
    FILE *f = fopen(host.c_str(), "rb");
    if (!f) return false;
    char chunk[512];
    size_t rd;
    while ((rd = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        text.append(chunk, rd);
        if (text.size() > AUTO_RULE_MAX) { text.resize(AUTO_RULE_MAX); break; }
    }
    fclose(f);
    return true;
}

/* {dir, rules:[{id, enabled, on}]} -- a summary of every /sd/automation/<id>.json, sorted by id. */
static void auto_list(std::string &out)
{
    out = "{\"dir\":\"" AUTO_DIR "\",\"rules\":[";
    std::string host;
    std::vector<std::string> ids;
    if (web_fs_resolve(AUTO_DIR, host) == 0) {
        DIR *d = opendir(host.c_str());
        if (d) {
            while (struct dirent *ent = readdir(d)) {
                std::string nm = ent->d_name;
                if (nm.size() <= 5 || nm.compare(nm.size() - 5, 5, ".json") != 0) continue;
                std::string id = nm.substr(0, nm.size() - 5);
                if (!auto_id_ok(id)) continue;
                struct stat st;
                if (stat((host + "/" + nm).c_str(), &st) == 0 && S_ISREG(st.st_mode)) ids.push_back(id);
            }
            closedir(d);
        }
    }
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); i++) {
        std::string text;
        if (!auto_read_file(ids[i], text)) continue;
        std::string on;
        bool enabled = true;
        web_json_get_str(text, "on", on);
        web_json_get_bool(text, "enabled", &enabled);
        if (i) out += ',';
        out += "{\"id\":";
        web_json_str(out, ids[i].c_str());
        out += ",\"enabled\":";
        out += enabled ? "true" : "false";
        out += ",\"on\":";
        web_json_str(out, on.c_str());
        out += "}";
    }
    out += "]}";
}

/* ---------------------------------------------------------------- web endpoints */

static bool automation_handle(web_req_t *r)
{
    if (tools_is(r, "GET", "/api/automation/list")) {
        std::string j;
        auto_list(j);
        web_resp_json(r, 200, j);
        return true;
    }
    if (tools_is(r, "GET", "/api/automation/read")) {
        char idbuf[64] = {0};
        web_query_get(r, "id", idbuf, sizeof(idbuf));
        std::string id = idbuf;
        if (!auto_id_ok(id)) { web_resp_error(r, 400, "bad_name", "bad rule id"); return true; }
        std::string host;
        if (web_fs_resolve(std::string(AUTO_DIR "/" + id + ".json").c_str(), host)) {
            web_resp_error(r, 503, "sd_missing", "SD card not mounted");
            return true;
        }
        std::string text;
        if (!auto_read_file(id, text) || text.empty()) { web_resp_error(r, 404, "not_found", "no such rule"); return true; }
        std::string j = "{\"id\":";
        web_json_str(j, id.c_str());
        j += ",\"rule\":";
        j += text;                     /* the file is a JSON object (validated on save) -- embed raw */
        j += "}";
        web_resp_json(r, 200, j);
        return true;
    }
    if (tools_is(r, "POST", "/api/automation/save")) {
        std::string body;
        if (!tools_read_json(r, body, AUTO_RULE_MAX + 1024)) return true;
        if (body.size() > AUTO_RULE_MAX) { web_resp_error(r, 400, "too_large", "rule too large"); return true; }
        if (!web_json_is_object(body)) { web_resp_error(r, 400, "bad_request", "rule must be a JSON object"); return true; }
        std::string id;
        if (!web_json_get_str(body, "id", id) || !auto_id_ok(id)) {
            web_resp_error(r, 400, "bad_name", "rule needs a valid id");
            return true;
        }
        std::string dirhost;
        if (web_fs_resolve(AUTO_DIR, dirhost)) { web_resp_error(r, 503, "sd_missing", "SD card not mounted"); return true; }
        mkdir(dirhost.c_str(), 0777);
        std::string host;
        web_fs_resolve(std::string(AUTO_DIR "/" + id + ".json").c_str(), host);
        FILE *f = fopen(host.c_str(), "wb");
        if (!f) { web_resp_error(r, 500, "write_failed", "could not write rule"); return true; }
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
        std::string j = "{\"ok\":true,\"id\":";
        web_json_str(j, id.c_str());
        j += ",\"size\":" + tools_int((long)body.size()) + "}";
        web_resp_json(r, 200, j);
        return true;
    }
    if (tools_is(r, "POST", "/api/automation/delete")) {
        std::string body;
        if (!tools_read_json(r, body, 512)) return true;
        std::string id;
        web_json_get_str(body, "id", id);
        if (!auto_id_ok(id)) { web_resp_error(r, 400, "bad_name", "bad rule id"); return true; }
        std::string host;
        if (web_fs_resolve(std::string(AUTO_DIR "/" + id + ".json").c_str(), host)) {
            web_resp_error(r, 503, "sd_missing", "SD card not mounted");
            return true;
        }
        if (remove(host.c_str()) != 0) { web_resp_error(r, 404, "not_found", "no such rule"); return true; }
        web_resp_ok(r);
        return true;
    }
    return false;
}

static const char *const AUTO_CAPS[] = { "automation", NULL };
const web_tool_t web_tool_automation = { AUTO_CAPS, automation_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
