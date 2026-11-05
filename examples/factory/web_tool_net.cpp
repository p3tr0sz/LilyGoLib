/**
 * @file      web_tool_net.cpp
 * @brief     Wi-Fi tools (docs/web-api.md, "Wi-Fi tools"): the watch's connection, DNS and TCP
 *            reach, and a 2.4 GHz channel overview. Read-only; it looks and reaches out, it does
 *            not transmit management frames. The Network Tools and Wi-Fi Analyzer apps, remoted.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <mutex>
#include <vector>

static const char *const APPS[] = { "Network Tools", "Wi-Fi Analyzer", NULL };

static std::mutex s_mx;
static std::vector<web_hw_wifi_t> s_aps;        /* the last channel scan */
static long long s_aps_ts = 0;
static bool s_scanning = false;

/* ---------------------------------------------------------------- the watch's own connection */

static void get_net(web_req_t *r)
{
    web_hw_net_t n;
    web_ui_run([&] { web_hw_net_info(&n); });
    std::string j = std::string("{\"connected\":") + (n.connected ? "true" : "false") + ",\"ssid\":";
    web_json_str(j, n.ssid);
    j += ",\"ip\":";
    web_json_str(j, n.ip);
    j += ",\"gateway\":";
    web_json_str(j, n.gateway);
    j += ",\"netmask\":";
    web_json_str(j, n.netmask);
    j += ",\"dns\":";
    web_json_str(j, n.dns);
    j += ",\"mac\":";
    web_json_str(j, n.mac);
    j += ",\"rssi\":" + tools_int(n.rssi) + ",\"channel\":" + tools_int(n.channel) + "}";
    web_resp_json(r, 200, j);
}

static void resolve_route(web_req_t *r)
{
    std::string body, host;
    if (!tools_read_json(r, body)) return;
    if (!web_json_get_str(body, "host", host) || host.empty() || host.size() > 128) {
        web_resp_error(r, 400, "bad_request", "host");
        return;
    }
    char ip[16] = "";
    int ms = 0, rc = -1;
    web_ui_run([&] { rc = web_hw_net_resolve(host.c_str(), ip, sizeof(ip), &ms); });
    if (rc < 0) {
        web_resp_error(r, 409, "offline", "the watch is not on Wi-Fi");
        return;
    }
    std::string j = std::string("{\"ok\":") + (rc ? "true" : "false") + ",\"ip\":";
    if (rc) web_json_str(j, ip); else j += "null";
    web_resp_json(r, 200, j + ",\"ms\":" + tools_int(ms) + "}");
}

static void tcp_route(web_req_t *r)
{
    std::string body, host;
    double port = 0;
    if (!tools_read_json(r, body)) return;
    if (!web_json_get_str(body, "host", host) || host.empty() ||
            !web_json_get_num(body, "port", &port) || port < 1 || port > 65535) {
        web_resp_error(r, 400, "bad_request", "host and port 1-65535");
        return;
    }
    int ms = 0, rc = -1;
    web_ui_run([&] { rc = web_hw_net_tcp(host.c_str(), (int)port, &ms); });
    if (rc < 0) {
        web_resp_error(r, 409, "offline", "the watch is not on Wi-Fi");
        return;
    }
    web_resp_json(r, 200, std::string("{\"ok\":") + (rc ? "true" : "false") + ",\"ms\":" + tools_int(ms) + "}");
}

/* ---------------------------------------------------------------- channel overview (from a scan) */

static int overlap_score(int channel)
{
    int score = 0;                      /* lower is clearer: nearby APs weighted by channel distance */
    for (const web_hw_wifi_t &ap : s_aps) {
        int delta = abs(ap.channel - channel);
        if (delta <= 4) score += (5 - delta) * (ap.rssi + 100 > 0 ? ap.rssi + 100 : 1);
    }
    return score;
}

static std::string channels_json(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    int best = 1, best_score = -1;
    std::string chans = "[";
    for (int ch = 1; ch <= 13; ch++) {
        int count = 0;
        for (const web_hw_wifi_t &ap : s_aps) if (ap.channel == ch) count++;
        int score = overlap_score(ch);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best = ch;
        }
        if (ch > 1) chans += ",";
        chans += "{\"channel\":" + tools_int(ch) + ",\"count\":" + tools_int(count) + ",\"score\":" + tools_int(score) + "}";
    }
    chans += "]";
    std::string aps = "[";
    for (size_t i = 0; i < s_aps.size(); i++) {
        const web_hw_wifi_t &ap = s_aps[i];
        if (i) aps += ",";
        aps += "{\"ssid\":";
        web_json_str(aps, ap.ssid);
        aps += ",\"bssid\":";
        web_json_str(aps, ap.bssid);
        aps += ",\"channel\":" + tools_int(ap.channel) + ",\"rssi\":" + tools_int(ap.rssi) + "}";
    }
    aps += "]";
    std::string j = std::string("{\"best\":") + tools_int(best) + ",\"scanning\":" + (s_scanning ? "true" : "false") +
                    ",\"channels\":" + chans + ",\"aps\":" + aps + "}";
    return j;
}

static void channels_task(void *arg)
{
    (void)arg;
    std::vector<web_hw_wifi_t> v;
    web_hw_wifi_scan(v);
    std::sort(v.begin(), v.end(), [](const web_hw_wifi_t &a, const web_hw_wifi_t &b) { return a.rssi > b.rssi; });
    std::lock_guard<std::mutex> lk(s_mx);
    s_aps = v;
    s_aps_ts = tools_epoch();
    s_scanning = false;
}

static void get_channels(web_req_t *r)
{
    bool start;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        start = !s_scanning && (s_aps.empty() || tools_epoch() - s_aps_ts > 20);
        if (start) s_scanning = true;
    }
    if (start) web_plat_spawn(channels_task, NULL);
    web_resp_json(r, 200, channels_json());
}

static bool net_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/net") && strncmp(r->path, "/api/net/", 9)) return false;
    if (tools_is(r, "GET", "/api/net")) get_net(r);
    else if (tools_is(r, "POST", "/api/net/resolve")) {
        if (!tools_busy(r, APPS)) resolve_route(r);
    } else if (tools_is(r, "POST", "/api/net/tcp")) {
        if (!tools_busy(r, APPS)) tcp_route(r);
    } else if (tools_is(r, "GET", "/api/net/channels")) {
        if (!tools_busy(r, APPS)) get_channels(r);
    } else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "wifi.tools", NULL };
const web_tool_t web_tool_net = { CAPS, net_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
