/**
 * @file      web_tool_wardrive.cpp
 * @brief     Wardriving (docs/web-api.md, "Wardrive"). A session logs the Wi-Fi APs seen across
 *            repeated scans, tags each with the watch's current GNSS fix, dedupes by BSSID (keeping
 *            the location of the strongest signal, WiGLE convention) and exports the catalogue as a
 *            WiGLE-1.6 CSV on the SD card for upload. Passive: it only records what the Wi-Fi scan
 *            and the GNSS receiver already see; it never transmits. The browser drives the scan
 *            cadence (the watch decides, the browser shows); the watch keeps the catalogue.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "web_internal.h"
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#define WD_MAX          512     /* most APs kept in one session (RAM bound) */
#define WD_LIST_MAX     400     /* most networks returned in one GET (response bound) */
#define WD_ACCURACY_M   "10.0"  /* nominal horizontal accuracy written to the CSV (v1: no HDOP) */

/* One observed access point, deduped by BSSID. The stored location is the one where the AP was
 * heard strongest and a fix was available (WiGLE records the peak-signal position). */
struct wd_obs {
    std::string ssid, bssid, auth;
    int channel = 0;
    int best_rssi = -127;
    long long first_seen = 0, last_seen = 0;
    int count = 0;
    bool has_fix = false;
    double lat = 0, lon = 0, alt = 0;
    bool has_alt = false;
};

static std::mutex s_mx;
static bool s_running = false;
static bool s_scanning = false;
static long long s_started = 0;
static long long s_ts = 0;              /* epoch of the last completed scan */
static std::vector<wd_obs> s_obs;

/* Map our auth strings (web_tools_hw.cpp auth_name) to WiGLE AuthMode bracket notation. */
static const char *wigle_auth(const std::string &a)
{
    if (a == "open")        return "[ESS]";
    if (a == "wep")         return "[WEP][ESS]";
    if (a == "wpa")         return "[WPA-PSK-TKIP][ESS]";
    if (a == "wpa2")        return "[WPA2-PSK-CCMP][ESS]";
    if (a == "wpa/wpa2")    return "[WPA-PSK-TKIP][WPA2-PSK-CCMP][ESS]";
    if (a == "wpa2-ent")    return "[WPA2-EAP-CCMP][ESS]";
    if (a == "wpa3")        return "[WPA3-SAE-CCMP][ESS]";
    if (a == "wpa2/wpa3")   return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    return "[ESS]";
}

/* Escape one field for CSV: wrap in quotes and double internal quotes when it holds a comma,
 * quote, or newline (WiGLE/RFC 4180). Returns the field unchanged otherwise. */
static std::string csv_field(const std::string &s)
{
    bool need = s.find_first_of(",\"\r\n") != std::string::npos;
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static int count_with_fix_locked(void)
{
    int n = 0;
    for (const wd_obs &o : s_obs) if (o.has_fix) n++;
    return n;
}

static std::string status_json_locked(bool with_list)
{
    std::string j = "{\"running\":";
    j += s_running ? "true" : "false";
    j += ",\"scanning\":";
    j += s_scanning ? "true" : "false";
    j += ",\"started\":";
    j += s_started ? tools_int(s_started) : "null";
    j += ",\"ts\":";
    j += s_ts ? tools_int(s_ts) : "null";
    j += ",\"count\":" + tools_int((long long)s_obs.size());
    j += ",\"with_fix\":" + tools_int(count_with_fix_locked());
    if (with_list) {
        j += ",\"networks\":[";
        size_t n = std::min(s_obs.size(), (size_t)WD_LIST_MAX);
        for (size_t i = 0; i < n; i++) {
            const wd_obs &o = s_obs[i];
            if (i) j += ",";
            j += "{\"ssid\":";
            web_json_str(j, o.ssid.c_str());
            j += ",\"bssid\":";
            web_json_str(j, o.bssid.c_str());
            j += ",\"auth\":";
            web_json_str(j, o.auth.c_str());
            j += ",\"channel\":" + tools_int(o.channel);
            j += ",\"rssi\":" + tools_int(o.best_rssi);
            j += ",\"count\":" + tools_int(o.count);
            j += ",\"first_seen\":" + tools_int(o.first_seen);
            j += ",\"last_seen\":" + tools_int(o.last_seen);
            j += ",\"has_fix\":";
            j += o.has_fix ? "true" : "false";
            if (o.has_fix) {
                j += ",\"lat\":" + tools_num(o.lat, 6) + ",\"lon\":" + tools_num(o.lon, 6);
                if (o.has_alt) j += ",\"alt\":" + tools_num(o.alt, 1);
            }
            j += "}";
        }
        j += "]";
        if (s_obs.size() > n) j += ",\"truncated\":true";
    }
    return j + "}";
}

/* The scan task: one Wi-Fi scan, read the current GNSS fix, then merge both into the catalogue. */
static void wardrive_task(void *arg)
{
    (void)arg;

    std::vector<web_hw_wifi_t> nets;
    web_hw_wifi_scan(nets);

    web_hw_gnss_t g;
    memset(&g, 0, sizeof(g));
    bool fix = web_hw_gnss_read(&g) && g.fix;

    long long now = tools_epoch();
    int added = 0;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_running) {
            for (const web_hw_wifi_t &w : nets) {
                wd_obs *o = NULL;
                for (wd_obs &e : s_obs) {
                    if (e.bssid == w.bssid) { o = &e; break; }
                }
                if (!o) {
                    if (s_obs.size() >= WD_MAX) continue;    /* catalogue full: keep what we have */
                    s_obs.push_back(wd_obs());
                    o = &s_obs.back();
                    o->bssid = w.bssid;
                    o->first_seen = now;
                    o->best_rssi = -127;
                    added++;
                }
                o->ssid = w.ssid;
                o->auth = w.auth ? w.auth : "other";
                o->channel = w.channel;
                o->last_seen = now;
                o->count++;
                /* Record the position of the strongest heard signal (only when a fix is in hand).
                 * Adopt the first fix we ever get for this AP even if the signal is not the peak. */
                bool stronger = w.rssi > o->best_rssi;
                if (fix && (stronger || !o->has_fix)) {
                    o->lat = g.lat; o->lon = g.lon;
                    o->has_alt = g.has_alt; o->alt = g.alt;
                    o->has_fix = true;
                }
                if (stronger) o->best_rssi = w.rssi;
            }
            s_ts = now;
        }
        s_scanning = false;
    }

    web_plat_log("[web] wardrive scan: %u seen, %d new, fix=%d", (unsigned)nets.size(), added, (int)fix);

    std::string ev;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        ev = status_json_locked(false);
    }
    web_events_publish("wardrive", ev.c_str());
}

/* Write the catalogue (networks with a fix) as a WiGLE-1.6 CSV at host. Returns rows written, or
 * -1 on an IO error; *skipped gets the count of fix-less networks left out. */
static int write_wigle(const std::string &host, int *skipped)
{
    FILE *f = fopen(host.c_str(), "wb");
    if (!f) return -1;

    const char *fw = web_plat_fw_version();
    fprintf(f, "WigleWifi-1.6,appRelease=%s,model=T-Watch-Ultra,release=%s,"
               "device=twatch-ultra,display=,board=esp32s3,brand=LilyGo\n",
            fw, fw);
    fprintf(f, "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
               "AltitudeMeters,AccuracyMeters,Type\n");

    int rows = 0, skip = 0;
    std::lock_guard<std::mutex> lk(s_mx);
    for (const wd_obs &o : s_obs) {
        if (!o.has_fix) { skip++; continue; }
        char when[24] = "";
        time_t t = (time_t)o.first_seen;
        struct tm *g = gmtime(&t);
        if (g) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", g);
        std::string line = csv_field(o.bssid) + "," + csv_field(o.ssid) + "," +
                           csv_field(wigle_auth(o.auth)) + "," + when + "," +
                           tools_int(o.channel) + "," + tools_int(o.best_rssi) + "," +
                           tools_num(o.lat, 6) + "," + tools_num(o.lon, 6) + "," +
                           (o.has_alt ? tools_num(o.alt, 1) : std::string("")) + "," +
                           WD_ACCURACY_M + ",WIFI\n";
        fwrite(line.data(), 1, line.size(), f);
        rows++;
    }
    fclose(f);
    if (skipped) *skipped = skip;
    return rows;
}

/* ---------------------------------------------------------------- routes */

static void get_status(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    web_resp_json(r, 200, status_json_locked(true));
}

static void post_start(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_running = true;
    s_started = tools_epoch();
    s_obs.clear();
    s_ts = 0;
    web_resp_json(r, 200, status_json_locked(false));
}

static void post_stop(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_running = false;
    web_resp_json(r, 200, status_json_locked(false));
}

static void post_clear(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_scanning) { web_resp_error(r, 409, "busy", "a scan is in progress"); return; }
    s_obs.clear();
    s_ts = 0;
    web_resp_json(r, 200, status_json_locked(false));
}

static void post_scan(web_req_t *r)
{
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_running) { web_resp_error(r, 409, "not_running", "start a session first"); return; }
        if (!s_scanning) { s_scanning = true; spawn = true; }
    }
    if (spawn) web_plat_spawn(wardrive_task, NULL);
    web_resp_json(r, 202, "{\"ok\":true}");
}

static void post_export(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;

    std::string api_path;
    web_json_get_str(body, "path", api_path);

    char def[80];
    if (api_path.empty()) {
        long long base = 0;
        { std::lock_guard<std::mutex> lk(s_mx); base = s_started ? s_started : tools_epoch(); }
        time_t t = (time_t)base;
        struct tm *g = gmtime(&t);
        if (g) snprintf(def, sizeof(def), "/sd/wardrive/wardrive-%04d%02d%02d-%02d%02d%02d.csv",
                        g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
        else   snprintf(def, sizeof(def), "/sd/wardrive/wardrive.csv");
        api_path = def;
    }

    /* Only ever write under /sd or /flash, and make the folder first. */
    std::string host;
    if (web_fs_resolve(api_path.c_str(), host)) {
        web_resp_error(r, 400, "bad_request", "path must be under /sd or /flash");
        return;
    }
    std::string slash = host.substr(0, host.find_last_of('/'));
    if (!slash.empty()) mkdir(slash.c_str(), 0777);

    int skipped = 0;
    int rows = write_wigle(host, &skipped);
    if (rows < 0) { web_resp_error(r, 503, "sd_missing", "could not write the CSV"); return; }

    std::string j = "{\"ok\":true,\"path\":";
    web_json_str(j, api_path.c_str());
    j += ",\"rows\":" + tools_int(rows) + ",\"skipped\":" + tools_int(skipped) + "}";
    web_resp_json(r, 200, j);
}

/* ---------------------------------------------------------------- on-device surface (ui_wardrive.cpp)
 * The native app shows the session and drives it; the catalogue and the WiGLE format live here. */

void wardrive_counts(bool *running, bool *scanning, int *count, int *with_fix, long long *last_ts)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (running)  *running  = s_running;
    if (scanning) *scanning = s_scanning;
    if (count)    *count    = (int)s_obs.size();
    if (with_fix) *with_fix = count_with_fix_locked();
    if (last_ts)  *last_ts  = s_ts;
}

void wardrive_start(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_running = true;
    s_started = tools_epoch();
    s_obs.clear();
    s_ts = 0;
}

void wardrive_stop(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_running = false;
}

/* Start one scan on its own task. false when a scan is already running or no session is active. */
bool wardrive_scan_async(void)
{
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_running || s_scanning) return false;
        s_scanning = true;
    }
    web_plat_spawn(wardrive_task, NULL);
    return true;
}

/* Export to the default /sd/wardrive path. Returns rows written (-1 on IO error); fills path_out. */
int wardrive_export(std::string &path_out, int *skipped)
{
    long long base;
    { std::lock_guard<std::mutex> lk(s_mx); base = s_started ? s_started : tools_epoch(); }
    char def[80];
    time_t t = (time_t)base;
    struct tm *g = gmtime(&t);
    if (g) snprintf(def, sizeof(def), "/sd/wardrive/wardrive-%04d%02d%02d-%02d%02d%02d.csv",
                    g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
    else   snprintf(def, sizeof(def), "/sd/wardrive/wardrive.csv");
    path_out = def;

    std::string host;
    if (web_fs_resolve(path_out.c_str(), host)) return -1;
    std::string slash = host.substr(0, host.find_last_of('/'));
    if (!slash.empty()) mkdir(slash.c_str(), 0777);
    return write_wigle(host, skipped);
}

static bool wardrive_handle(web_req_t *r)
{
    if (tools_is(r, "GET",  "/api/wardrive"))        { get_status(r); return true; }
    if (tools_is(r, "POST", "/api/wardrive/start"))  { post_start(r); return true; }
    if (tools_is(r, "POST", "/api/wardrive/stop"))   { post_stop(r);  return true; }
    if (tools_is(r, "POST", "/api/wardrive/scan"))   { post_scan(r);  return true; }
    if (tools_is(r, "POST", "/api/wardrive/clear"))  { post_clear(r); return true; }
    if (tools_is(r, "POST", "/api/wardrive/export")) { post_export(r); return true; }
    return false;
}

/* The server is shutting down: end the session so a stale 'running' flag does not persist. */
static void wardrive_tool_stop(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_running = false;
}

static const char *const WARDRIVE_CAPS[] = { "wardrive", NULL };
const web_tool_t web_tool_wardrive = { WARDRIVE_CAPS, wardrive_handle, NULL, NULL, wardrive_tool_stop };

#endif /* FACTORY_WEB */
