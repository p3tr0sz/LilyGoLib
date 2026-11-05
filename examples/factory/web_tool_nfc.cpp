/**
 * @file      web_tool_nfc.cpp
 * @brief     NFC (docs/web-api.md, "NFC"): the reader on for a while, tags as events.
 *            Tags arrive through web_tool_nfc_tag() from the reader's callback.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <mutex>

static const char *const APPS[] = { "NFC", "NFC Card", "PN532", NULL };

static std::mutex s_mx;
static bool s_scanning = false;         /* written on the UI thread, read anywhere under s_mx */
static uint32_t s_until = 0;            /* UI thread */
static std::string s_last = "null";

static bool scanning(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    return s_scanning;
}

static void set_scanning(bool on)       /* UI thread */
{
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_scanning == on) return;
        s_scanning = on;
    }
    web_events_publish("nfc.state", on ? "{\"scanning\":true}" : "{\"scanning\":false}");
}

static void reader_stop(void)           /* UI thread */
{
    if (!scanning()) return;
    web_hw_nfc_stop();
    set_scanning(false);
}

void web_tool_nfc_tag(const web_nfc_tag_t &tag)
{
    std::string j = "{\"uid\":";
    web_json_str(j, tag.uid);
    j += ",\"type\":";
    web_json_str(j, tag.type);
    j += ",\"ts\":" + tools_int(tools_epoch()) + ",\"ndef\":[";
    for (int i = 0; i < tag.nrec && i < 4; i++) {
        if (i) j += ",";
        j += "{\"kind\":";
        web_json_str(j, tag.rec[i].kind);
        j += ",\"value\":";
        web_json_str(j, tag.rec[i].value);
        j += "}";
    }
    j += "]}";
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_scanning) return;        /* the watch's own NFC app read it */
        s_last = j;
    }
    web_plat_log("[web] NFC tag %s (%s)", tag.uid, tag.type);
    web_events_publish("nfc.tag", j.c_str());
}

static void nfc_tick(uint32_t now)
{
    if (!scanning()) return;
    web_hw_nfc_loop();
    if ((int32_t)(now - s_until) >= 0) reader_stop();
}

static void nfc_second(uint32_t now)
{
    (void)now;
    if (scanning() && tools_holder(APPS)) reader_stop();
}

static void scan_route(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;
    double secs = 20;
    if (web_json_has(body, "seconds") && (!web_json_get_num(body, "seconds", &secs) || secs < 1 || secs > 60)) {
        web_resp_error(r, 400, "bad_request", "seconds: 1-60");
        return;
    }
    if (tools_busy(r, APPS)) return;
    bool ok = false;
    web_ui_run([&] {
        ok = scanning() || web_hw_nfc_start();
        if (!ok) return;
        s_until = tools_now_ms() + (uint32_t)(secs * 1000);
        set_scanning(true);
    });
    if (!ok) {
        web_resp_error(r, 500, "internal", "the NFC reader did not start");
        return;
    }
    web_resp_ok(r);
}

static void get_route(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    web_resp_json(r, 200, std::string("{\"scanning\":") + (s_scanning ? "true" : "false") + ",\"last\":" + s_last + "}");
}

#ifndef ARDUINO
static void dev_tag(web_req_t *r)
{
    std::string body, s;
    web_read_body(r, body, 1024);
    if (!scanning()) {
        web_resp_ok(r, "\"read\":false");
        return;
    }
    web_nfc_tag_t tag;
    memset(&tag, 0, sizeof(tag));
    snprintf(tag.uid, sizeof(tag.uid), "%s", web_json_get_str(body, "uid", s) ? s.c_str() : "04:11:22:33:44:55:66");
    snprintf(tag.type, sizeof(tag.type), "%s", web_json_get_str(body, "type", s) ? s.c_str() : "NTAG215");
    if (web_json_get_str(body, "text", s)) {
        tag.nrec = 1;
        tag.rec[0].kind = "text";
        snprintf(tag.rec[0].value, sizeof(tag.rec[0].value), "%s", s.c_str());
    }
    web_tool_nfc_tag(tag);
    web_resp_ok(r, "\"read\":true");
}
#endif

static bool nfc_handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/nfc")) return dev_tag(r), true;
#endif
    if (strcmp(r->path, "/api/nfc") && strncmp(r->path, "/api/nfc/", 9)) return false;
    if (tools_is(r, "GET", "/api/nfc")) get_route(r);
    else if (tools_is(r, "POST", "/api/nfc/scan")) scan_route(r);
    else if (tools_is(r, "POST", "/api/nfc/stop")) {
        web_ui_run([] { reader_stop(); });
        web_resp_ok(r);
    } else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "nfc", NULL };
const web_tool_t web_tool_nfc = { CAPS, nfc_handle, nfc_tick, nfc_second, reader_stop };

#endif /* FACTORY_WEB */
