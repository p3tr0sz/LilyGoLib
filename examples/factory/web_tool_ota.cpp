/**
 * @file      web_tool_ota.cpp
 * @brief     Firmware update (docs/web-api.md, "Firmware update"): stream a .bin into the spare
 *            OTA slot, verify it, then commit (reboot into it) or roll back. The write and the
 *            verify are in the hardware layer (the Update library on the watch, a fake in the
 *            simulator); this module streams the body and reports progress.
 *
 *            Able to brick the watch: the desktop confirms twice and checks the file first.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <memory>

#define OTA_CHUNK       4096

static std::string ota_json(void)
{
    web_hw_ota_t o;
    web_hw_ota_status(&o);
    std::string j = "{\"slot\":";
    web_json_str(j, o.slot ? o.slot : "?");
    j += ",\"other_slot\":";
    web_json_str(j, o.other_slot ? o.other_slot : "?");
    j += ",\"state\":";
    web_json_str(j, o.state ? o.state : "idle");
    j += ",\"progress\":{\"done\":" + tools_int(o.done) + ",\"total\":" + tools_int(o.total) + "}";
    j += ",\"error\":";
    if (o.error) web_json_str(j, o.error); else j += "null";
    j += ",\"can_rollback\":";
    j += o.can_rollback ? "true" : "false";
    return j + "}";
}

static void publish(void)
{
    web_events_publish("ota", ota_json().c_str());
}

static void upload_route(web_req_t *r)
{
    if (r->content_length <= 0) {
        web_resp_error(r, 400, "bad_request", "Content-Length required");
        return;
    }
    long long total = r->content_length;
    bool begun = false;
    web_ui_run([&] { begun = web_hw_ota_begin(total); });
    if (!begun) {
        web_resp_error(r, 413, "too_large", "the image does not fit the spare slot");
        return;
    }
    publish();
    std::unique_ptr<char[]> buf(new char[OTA_CHUNK]);
    long long got = 0;
    uint32_t last_event = tools_now_ms();
    bool io_ok = true;
    for (;;) {
        int n = r->read(r, buf.get(), OTA_CHUNK);
        if (n < 0) {
            io_ok = false;
            break;
        }
        if (n == 0) break;
        bool w = false;
        web_ui_run([&] { w = web_hw_ota_write((const uint8_t *)buf.get(), (size_t)n); });
        if (!w) {
            io_ok = false;
            break;
        }
        got += n;
        if (tools_now_ms() - last_event >= 300) {
            last_event = tools_now_ms();
            publish();
        }
    }
    int rc = -1;
    web_ui_run([&] {
        int e = web_hw_ota_end();       /* always finish, to release the slot */
        rc = (io_ok && got == total) ? e : -1;
    });
    publish();
    if (!io_ok || got != total) {
        web_resp_error(r, 400, "bad_image", "the upload was cut short");
        return;
    }
    if (rc) {
        web_resp_error(r, 400, "bad_image", "the image did not verify");
        return;
    }
    web_resp_ok(r, ("\"size\":" + tools_int(got)).c_str());
}

static void commit_route(web_req_t *r)
{
    web_hw_ota_t o;
    web_hw_ota_status(&o);
    if (!o.state || strcmp(o.state, "ready")) {
        web_resp_error(r, 409, "conflict", "upload a firmware image first");
        return;
    }
    web_plat_log("[web] OTA commit: rebooting into the new slot");
    web_resp_ok(r);
    web_ui_run([] { web_hw_ota_commit(); }, 2000);      /* reboots after ~1 s */
}

static void rollback_route(web_req_t *r)
{
    web_hw_ota_t o;
    web_hw_ota_status(&o);
    if (!o.can_rollback) {
        web_resp_error(r, 409, "conflict", "nothing good to roll back to");
        return;
    }
    web_plat_log("[web] OTA rollback: rebooting into the other slot");
    web_resp_ok(r);
    web_ui_run([] { web_hw_ota_rollback(); }, 2000);
}

static bool ota_handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/ota") && strncmp(r->path, "/api/ota/", 9)) return false;
    if (tools_is(r, "GET", "/api/ota")) web_resp_json(r, 200, ota_json());
    else if (tools_is(r, "POST", "/api/ota/upload")) upload_route(r);
    else if (tools_is(r, "POST", "/api/ota/commit")) commit_route(r);
    else if (tools_is(r, "POST", "/api/ota/rollback")) rollback_route(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "ota", NULL };
const web_tool_t web_tool_ota = { CAPS, ota_handle, NULL, NULL, NULL };

#endif /* FACTORY_WEB */
