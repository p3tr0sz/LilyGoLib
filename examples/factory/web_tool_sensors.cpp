/**
 * @file      web_tool_sensors.cpp
 * @brief     IMU and GNSS (docs/web-api.md, "Sensors", "GNSS"). Both are polled by the desktop:
 *            the first GET turns them on and they stop 10 s after the last one.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <mutex>

#define POLL_KEEP_MS    10000

static bool expired(uint32_t asked, uint32_t now)
{
    return now - asked > POLL_KEEP_MS;
}

/* ---------------------------------------------------------------- IMU (state on the UI thread) */

static bool s_imu_on = false;
static uint32_t s_imu_asked = 0;

static void get_sensors(web_req_t *r)
{
    web_hw_imu_t v;
    web_ui_run([&] {
        if (!s_imu_on) {
            web_hw_imu_power(true);
            s_imu_on = true;
        }
        s_imu_asked = tools_now_ms();
        web_hw_imu_read(&v);
    });
    std::string j = "{\"imu\":{\"roll\":" + tools_num(v.roll, 1) + ",\"pitch\":" + tools_num(v.pitch, 1) +
                    ",\"heading\":" + tools_num(v.heading, 1) + ",\"accel\":";
    j += v.accel_valid ? "{\"x\":" + tools_num(v.ax, 2) + ",\"y\":" + tools_num(v.ay, 2) + ",\"z\":" + tools_num(v.az, 2) + "}"
         : std::string("null");
    j += "},\"steps\":" + (v.has_steps ? tools_int(v.steps) : std::string("null")) + ",\"seq\":" + tools_int(v.seq) + "}";
    web_resp_json(r, 200, j);
}

static void imu_off(void)
{
    if (!s_imu_on) return;
    web_hw_imu_power(false);
    s_imu_on = false;
}

static void imu_second(uint32_t now)
{
    if (s_imu_on && expired(s_imu_asked, now)) imu_off();
}

static bool sensors_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/sensors")) return false;
    if (!strcmp(r->method, "GET")) get_sensors(r);
    else web_resp_error(r, 405, "bad_request", "GET");
    return true;
}

static const char *const SENSORS_CAPS[] = { "sensors.imu", NULL };
const web_tool_t web_tool_sensors = { SENSORS_CAPS, sensors_handle, NULL, imu_second, imu_off };

/* ---------------------------------------------------------------- GNSS */

static std::mutex s_gnss_mx;
static web_hw_gnss_t s_gnss;            /* the last reading, under s_gnss_mx */
static bool s_gnss_have = false;
static uint32_t s_gnss_asked = 0;       /* UI thread; 0 = nobody watches */

static std::string fix_json(const web_hw_gnss_t &g)
{
    std::string j = std::string("{\"fix\":") + (g.fix ? "true" : "false");
    j += ",\"lat\":" + (g.fix ? tools_num(g.lat, 7) : "null") + ",\"lon\":" + (g.fix ? tools_num(g.lon, 7) : "null");
    j += ",\"alt\":" + (g.fix && g.has_alt ? tools_num(g.alt, 1) : "null");
    j += ",\"speed_kmh\":" + (g.fix && g.has_speed ? tools_num(g.speed_kmh, 1) : "null");
    j += ",\"sats\":" + tools_int(g.sats) + ",\"time\":" + (g.time ? tools_int(g.time) : "null") + "}";
    return j;
}

static std::string gnss_json(const web_hw_gnss_t &g)
{
    std::string fix = fix_json(g);
    std::string j = "{\"model\":";
    web_json_str(j, g.model);
    j += "," + fix.substr(1, fix.size() - 2);       /* the fix fields, inline */
    j += ",\"ttff_ms\":" + (g.has_ttff ? tools_int(g.ttff_ms) : std::string("null")) + ",\"antenna\":";
    web_json_str(j, g.antenna ? g.antenna : "unknown");
    j += ",\"satellites\":[";
    for (int i = 0; i < g.nsat; i++) {
        const web_hw_sat_t &s = g.sat[i];
        if (i) j += ",";
        j += "{\"sys\":";
        web_json_str(j, s.sys ? s.sys : "GPS");
        j += ",\"prn\":" + tools_int(s.prn) + ",\"el\":" + tools_int(s.el) + ",\"az\":" + tools_int(s.az) +
             ",\"cn0\":" + (s.has_cn0 ? tools_int(s.cn0) : "null") + ",\"used\":" + (s.used ? "true" : "false") + "}";
    }
    return j + "]}";
}

/* UI thread. true when there is a reading (new or kept). */
static bool gnss_read(void)
{
    static web_hw_gnss_t g;             /* big: not on the stack */
    memset(&g, 0, sizeof(g));
    std::lock_guard<std::mutex> lk(s_gnss_mx);
    if (web_hw_gnss_read(&g)) {
        s_gnss = g;
        s_gnss_have = true;
    }
    return s_gnss_have;
}

static void get_gnss(web_req_t *r)
{
    web_ui_run([] {
        s_gnss_asked = tools_now_ms();
        if (!s_gnss_have) gnss_read();
    });
    std::string j;
    {
        std::lock_guard<std::mutex> lk(s_gnss_mx);
        if (s_gnss_have) {
            j = gnss_json(s_gnss);
        } else {
            static web_hw_gnss_t none;
            memset(&none, 0, sizeof(none));
            j = gnss_json(none);
        }
    }
    web_resp_json(r, 200, j);
}

static void gnss_second(uint32_t now)
{
    if (!s_gnss_asked) return;
    if (expired(s_gnss_asked, now)) {
        s_gnss_asked = 0;
        return;
    }
    if (!gnss_read()) return;
    std::string ev;
    {
        std::lock_guard<std::mutex> lk(s_gnss_mx);
        ev = fix_json(s_gnss);
    }
    web_events_publish("gnss.fix", ev.c_str());
}

static void gnss_stop(void)
{
    s_gnss_asked = 0;
}

static bool gnss_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/gnss")) return false;
    if (!strcmp(r->method, "GET")) get_gnss(r);
    else web_resp_error(r, 405, "bad_request", "GET");
    return true;
}

static const char *const GNSS_CAPS[] = { "gnss", NULL };
const web_tool_t web_tool_gnss = { GNSS_CAPS, gnss_handle, NULL, gnss_second, gnss_stop };

#endif /* FACTORY_WEB */
