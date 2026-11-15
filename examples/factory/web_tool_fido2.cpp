/**
 * @file      web_tool_fido2.cpp
 * @brief     FIDO2 API contract and simulator backing.
 *
 * Three builds:
 *  - ARDUINO: the watch has no CTAP/FIDO HID transport wired yet, so this
 *    module advertises NO fido2 capability and every mutation fails clearly.
 *    It never pretends that keyboard HID is FIDO2.
 *  - FACTORY_FIDO_HOST_CRYPTO (desktop/simulator with OpenSSL): the routes are
 *    backed by the real CTAP2/CTAPHID core (fido2_core.cpp); credentials are
 *    real ES256 keys stored under the simulator's internal flash directory.
 *    This exercises the same protocol the watch will run.
 *  - otherwise: a tiny in-memory fake for desktop development.
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

static const char *const APPS[] = { "FIDO2", "FIDO2 Key", NULL };

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

#if defined(FACTORY_FIDO_HOST_CRYPTO)

#include "fido2_core.h"

static bool s_core_ok = false;
static bool s_core_tried = false;

/* Simulator has no watch screen; the transport toggle stands in for presence. */
static int core_user_present(void *ctx, const char *operation, const char *rp_id)
{
    (void)ctx; (void)operation; (void)rp_id;
    return s_available ? 1 : 0;
}

static void ensure_core(void)
{
    if (s_core_tried) return;
    s_core_tried = true;
    const char *root = web_plat_root_dir("flash");
    std::string dir = std::string(root ? root : "sim_flash") + "/fido2";
    s_core_ok = fido2_init(dir.c_str(), core_user_present, NULL);
    if (!s_core_ok) set_error("FIDO2 credential store could not be opened");
}

static void core_error(web_req_t *r, size_t status)
{
    if (status == FIDO2_ERR_NO_CREDENTIALS) {
        set_error("No credential for this relying party");
        web_resp_error(r, 404, "not_found", s_error);
    } else {
        char buf[80];
        snprintf(buf, sizeof(buf), "FIDO2 command failed (CTAP status 0x%02x)", (unsigned)status);
        set_error(buf);
        web_resp_error(r, 500, "fido_error", s_error);
    }
}

static bool core_begin_register(const std::string &rp, const std::string &user)
{
    uint8_t req[FIDO2_MAX_MESSAGE], resp[FIDO2_MAX_MESSAGE];
    size_t rl = fido2_make_management_request(0x01, rp.c_str(), user.c_str(), req, sizeof(req));
    if (!rl) return false;
    size_t n = fido2_handle(req, rl, resp, sizeof(resp));
    return n >= 1 && resp[0] == FIDO2_OK;
}

static bool core_begin_auth(const std::string &rp, size_t *status_out)
{
    uint8_t req[FIDO2_MAX_MESSAGE], resp[FIDO2_MAX_MESSAGE];
    size_t rl = fido2_make_management_request(0x02, rp.c_str(), "", req, sizeof(req));
    if (!rl) return false;
    size_t n = fido2_handle(req, rl, resp, sizeof(resp));
    if (n < 1) return false;
    if (resp[0] != FIDO2_OK) { *status_out = resp[0]; return false; }
    return true;
}

#endif /* FACTORY_FIDO_HOST_CRYPTO */

#ifndef ARDUINO
#if !defined(FACTORY_FIDO_HOST_CRYPTO)
struct sim_credential_t { char rp[65], user[65], key_handle[32]; };
static std::vector<sim_credential_t> s_creds;
#endif
#endif

static void get_status(web_req_t *r)
{
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    bool available = s_available && s_core_ok;
    int credentials = fido2_credential_count();
#else
    bool available = s_available;
    int credentials = s_credentials;
#endif
    std::string j = std::string("{\"available\":") + (available ? "true" : "false") + ",\"state\":";
    web_json_str(j, available ? s_state : "unavailable");
    j += ",\"credentials\":" + tools_int(credentials) + ",\"error\":";
    if (!available && s_error[0]) web_json_str(j, s_error); else j += "null";
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
    if (rp.empty() || rp.size() > 64 || user.empty() || user.size() > 64) {
        web_resp_error(r, 400, "bad_request", "rp and user are required (1-64 bytes each)"); return;
    }
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!s_core_ok) { web_resp_error(r, 503, "fido_unavailable", s_error[0] ? s_error : "FIDO2 core unavailable"); return; }
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
    s_state = "registering";
    if (!core_begin_register(rp, user)) {
        s_state = "idle";
        web_resp_error(r, 500, "fido_error", s_error[0] ? s_error : "FIDO2 registration failed"); return;
    }
    s_credentials = fido2_credential_count();
    s_state = "idle";
    set_error("");
    web_resp_ok(r);
#else
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
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
    if (rp.empty()) { web_resp_error(r, 400, "bad_request", "rp is required"); return; }
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!s_core_ok) { web_resp_error(r, 503, "fido_unavailable", s_error[0] ? s_error : "FIDO2 core unavailable"); return; }
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
    s_state = "authenticating";
    size_t status = 0;
    bool ok = core_begin_auth(rp, &status);
    s_state = "idle";
    if (!ok) { core_error(r, status); return; }
    set_error("");
    web_resp_ok(r);
#else
    if (!s_available) { web_resp_error(r, 503, "fido_unavailable", "Enable the simulated FIDO2 transport first"); return; }
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
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!fido2_credential_count() || !web_json_get_num(body, "index", &index) ||
        index < 0 || index != (int)index || index >= fido2_credential_count()) {
        web_resp_error(r, 400, "bad_request", "valid credential index required"); return;
    }
    if (!fido2_remove_credential_at((int)index)) { web_resp_error(r, 500, "fido_error", "could not remove credential"); return; }
    s_credentials = fido2_credential_count();
    set_error("");
    web_resp_ok(r);
#else
    if (!web_json_get_num(body, "index", &index) || index < 0 || index != (int)index || index >= (int)s_creds.size()) {
        web_resp_error(r, 400, "bad_request", "valid credential index required"); return;
    }
    s_creds.erase(s_creds.begin() + (int)index);
    s_credentials = (int)s_creds.size();
    set_error("");
    web_resp_ok(r);
#endif
#endif
}

static void get_credentials(web_req_t *r)
{
    std::string j = "{\"credentials\":[";
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    int count = fido2_credential_count();
    for (int i = 0; i < count; i++) {
        fido2_credential_info_t info;
        if (fido2_credential_at(i, &info) != 0) continue;
        if (i) j += ",";
        j += "{\"index\":" + tools_int(i) + ",\"rp\":";
        web_json_str(j, info.rp_id);
        j += ",\"user\":";
        web_json_str(j, info.user_name);
        j += "}";
    }
#elif !defined(ARDUINO)
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
#if defined(FACTORY_FIDO_HOST_CRYPTO)
        ensure_core();
        if (reset && s_core_ok) fido2_reset_authenticator();
        s_credentials = fido2_credential_count();
#else
        if (reset) {
            s_creds.clear();
            s_credentials = 0;
        }
#endif
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
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!s_core_ok || !s_available || !rp || !user) return false;
    bool ok = core_begin_register(rp, user);
    s_credentials = fido2_credential_count();
    return ok;
#else
    (void)rp; (void)user;
    return false;
#endif
}
bool web_hw_fido2_begin_authenticate(const char *rp)
{
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!s_core_ok || !s_available || !rp) return false;
    size_t status = 0;
    return core_begin_auth(rp, &status);
#else
    (void)rp;
    return false;
#endif
}
int web_hw_fido2_credentials(void)
{
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    return fido2_credential_count();
#else
    return s_credentials;
#endif
}
bool web_hw_fido2_remove_credential(int index)
{
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    if (!fido2_remove_credential_at(index)) return false;
    s_credentials = fido2_credential_count();
    return true;
#else
    (void)index; return false;
#endif
}
void web_hw_fido2_status(web_hw_fido2_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
#if defined(FACTORY_FIDO_HOST_CRYPTO)
    ensure_core();
    out->available = s_available && s_core_ok;
#else
    out->available = s_available;
#endif
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
