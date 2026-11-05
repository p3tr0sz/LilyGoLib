/**
 * @file      web_tool_scan.cpp
 * @brief     Wi-Fi and BLE scans (docs/web-api.md, "Scanners"). A scan runs on its own task:
 *            POST starts it, the result is published when it ends and kept for GET.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <algorithm>
#include <mutex>

static const char *const BLE_APPS[] = { "BLE Scan", "BLE HID", NULL };

/* One scanner's state; T is the hardware's result type. */
template <typename T>
struct web_scanner_t {
    std::mutex mx;
    bool scanning = false;
    long long ts = 0;
    int seconds = 5;
    std::vector<T> found;

    /* Mark a scan as started; false when one is already running (the caller joins it). */
    bool begin(int secs)
    {
        std::lock_guard<std::mutex> lk(mx);
        if (scanning) return false;
        scanning = true;
        seconds = secs;
        return true;
    }

    void finish(std::vector<T> &v)
    {
        std::lock_guard<std::mutex> lk(mx);
        found.swap(v);
        ts = tools_epoch();
        scanning = false;
    }

    std::string head(void)              /* call with mx held */
    {
        return std::string("{\"scanning\":") + (scanning ? "true" : "false") + ",\"ts\":" + (ts ? tools_int(ts) : "null");
    }
};

/* ---------------------------------------------------------------- Wi-Fi */

static web_scanner_t<web_hw_wifi_t> s_wifi;

static std::string wifi_json(void)
{
    std::lock_guard<std::mutex> lk(s_wifi.mx);
    std::string j = s_wifi.head() + ",\"networks\":[";
    for (size_t i = 0; i < s_wifi.found.size(); i++) {
        const web_hw_wifi_t &w = s_wifi.found[i];
        if (i) j += ",";
        j += "{\"ssid\":";
        web_json_str(j, w.ssid);
        j += ",\"bssid\":";
        web_json_str(j, w.bssid);
        j += ",\"rssi\":" + tools_int(w.rssi) + ",\"channel\":" + tools_int(w.channel) + ",\"auth\":";
        web_json_str(j, w.auth ? w.auth : "other");
        j += "}";
    }
    return j + "]}";
}

static void wifi_task(void *arg)
{
    (void)arg;
    std::vector<web_hw_wifi_t> v;
    web_hw_wifi_scan(v);
    std::sort(v.begin(), v.end(), [](const web_hw_wifi_t &a, const web_hw_wifi_t &b) { return a.rssi > b.rssi; });
    web_plat_log("[web] Wi-Fi scan: %u networks", (unsigned)v.size());
    s_wifi.finish(v);
    web_events_publish("wifi.scan", wifi_json().c_str());
}

static bool wifi_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/wifi/scan")) return false;
    if (!strcmp(r->method, "GET")) {
        web_resp_json(r, 200, wifi_json());
    } else if (!strcmp(r->method, "POST")) {
        if (s_wifi.begin(0)) web_plat_spawn(wifi_task, NULL);
        web_resp_json(r, 202, "{\"ok\":true}");
    } else {
        web_resp_error(r, 405, "bad_request", "GET or POST");
    }
    return true;
}

static const char *const WIFI_CAPS[] = { "wifi.scan", NULL };
const web_tool_t web_tool_wifi_scan = { WIFI_CAPS, wifi_handle, NULL, NULL, NULL };

/* ---------------------------------------------------------------- BLE */

static web_scanner_t<web_hw_ble_t> s_ble;

static std::string ble_json(void)
{
    std::lock_guard<std::mutex> lk(s_ble.mx);
    std::string j = s_ble.head() + ",\"devices\":[";
    for (size_t i = 0; i < s_ble.found.size(); i++) {
        const web_hw_ble_t &d = s_ble.found[i];
        if (i) j += ",";
        j += "{\"addr\":";
        web_json_str(j, d.addr.c_str());
        j += ",\"name\":";
        web_json_str(j, d.name.c_str());
        j += ",\"rssi\":" + tools_int(d.rssi) + ",\"connectable\":" + (d.connectable ? "true" : "false") + ",\"mfr\":";
        web_json_str(j, d.mfr.c_str());
        j += ",\"services\":[";
        for (size_t k = 0; k < d.services.size(); k++) {
            if (k) j += ",";
            web_json_str(j, d.services[k].c_str());
        }
        j += "]}";
    }
    return j + "]}";
}

static void ble_task(void *arg)
{
    (void)arg;
    int secs;
    {
        std::lock_guard<std::mutex> lk(s_ble.mx);
        secs = s_ble.seconds;
    }
    std::vector<web_hw_ble_t> v;
    web_hw_ble_scan(secs, v);
    std::sort(v.begin(), v.end(), [](const web_hw_ble_t &a, const web_hw_ble_t &b) { return a.rssi > b.rssi; });
    web_plat_log("[web] BLE scan: %u devices", (unsigned)v.size());
    s_ble.finish(v);
    web_events_publish("ble.scan", ble_json().c_str());
}

static void ble_post(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;
    double secs = 5;
    if (web_json_has(body, "seconds") && (!web_json_get_num(body, "seconds", &secs) || secs < 1 || secs > 15)) {
        web_resp_error(r, 400, "bad_request", "seconds: 1-15");
        return;
    }
    if (tools_busy(r, BLE_APPS)) return;
    if (s_ble.begin((int)secs)) web_plat_spawn(ble_task, NULL);
    web_resp_json(r, 202, "{\"ok\":true}");
}

static bool ble_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/ble/scan")) return false;
    if (!strcmp(r->method, "GET")) web_resp_json(r, 200, ble_json());
    else if (!strcmp(r->method, "POST")) ble_post(r);
    else web_resp_error(r, 405, "bad_request", "GET or POST");
    return true;
}

static const char *const BLE_CAPS[] = { "ble.scan", NULL };
const web_tool_t web_tool_ble_scan = { BLE_CAPS, ble_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
