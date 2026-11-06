/**
 * @file      web_tool_fido2.cpp
 * @brief     FIDO2 API contract and simulator fake.
 *
 * The watch does not currently have a CTAP/FIDO HID transport or credential
 * crypto. On hardware this module therefore advertises NO fido2 capability and
 * every mutation fails clearly; it never pretends that keyboard HID is FIDO2.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>

static bool s_available = false;
static const char *s_state = "unavailable";
static char s_error[128] = "FIDO2 authentication is not implemented in this firmware";
static int s_credentials = 0;

#ifndef ARDUINO
struct sim_credential_t { char rp[65], user[65], key_handle[32]; };
static std::vector<sim_credential_t> s_creds;
#endif

static const char *const APPS[] = { "FIDO2", "FIDO2 Key", NULL };

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static void get_status(web_req_t *r)
{
    std::string j = std::string("{\"available\":") + (s_available ? "true" : "false") + ",\"state\":";
    web_json_str(j, s_state);
    j += ",\"credentials\":" + tools_int(s_credentials) + ",\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += "}";
    web_resp_json(r, 200, j);
}

#ifdef ARDUINO
static bool unavailable(web_req_t *r)
{
    web_resp_error(r, 503, "fido_unavailable", "FIDO2 authentication is not implemented in this firmware");
    return false;
}
#endif

static void begin_register(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
#ifdef ARDUINO
    unavailable(r); return;
#else
    std::string body, rp, user;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "rp", rp);
    web_json_get_str(body, "user", user);
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
    if (rp.empty() || rp.size() > 64 || user.empty() || user.size() > 64) {
        web_resp_error(r, 400, "bad_request", "rp and user are required (1-64 bytes each)"); return;
    }
    if (s_creds.size() >= 32) { web_resp_error(r, 409, "conflict", "simulator credential store is full"); return; }
    s_state = "registering";
    sim_credential_t cred = {};
    snprintf(cred.rp, sizeof(cred.rp), "%s", rp.c_str());
    snprintf(cred.user, sizeof(cred.user), "%s", user.c_str());
    snprintf(cred.key_handle, sizeof(cred.key_handle), "sim-%zu", s_creds.size());
    s_creds.push_back(cred);
    s_credentials = (int)s_creds.size();
    s_state = "idle";
    set_error("");
    web_resp_ok(r);
#endif
}

static void begin_auth(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
#ifdef ARDUINO
    unavailable(r); return;
#else
    std::string body, rp;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "rp", rp);
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
    if (rp.empty()) { web_resp_error(r, 400, "bad_request", "rp is required"); return; }
    bool found = false;
    for (const auto &c : s_creds) if (rp == c.rp) { found = true; break; }
    if (!found) {
        s_state = "idle";
        set_error("No credential for this relying party");
        web_resp_error(r, 404, "not_found", s_error);
        return;
    }
    s_state = "idle";
    set_error("");
    web_resp_ok(r);
#endif
}

static void remove_credential(web_req_t *r)
{
#ifdef ARDUINO
    unavailable(r); return;
#else
    std::string body;
    double index = -1;
    if (!tools_read_json(r, body)) return;
    if (!web_json_get_num(body, "index", &index) || index < 0 || index != (int)index || index >= (int)s_creds.size()) {
        web_resp_error(r, 400, "bad_request", "valid credential index required"); return;
    }
    s_creds.erase(s_creds.begin() + (int)index);
    s_credentials = (int)s_creds.size();
    set_error("");
    web_resp_ok(r);
#endif
}

static void get_credentials(web_req_t *r)
{
    std::string j = "{\"credentials\":[";
#ifndef ARDUINO
    for (size_t i = 0; i < s_creds.size(); i++) {
        if (i) j += ",";
        j += "{\"index\":" + tools_int((long long)i) + ",\"rp\":";
        web_json_str(j, s_creds[i].rp);
        j += ",\"user\":";
        web_json_str(j, s_creds[i].user);
        j += "}";
    }
#endif
    j += "]}";
    web_resp_json(r, 200, j);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/fido2")) {
        std::string body;
        bool available = true, reset = false;
        if (!tools_read_json(r, body)) return true;
        web_json_get_bool(body, "available", &available);
        web_json_get_bool(body, "reset", &reset);
        if (reset) {
            s_creds.clear();
            s_credentials = 0;
        }
        s_available = available;
        s_state = available ? "idle" : "unavailable";
        set_error(available ? "" : "Simulated FIDO2 transport is disabled");
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/fido2") && strncmp(r->path, "/api/fido2/", 11)) return false;
    if (tools_is(r, "GET", "/api/fido2")) get_status(r);
    else if (tools_is(r, "POST", "/api/fido2/register")) begin_register(r);
    else if (tools_is(r, "POST", "/api/fido2/authenticate")) begin_auth(r);
    else if (tools_is(r, "GET", "/api/fido2/credentials")) get_credentials(r);
    else if (tools_is(r, "POST", "/api/fido2/remove")) remove_credential(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

bool web_hw_fido2_begin_register(const char *rp, const char *user)
{
    (void)rp; (void)user;
    return false;
}
bool web_hw_fido2_begin_authenticate(const char *rp)
{
    (void)rp;
    return false;
}
int web_hw_fido2_credentials(void) { return s_credentials; }
bool web_hw_fido2_remove_credential(int index) { (void)index; return false; }
void web_hw_fido2_status(web_hw_fido2_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->available = s_available;
    out->state = s_state;
    out->credentials = s_credentials;
    out->error = s_error[0] ? s_error : NULL;
}

#ifdef ARDUINO
static const char *const CAPS[] = { NULL };
#else
static const char *const CAPS[] = { "fido2", NULL };
#endif
const web_tool_t web_tool_fido2 = { CAPS, handle, NULL, NULL, NULL };

#else
const web_tool_t web_tool_fido2 = { NULL, NULL, NULL, NULL, NULL };
#endif
