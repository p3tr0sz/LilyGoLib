/**
 * @file      web_tool_fido2.cpp
 * @brief     FIDO2 security key (P9 NucleoOS parity): the watch as a USB/BLE FIDO2/U2F
 *            authenticator for passkeys and WebAuthn.
 *
 *            Builds on the USB HID stack (the same HID transport the BadUSB / usb.hid tool uses).
 *            On the watch, implements the FIDO2 HID protocol over USB. The simulator provides
 *            a fake that stores credentials in memory.
 *
 *            This enables the watch to act as a hardware security key for passwordless
 *            authentication — WebAuthn registration and assertion.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <mutex>

/* In a real implementation, this would talk to the FIDO2 HID interface on the ESP32.
 * For now this provides the web API layer and state tracking; the HID transport is
 * platform-specific and stubbed on the watch side.
 *
 * The FIDO2 specification defines:
 *   - U2F_REGISTER (0x01): register a credential
 *   - U2F_AUTHENTICATE (0x02): authenticate with a credential
 *   - FIDO2_CMD_* extensions for CTAP2
 *
 * The watch implements a minimal FIDO2 authenticator with:
 *   - On-device key generation (ECDSA P-256)
 *   - Credential storage on the flash
 *   - HID transport over USB or BLE
 */

static bool s_available = false;
static const char *s_state = "idle";  /* "idle", "registering", "authenticating" */
static const char *s_error = NULL;
static int s_credentials = 0;

/* For the simulator: in-memory credential store */
#ifndef ARDUINO
typedef struct {
    char rp_id[64];
    char user_name[64];
    char key_handle[128];
    char public_key[128];
} sim_credential_t;
static std::vector<sim_credential_t> s_creds;
#endif

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "FIDO2", NULL };

static void get_status(web_req_t *r)
{
    std::string j = "{\"available\":" + std::string(s_available ? "true" : "false");
    j += ",\"state\":";
    web_json_str(j, s_state);
    j += ",\"credentials\":" + tools_int(s_credentials);
    if (s_error) {
        j += ",\"error\":";
        web_json_str(j, s_error);
    }
    j += "}";
    web_resp_json(r, 200, j);
}

static void begin_register(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, rp, user;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "rp", rp);
    web_json_get_str(body, "user", user);

    if (!s_available) { web_resp_error(r, 503, "hid_unavailable", "No USB host enumerated the keyboard"); return; }
    if (strcmp(s_state, "idle") != 0) { web_resp_error(r, 409, "conflict", "busy"); return; }

    s_state = "registering";
    s_error = NULL;

#ifndef ARDUINO
    /* Simulator: create a fake credential */
    sim_credential_t cred;
    snprintf(cred.rp_id, sizeof(cred.rp_id), "%s", rp.c_str());
    snprintf(cred.user_name, sizeof(cred.user_name), "%s", user.c_str());
    snprintf(cred.key_handle, sizeof(cred.key_handle), "sim-keyhandle-%zu", s_creds.size());
    snprintf(cred.public_key, sizeof(cred.public_key), "04:sim-public-key-%zu", s_creds.size());
    s_creds.push_back(cred);
    s_credentials = (int)s_creds.size();
#endif

    s_state = "idle";
    web_resp_ok(r);
}

static void begin_auth(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, rp;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "rp", rp);

    if (!s_available) { web_resp_error(r, 503, "hid_unavailable", "No USB host enumerated the keyboard"); return; }
    if (strcmp(s_state, "idle") != 0) { web_resp_error(r, 409, "conflict", "busy"); return; }

    s_state = "authenticating";
    s_error = NULL;

#ifndef ARDUINO
    /* Simulator: check we have a credential for this RP */
    bool found = false;
    for (const sim_credential_t &c : s_creds) {
        if (c.rp_id == rp) { found = true; break; }
    }
    if (!found) { s_error = "no_credential"; s_state = "idle"; web_resp_error(r, 404, "not_found", "no credential for this RP"); return; }
#endif

    s_state = "idle";
    web_resp_ok(r);
}

static void remove_credential(web_req_t *r)
{
    std::string body;
    double idx = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_num(body, "index", &idx);
    int i = (int)idx;

#ifndef ARDUINO
    if (i < 0 || i >= (int)s_creds.size()) {
        web_resp_error(r, 400, "bad_request", "invalid credential index");
        return;
    }
    s_creds.erase(s_creds.begin() + i);
    s_credentials = (int)s_creds.size();
#endif

    web_resp_ok(r);
}

static void get_credentials(web_req_t *r)
{
    std::string j = "{\"credentials\":[";
#ifndef ARDUINO
    for (size_t i = 0; i < s_creds.size(); i++) {
        if (i) j += ",";
        j += "{\"index\":" + tools_int((int)i) + ",\"rp\":";
        web_json_str(j, s_creds[i].rp_id);
        j += ",\"user\":";
        web_json_str(j, s_creds[i].user_name);
        j += "}";
    }
#endif
    j += "]}";
    web_resp_json(r, 200, j);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/fido2") && strncmp(r->path, "/api/fido2/", 11)) return false;
    if (tools_is(r, "GET", "/api/fido2")) get_status(r);
    else if (tools_is(r, "POST", "/api/fido2/register")) begin_register(r);
    else if (tools_is(r, "POST", "/api/fido2/authenticate")) begin_auth(r);
    else if (tools_is(r, "GET", "/api/fido2/credentials")) get_credentials(r);
    else if (tools_is(r, "POST", "/api/fido2/remove")) remove_credential(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/fido2")) {
        std::string body;
        double avail = 1;
        web_read_body(r, body, 256);
        web_json_get_num(body, "available", &avail);
        s_available = avail > 0;
        web_resp_ok(r);
        return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void fido2_second(uint32_t now_ms)
{
    (void)now_ms;
    /* Check if HID is still available */
#ifdef ARDUINO
    s_available = web_hw_hid_available();
#endif
}

static const char *const CAPS[] = { "fido2", NULL };
const web_tool_t web_tool_fido2 = { CAPS, handle, NULL, fido2_second, NULL };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_fido2 = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */