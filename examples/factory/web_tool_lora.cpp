/**
 * @file      web_tool_lora.cpp
 * @brief     LoRa (docs/web-api.md, "LoRa"): radio settings, send, receive, packet log.
 *
 *            The radio is the desktop's only after its first LoRa request; the watch's LoRa,
 *            Walkie, Sub-G and LoRaWAN apps take it back (503 busy while they are open, and the
 *            desktop lets go when one opens). Radio state lives on the UI thread; the packet
 *            log is shared with request handlers under s_log_mx.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include <deque>
#include <mutex>

#define LOG_KEEP        50
#define MAX_PAYLOAD     255
#define TX_QUEUE        8       /* packets waiting while one is on air (SF12 takes seconds) */

static const char *const APPS[] = { "LoRa", "Walkie", "Sub-G", "LoRaWAN", NULL };
static const float BANDWIDTHS[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125, 250, 500 };

/* UI thread */
static web_hw_lora_cfg_t s_cfg;
static bool s_loaded = false;
static bool s_owned = false;            /* the radio is set up for the desktop */
static bool s_tx_pending = false;     /* a packet is on air */
static std::deque<std::string> s_tx_queue;

struct lora_packet_t { uint32_t n; bool tx; long long ts; int rssi, snr; std::string data; };
static std::mutex s_log_mx;
static std::deque<lora_packet_t> s_log;
static uint32_t s_log_n = 0;

/* ---------------------------------------------------------------- settings, kept in <flash>/.web/lora.txt */

static std::string cfg_path(void)
{
    const char *dir = web_plat_root_dir("flash");
    return std::string(dir ? dir : ".") + "/.web/lora.txt";
}

static void cfg_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    web_hw_lora_defaults(&s_cfg);
    s_cfg.listen = false;               /* receive only once the desktop asks */
    FILE *f = fopen(cfg_path().c_str(), "rb");
    if (!f) return;
    web_hw_lora_cfg_t c;
    int listen;
    if (fscanf(f, "%f %f %d %d %d %d %d", &c.freq, &c.bw, &c.sf, &c.cr, &c.power, &c.sync_word, &listen) == 7) {
        c.listen = listen != 0;
        s_cfg = c;
    }
    fclose(f);
}

static void cfg_save(void)
{
    std::string p = cfg_path(), tmp = p + ".tmp";
    mkdir(p.substr(0, p.rfind('/')).c_str(), 0775);
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "%.4f %.2f %d %d %d %d %d\n", s_cfg.freq, s_cfg.bw, s_cfg.sf, s_cfg.cr, s_cfg.power, s_cfg.sync_word,
            s_cfg.listen ? 1 : 0);
    fclose(f);
    remove(p.c_str());                  /* FAT's rename does not replace */
    rename(tmp.c_str(), p.c_str());
}

static std::string cfg_json(const web_hw_lora_cfg_t &c)
{
    return "{\"freq\":" + tools_num(c.freq, 3) + ",\"bw\":" + tools_num(c.bw, c.bw == (int)c.bw ? 0 : 2) +
           ",\"sf\":" + tools_int(c.sf) + ",\"cr\":" + tools_int(c.cr) + ",\"power\":" + tools_int(c.power) +
           ",\"sync_word\":" + tools_int(c.sync_word) + ",\"listen\":" + (c.listen ? "true" : "false") + "}";
}

/* Merge a PATCH body into c. Answers 400 and returns false on a bad value. */
static bool cfg_merge(web_req_t *r, const std::string &body, web_hw_lora_cfg_t &c)
{
    static const struct { const char *key; double lo, hi; bool whole; } RANGES[] = {
        { "freq", 150, 960, false }, { "sf", 5, 12, true }, { "cr", 5, 8, true },
        { "power", -9, 22, true }, { "sync_word", 0, 255, true },
    };
    double v;
    for (const auto &x : RANGES) {
        if (!web_json_has(body, x.key)) continue;
        if (!web_json_get_num(body, x.key, &v) || v < x.lo || v > x.hi || (x.whole && v != (int)v)) {
            char msg[48];
            snprintf(msg, sizeof(msg), "%s: %g-%g", x.key, x.lo, x.hi);
            web_resp_error(r, 400, "bad_request", msg);
            return false;
        }
        if (!strcmp(x.key, "freq")) c.freq = (float)v;
        else if (!strcmp(x.key, "sf")) c.sf = (int)v;
        else if (!strcmp(x.key, "cr")) c.cr = (int)v;
        else if (!strcmp(x.key, "power")) c.power = (int)v;
        else c.sync_word = (int)v;
    }
    if (web_json_has(body, "bw")) {
        bool known = web_json_get_num(body, "bw", &v);
        if (known) {
            known = false;
            for (float b : BANDWIDTHS) {
                if (fabs(v - b) < 0.01) {
                    c.bw = b;
                    known = true;
                }
            }
        }
        if (!known) {
            web_resp_error(r, 400, "bad_request", "bw: 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250 or 500");
            return false;
        }
    }
    if (web_json_has(body, "listen") && !web_json_get_bool(body, "listen", &c.listen)) {
        web_resp_error(r, 400, "bad_request", "listen");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- packet log */

static std::string packet_json(const lora_packet_t &p)
{
    return "{\"n\":" + tools_int(p.n) + ",\"dir\":" + (p.tx ? "\"tx\"" : "\"rx\"") + ",\"ts\":" + tools_int(p.ts) +
           ",\"rssi\":" + (p.tx ? "null" : tools_int(p.rssi)) + ",\"snr\":" + (p.tx ? "null" : tools_int(p.snr)) +
           ",\"len\":" + tools_int((long long)p.data.size()) + ",\"data_b64\":\"" +
           tools_b64_encode((const uint8_t *)p.data.data(), p.data.size()) + "\"}";
}

static lora_packet_t log_add(bool tx, const std::string &data, int rssi, int snr)
{
    lora_packet_t p { 0, tx, tools_epoch(), rssi, snr, data };
    std::lock_guard<std::mutex> lk(s_log_mx);
    p.n = ++s_log_n;
    s_log.push_back(p);
    while (s_log.size() > LOG_KEEP) s_log.pop_front();
    return p;
}

/* ---------------------------------------------------------------- radio (UI thread) */

static bool radio_take(void)
{
    if (s_owned) return true;
    cfg_load();
    s_owned = web_hw_lora_apply(s_cfg);
    return s_owned;
}

static void radio_release(void)
{
    if (!s_owned) return;
    web_hw_lora_release();
    s_owned = false;
    s_tx_pending = false;
    s_tx_queue.clear();
}

static void lora_tick(uint32_t now)
{
    (void)now;
    if (!s_owned) return;
    if (s_tx_pending && web_hw_lora_tx_done()) {
        s_tx_pending = false;
        if (s_tx_queue.empty()) web_hw_lora_apply(s_cfg);   /* back to receiving, or standby */
    }
    if (!s_tx_pending && !s_tx_queue.empty()) {
        std::string next = s_tx_queue.front();
        s_tx_queue.pop_front();
        s_tx_pending = web_hw_lora_send((const uint8_t *)next.data(), next.size());
        if (!s_tx_pending) web_plat_log("[web] LoRa send failed (%u bytes)", (unsigned)next.size());
    }
    if (s_tx_pending || !s_cfg.listen) return;
    uint8_t buf[MAX_PAYLOAD + 1];
    size_t len = 0;
    int rssi = 0, snr = 0;
    if (web_hw_lora_rx(buf, &len, &rssi, &snr) && len) {
        lora_packet_t p = log_add(false, std::string((const char *)buf, len), rssi, snr);
        web_events_publish("lora.rx", packet_json(p).c_str());
    }
}

static void lora_second(uint32_t now)
{
    (void)now;
    if (s_owned && tools_holder(APPS)) {
        web_plat_log("[web] %s opened on the watch: LoRa released", tools_current_app());
        radio_release();
    }
}

/* ---------------------------------------------------------------- routes */

static void config_route(web_req_t *r, bool patch)
{
    if (tools_busy(r, APPS)) return;
    web_hw_lora_cfg_t c;
    web_ui_run([&] { cfg_load(); c = s_cfg; });
    if (patch) {
        std::string body;
        if (!tools_read_json(r, body) || !cfg_merge(r, body, c)) return;
        bool ok = false;
        web_ui_run([&] {
            s_cfg = c;
            cfg_save();
            ok = s_owned = web_hw_lora_apply(s_cfg);
        });
        if (!ok) {
            web_resp_error(r, 500, "internal", "the radio did not take the settings");
            return;
        }
    }
    web_resp_json(r, 200, cfg_json(c));
}

static void send_route(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, field, data;
    if (!tools_read_json(r, body, 1024)) return;
    if (web_json_get_str(body, "text", field)) {
        data = field;
    } else if (!web_json_get_str(body, "data_b64", field) || !tools_b64_decode(field, data)) {
        web_resp_error(r, 400, "bad_request", "text or data_b64");
        return;
    }
    if (data.empty() || data.size() > MAX_PAYLOAD) {
        web_resp_error(r, 400, "bad_request", "1-255 bytes");
        return;
    }
    bool ok = false, full = false;
    web_ui_run([&] {
        full = s_tx_queue.size() >= TX_QUEUE;
        if (full || !radio_take()) return;
        s_tx_queue.push_back(data);         /* lora_tick sends it */
        ok = true;
    });
    if (full) {
        web_resp_error(r, 409, "conflict", "too many packets waiting to be sent");
        return;
    }
    if (!ok) {
        web_resp_error(r, 500, "internal", "the radio did not take the settings");
        return;
    }
    log_add(true, data, 0, 0);
    web_resp_ok(r, ("\"len\":" + tools_int((long long)data.size())).c_str());
}

static void log_route(web_req_t *r)
{
    std::string j = "{\"packets\":[";
    std::lock_guard<std::mutex> lk(s_log_mx);
    for (size_t i = 0; i < s_log.size(); i++) {
        if (i) j += ",";
        j += packet_json(s_log[i]);
    }
    web_resp_json(r, 200, j + "]}");
}

#ifndef ARDUINO
static void dev_inject(web_req_t *r)
{
    std::string body, field, data;
    web_read_body(r, body, 1024);
    if (web_json_get_str(body, "text", field)) data = field;
    else if (web_json_get_str(body, "data_b64", field)) tools_b64_decode(field, data);
    if (data.empty() || data.size() > MAX_PAYLOAD) {
        web_resp_error(r, 400, "bad_request", "text or data_b64, 1-255 bytes");
        return;
    }
    double rssi = -70, snr = 8;
    web_json_get_num(body, "rssi", &rssi);
    web_json_get_num(body, "snr", &snr);
    web_hw_sim_lora_inject(data, (int)rssi, (int)snr);
    bool received = false;
    web_ui_run([&] { received = s_owned && s_cfg.listen; });
    web_resp_ok(r, received ? "\"received\":true" : "\"received\":false");
}
#endif

static bool lora_handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/lora")) return dev_inject(r), true;
#endif
    if (strncmp(r->path, "/api/lora/", 10)) return false;
    if (tools_is(r, "GET", "/api/lora/config")) config_route(r, false);
    else if (tools_is(r, "PATCH", "/api/lora/config")) config_route(r, true);
    else if (tools_is(r, "POST", "/api/lora/send")) send_route(r);
    else if (tools_is(r, "GET", "/api/lora/log")) log_route(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "lora", NULL };
const web_tool_t web_tool_lora = { CAPS, lora_handle, lora_tick, lora_second, radio_release };

#endif /* FACTORY_WEB */
