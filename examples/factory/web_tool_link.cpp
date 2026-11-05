/**
 * @file      web_tool_link.cpp
 * @brief     Link: ESP-NOW file transfer between watches (docs/web-api.md, "Link").
 *
 *            The whole transfer is in the hardware layer (a real ESP-NOW protocol on the watch,
 *            a fake in the simulator). This module shows the state, starts a send, and turns the
 *            radio on and off; the tick moves the transfer along and publishes `link` and, when a
 *            file arrives, `files.changed`.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>

static std::string s_last_sig;          /* transfer+enabled signature, to publish only on a change */
static std::string s_last_peers;
static std::string s_rx_announced;      /* the received file we last fired files.changed for */

static std::string transfer_json(const web_hw_link_transfer_t &t)
{
    std::string j = std::string("{\"dir\":") + (t.tx ? "\"tx\"" : "\"rx\"") + ",\"peer\":";
    web_json_str(j, t.peer);
    j += ",\"name\":";
    web_json_str(j, t.name);
    j += ",\"size\":" + tools_int(t.size) + ",\"done\":" + tools_int(t.done) + ",\"state\":";
    web_json_str(j, t.state ? t.state : "error");
    j += ",\"error\":";
    if (t.error) web_json_str(j, t.error); else j += "null";
    return j + "}";
}

static std::string peers_json(void)
{
    web_hw_peer_t peers[16];
    int n = web_hw_link_peers(peers, 16);
    std::string j = "[";
    for (int i = 0; i < n; i++) {
        if (i) j += ",";
        j += "{\"mac\":";
        web_json_str(j, peers[i].mac);
        j += ",\"name\":";
        web_json_str(j, peers[i].name);
        j += ",\"rssi\":" + tools_int(peers[i].rssi) + ",\"last_seen\":" + tools_int(peers[i].last_seen) + "}";
    }
    return j + "]";
}

static std::string link_json(void)
{
    char mac[18], name[32];
    web_hw_link_self(mac, sizeof(mac), name, sizeof(name));
    std::string j = std::string("{\"enabled\":") + (web_hw_link_enabled() ? "true" : "false") + ",\"mac\":";
    web_json_str(j, mac);
    j += ",\"name\":";
    web_json_str(j, name);
    j += ",\"peers\":" + peers_json() + ",\"transfer\":";
    web_hw_link_transfer_t t;
    j += web_hw_link_transfer(&t) ? transfer_json(t) : std::string("null");
    return j + "}";
}

static void get_link(web_req_t *r)
{
    std::string j;
    web_ui_run([&] { j = link_json(); });
    web_resp_json(r, 200, j);
}

static void enable_route(web_req_t *r)
{
    std::string body;
    if (!tools_read_json(r, body)) return;
    bool on = true;
    if (!web_json_get_bool(body, "on", &on)) {
        web_resp_error(r, 400, "bad_request", "on");
        return;
    }
    bool enabled = false;
    web_ui_run([&] { web_hw_link_set_enabled(on); enabled = web_hw_link_enabled(); });
    web_plat_log("[web] ESP-NOW link %s", enabled ? "on" : "off");
    web_resp_ok(r, enabled ? "\"enabled\":true" : "\"enabled\":false");
}

static void send_route(web_req_t *r)
{
    std::string body, path, mac;
    if (!tools_read_json(r, body)) return;
    if (!web_json_get_str(body, "path", path) || !web_json_get_str(body, "to_mac", mac)) {
        web_resp_error(r, 400, "bad_request", "path and to_mac");
        return;
    }
    std::string host;
    int st = web_fs_resolve(path.c_str(), host);
    if (st == 503) {
        web_resp_error(r, 503, "sd_missing", "SD card not mounted");
        return;
    }
    if (st) {
        web_resp_error(r, 400, "bad_request", "bad path");
        return;
    }
    const char *slash = strrchr(path.c_str(), '/');
    std::string name = slash ? slash + 1 : path;
    int rc = 0;
    web_ui_run([&] { rc = web_hw_link_send(host.c_str(), name.c_str(), mac.c_str()); });
    if (rc == 404) web_resp_error(r, 404, "not_found", "no such peer");
    else if (rc == 409) web_resp_error(r, 409, "conflict", "a transfer is already in flight");
    else if (rc == 503) web_resp_error(r, 503, "sd_missing", "could not read the file");
    else if (rc) web_resp_error(r, 500, "internal", "the transfer did not start");
    else {
        web_plat_log("[web] ESP-NOW send %s -> %s", name.c_str(), mac.c_str());
        web_resp_json(r, 202, "{\"ok\":true}");
    }
}

static void link_tick(uint32_t now)
{
    (void)now;
    web_hw_link_poll();
    web_hw_link_transfer_t t;
    bool active = web_hw_link_transfer(&t);

    /* A received file just finished: let the file views know (once per file). */
    if (active && !t.tx && t.state && !strcmp(t.state, "done")) {
        std::string file = std::string("/sd/data/link/") + t.name;
        if (file != s_rx_announced) {
            s_rx_announced = file;
            std::string ev = "{\"op\":\"write\",\"path\":";
            web_json_str(ev, file.c_str());
            web_events_publish("files.changed", (ev + "}").c_str());
        }
    }

    char sig[160];
    snprintf(sig, sizeof(sig), "%d/%s/%s/%u/%u", web_hw_link_enabled() ? 1 : 0,
             active ? (t.tx ? "tx" : "rx") : "-", active && t.state ? t.state : "-",
             active ? (unsigned)t.done : 0, active ? (unsigned)t.size : 0);
    std::string peers = peers_json();
    if (sig == s_last_sig && peers == s_last_peers) return;
    s_last_sig = sig;
    s_last_peers = peers;
    web_events_publish("link", link_json().c_str());
}

static void link_stop(void)
{
    web_hw_link_cancel();
    web_hw_link_set_enabled(false);
}

static bool link_handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/link")) {
        std::string body, name;
        web_read_body(r, body, 512);
        if (web_json_has(body, "rx")) {                     /* the inner {name,size} is the mock's; the sim uses a default */
            web_ui_run([&] { web_hw_sim_link_rx("incoming.bin", 4096); });
        } else {
            name = "T-Watch-9F2A";
            web_json_get_str(body, "peer", name);
            web_ui_run([&] { web_hw_sim_link_peer(name.c_str()); });
        }
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/link") && strncmp(r->path, "/api/link/", 10)) return false;
    if (tools_is(r, "GET", "/api/link")) get_link(r);
    else if (tools_is(r, "POST", "/api/link/enable")) enable_route(r);
    else if (tools_is(r, "POST", "/api/link/send")) send_route(r);
    else if (tools_is(r, "POST", "/api/link/cancel")) {
        web_ui_run([] { web_hw_link_cancel(); });
        web_resp_ok(r);
    } else web_resp_error(r, 404, "not_found");
    return true;
}

static const char *const CAPS[] = { "link.espnow", NULL };
const web_tool_t web_tool_link = { CAPS, link_handle, link_tick, NULL, link_stop };

#endif /* FACTORY_WEB */
