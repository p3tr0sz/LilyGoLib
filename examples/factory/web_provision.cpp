/**
 * @file      web_provision.cpp
 * @brief     Wi-Fi setup from a phone (see web_provision.h).
 *
 * Flow: web_provision_start() claims the Wi-Fi radio, raises a WPA2 SoftAP named
 * "T-Watch-Setup" with a random 8-digit PIN as its passphrase, and starts a captive
 * DNS server. The phone joins the AP (the watch shows the name + PIN + a QR code),
 * any page request is answered with the setup page, and the page's JavaScript talks to:
 *   GET  /prov/scan     -> {"scanning":bool,"networks":[{"ssid","rssi","open"}]}
 *   POST /prov/connect  {"ssid","pass"} -> {"ok":true}
 *   GET  /prov/status   -> {"state":"waiting|connecting|connected|failed","ssid","ip"}
 * On success the hardware layer saves the credentials (hw_wifi_remember_connected on
 * GOT_IP), so the watch rejoins on its own next boot.
 *
 * SoftAP / DNS are Arduino-only; the simulator stubs them and returns a canned scan
 * list and a fake successful connect, so the page and endpoints can be driven there.
 */

#include "web_provision.h"

#if FACTORY_WEB

#include "web_internal.h"
#include "web_tools.h"
#include "hal_interface.h"

#include <lvgl.h>
#include <string>
#include <vector>
#include <string.h>
#include <stdio.h>

#ifdef ARDUINO
#include <WiFi.h>
#include <DNSServer.h>
#endif

#define PROV_RADIO_OWNER   "provision"
#define PROV_AP_SSID       "T-Watch-Setup"
#define PROV_CONNECT_MS    25000      /* give the join this long before calling it failed */
#define PROV_SCAN_STALE_MS 20000      /* re-scan if the last results are older than this */

static bool             s_active;
static web_prov_state_t s_state = WEB_PROV_IDLE;
static char             s_psk[16];
static char             s_ap_ip[16] = "192.168.4.1";
static char             s_result_ip[16];
static char             s_target[WIFI_SSID_MAX_LEN];
static uint32_t         s_connect_deadline;
static uint32_t         s_last_scan_ms;
static bool             s_scan_kicked;
static bool             s_started_server;   /* we turned the web transport on just for setup */
static lv_timer_t      *s_timer;

#ifdef ARDUINO
static DNSServer        s_dns;
#endif

/* ---------------------------------------------------------------- helpers */

static void make_pin(void)
{
    uint8_t b[8];
    web_plat_random(b, sizeof(b));
    for (size_t i = 0; i < 8; i++) s_psk[i] = '0' + (b[i] % 10);
    s_psk[8] = 0;
}

/* Kick a scan if nothing fresh is in flight, then report whatever results we have. */
static void handle_scan(web_req_t *r)
{
    bool scanning = false;
    std::vector<wifi_scan_params_t> list;
    uint32_t now = lv_tick_get();
    bool stale = (s_last_scan_ms == 0) || (now - s_last_scan_ms > PROV_SCAN_STALE_MS);

    web_ui_run([&] {
        scanning = hw_get_wifi_scanning();
        if (!scanning && (stale || !s_scan_kicked)) {
            hw_set_wifi_scan();
            s_scan_kicked = true;
            s_last_scan_ms = lv_tick_get();
            scanning = true;
        } else {
            hw_get_wifi_scan_result(list);
            if (!list.empty()) s_last_scan_ms = lv_tick_get();
        }
    });

    /* Strongest first; skip blanks and duplicate names. */
    std::string j = "{\"scanning\":";
    j += scanning ? "true" : "false";
    j += ",\"networks\":[";
    std::vector<std::string> seen;
    bool first = true;
    for (const auto &ap : list) {
        if (!ap.ssid[0]) continue;
        bool dup = false;
        for (const auto &s : seen) if (s == ap.ssid) { dup = true; break; }
        if (dup) continue;
        seen.push_back(ap.ssid);
        if (!first) j += ",";
        first = false;
        j += "{\"ssid\":";
        web_json_str(j, ap.ssid);
        j += ",\"rssi\":";
        j += std::to_string((int)ap.rssi);
        j += ",\"open\":";
        j += (ap.authmode == 0) ? "true" : "false";   /* WIFI_AUTH_OPEN */
        j += "}";
    }
    j += "]}";
    web_resp_json(r, 200, j);
}

static void handle_connect(web_req_t *r)
{
    std::string body, ssid, pass;
    if (!web_read_body(r, body, 512) || !web_json_get_str(body, "ssid", ssid) || ssid.empty()) {
        web_resp_error(r, 400, "bad_request", "ssid");
        return;
    }
    web_json_get_str(body, "pass", pass);   /* empty for an open network */

    wifi_conn_params_t params;
    memset(&params, 0, sizeof(params));
    strncpy(params.ssid, ssid.c_str(), sizeof(params.ssid) - 1);
    strncpy(params.password, pass.c_str(), sizeof(params.password) - 1);

    strncpy(s_target, params.ssid, sizeof(s_target) - 1);
    s_target[sizeof(s_target) - 1] = 0;
    s_result_ip[0] = 0;
    s_state = WEB_PROV_CONNECTING;
    s_connect_deadline = lv_tick_get() + PROV_CONNECT_MS;

    web_ui_run([&] { hw_set_wifi_connect(params); });
    web_plat_log("[prov] joining \"%s\"", s_target);
    web_resp_ok(r);
}

static const char *state_name(web_prov_state_t s)
{
    switch (s) {
        case WEB_PROV_CONNECTING: return "connecting";
        case WEB_PROV_CONNECTED:  return "connected";
        case WEB_PROV_FAILED:     return "failed";
        default:                  return "waiting";
    }
}

static void handle_status(web_req_t *r)
{
    std::string j = "{\"state\":";
    web_json_str(j, state_name(s_state));
    j += ",\"ssid\":";
    web_json_str(j, s_target);
    j += ",\"ip\":";
    web_json_str(j, s_result_ip);
    j += "}";
    web_resp_json(r, 200, j);
}

/* ---------------------------------------------------------------- the captive page */

static const char PROV_PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Watch Wi-Fi setup</title><style>"
"*{box-sizing:border-box}body{margin:0;font:16px/1.4 -apple-system,system-ui,sans-serif;"
"background:#111;color:#eee;padding:18px}h1{font-size:20px;margin:0 0 4px}"
".sub{color:#9a9a9a;margin:0 0 18px}ul{list-style:none;padding:0;margin:0}"
"li{background:#1c1c1e;border-radius:12px;padding:14px 16px;margin-bottom:8px;"
"display:flex;justify-content:space-between;align-items:center;cursor:pointer}"
"li .n{font-weight:600}li .s{color:#9a9a9a;font-size:14px}"
"input,button{width:100%;font:16px inherit;padding:14px;border-radius:12px;border:0;margin-top:8px}"
"input{background:#1c1c1e;color:#eee}button{background:#0a84ff;color:#fff;font-weight:600}"
"button.sec{background:#2c2c2e;color:#eee}.hide{display:none}"
".msg{text-align:center;padding:24px 0}.msg .big{font-size:42px}.ok{color:#30d158}.err{color:#ff453a}"
"</style></head><body>"
"<h1>Connect your watch</h1><p class=sub id=sub>Pick your home Wi-Fi network.</p>"
"<ul id=list></ul>"
"<div id=form class=hide><input id=pass type=password placeholder='Wi-Fi password' autocomplete=off>"
"<button id=go>Connect</button><button class=sec id=back>Back</button></div>"
"<div id=result class=msg hide></div>"
"<script>"
"var list=document.getElementById('list'),sub=document.getElementById('sub'),"
"form=document.getElementById('form'),result=document.getElementById('result'),"
"pass=document.getElementById('pass'),sel=null,poll=null;"
"function scan(){fetch('/prov/scan').then(r=>r.json()).then(d=>{"
"if(result.classList.contains('hide')===false)return;"
"if(!form.classList.contains('hide'))return;"
"list.innerHTML='';d.networks.forEach(function(n){var li=document.createElement('li');"
"li.innerHTML=\"<span class=n></span><span class=s></span>\";"
"li.querySelector('.n').textContent=n.ssid;"
"li.querySelector('.s').textContent=(n.open?'open':'\\uD83D\\uDD12')+' '+n.rssi+' dBm';"
"li.onclick=function(){pick(n)};list.appendChild(li)});"
"if(!d.networks.length)sub.textContent=d.scanning?'Scanning\\u2026':'No networks found. Retrying\\u2026';"
"else sub.textContent='Pick your home Wi-Fi network.';});}"
"function pick(n){sel=n;form.classList.remove('hide');list.classList.add('hide');"
"sub.textContent=n.ssid;pass.value='';pass.classList.toggle('hide',n.open);"
"if(!n.open)pass.focus();}"
"document.getElementById('back').onclick=function(){sel=null;form.classList.add('hide');"
"list.classList.remove('hide');sub.textContent='Pick your home Wi-Fi network.';};"
"document.getElementById('go').onclick=function(){if(!sel)return;"
"fetch('/prov/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:sel.ssid,pass:pass.value})}).then(function(){show('connecting');"
"if(poll)clearInterval(poll);poll=setInterval(status,1500);status();});};"
"function status(){fetch('/prov/status').then(r=>r.json()).then(d=>{"
"if(d.state==='connected'){clearInterval(poll);show('connected',d);}"
"else if(d.state==='failed'){clearInterval(poll);show('failed',d);}});}"
"function show(st,d){form.classList.add('hide');list.classList.add('hide');"
"sub.classList.add('hide');result.classList.remove('hide');"
"if(st==='connecting')result.innerHTML=\"<div class=big>\\u23F3</div><p>Connecting\\u2026</p>\";"
"else if(st==='connected')result.innerHTML=\"<div class='big ok'>\\u2713</div><p>Connected to \"+(sel?sel.ssid:'')+\".<br>You can close this page.</p>\";"
"else result.innerHTML=\"<div class='big err'>\\u2715</div><p>Could not connect.</p><button class=sec id=retry>Try again</button>\";"
"var rt=document.getElementById('retry');if(rt)rt.onclick=function(){location.reload()};}"
"scan();setInterval(scan,1500);"
"</script></body></html>";

static void serve_page(web_req_t *r)
{
    web_resp_html(r, 200, PROV_PAGE);
}

/* ---------------------------------------------------------------- tick: DNS + join progress */

static void prov_tick(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_active) return;
#ifdef ARDUINO
    s_dns.processNextRequest();
#endif
    if (s_state == WEB_PROV_CONNECTING) {
        if (hw_get_wifi_connected()) {
            s_state = WEB_PROV_CONNECTED;
#ifdef ARDUINO
            strncpy(s_result_ip, WiFi.localIP().toString().c_str(), sizeof(s_result_ip) - 1);
            s_result_ip[sizeof(s_result_ip) - 1] = 0;
#else
            strcpy(s_result_ip, "192.168.1.42");
#endif
            web_plat_log("[prov] connected, %s", s_result_ip);
        } else if ((int32_t)(lv_tick_get() - s_connect_deadline) >= 0) {
            s_state = WEB_PROV_FAILED;
            web_plat_log("[prov] join timed out");
        }
    }
}

/* ---------------------------------------------------------------- start / stop */

bool web_provision_start(void)
{
    if (s_active) return true;

    make_pin();
    s_target[0] = 0;
    s_result_ip[0] = 0;
    s_last_scan_ms = 0;
    s_scan_kicked = false;
    s_state = WEB_PROV_WAITING;

    /* The captive page is served by the web transport; bring it up if it is off. */
    s_started_server = false;
    if (!web_server_running()) {
        if (!web_server_start()) {
            web_plat_log("[prov] web server did not start");
            s_state = WEB_PROV_IDLE;
            return false;
        }
        s_started_server = true;
    }

#ifdef ARDUINO
    std::string err;
    if (!tools_wifi_claim(PROV_RADIO_OWNER, err)) {
        web_plat_log("[prov] radio busy: %s", err.c_str());
        if (s_started_server) { web_server_stop(); s_started_server = false; }
        s_state = WEB_PROV_IDLE;
        return false;
    }
    IPAddress gw(192, 168, 4, 1);
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAPConfig(gw, gw, IPAddress(255, 255, 255, 0)) ||
        !WiFi.softAP(PROV_AP_SSID, s_psk, 1, 0, 4)) {
        WiFi.softAPdisconnect(true);
        tools_wifi_release(PROV_RADIO_OWNER);
        if (s_started_server) { web_server_stop(); s_started_server = false; }
        web_plat_log("[prov] could not start the setup AP");
        s_state = WEB_PROV_IDLE;
        return false;
    }
    strncpy(s_ap_ip, WiFi.softAPIP().toString().c_str(), sizeof(s_ap_ip) - 1);
    s_ap_ip[sizeof(s_ap_ip) - 1] = 0;
    s_dns.start(53, "*", gw);
#else
    strcpy(s_ap_ip, "192.168.4.1");
#endif

    s_active = true;
    if (!s_timer) s_timer = lv_timer_create(prov_tick, 200, NULL);
    else lv_timer_resume(s_timer);
    web_plat_log("[prov] setup AP \"%s\" up, PIN %s", PROV_AP_SSID, s_psk);
    return true;
}

void web_provision_stop(void)
{
    if (!s_active) return;
    s_active = false;
    if (s_timer) lv_timer_pause(s_timer);
#ifdef ARDUINO
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    tools_wifi_release(PROV_RADIO_OWNER);
#endif
    if (s_started_server) { web_server_stop(); s_started_server = false; }
    s_state = WEB_PROV_IDLE;
}

bool web_provision_active(void)            { return s_active; }
web_prov_state_t web_provision_state(void) { return s_state; }
const char *web_provision_ap_ssid(void)    { return PROV_AP_SSID; }
const char *web_provision_ap_psk(void)     { return s_psk; }
const char *web_provision_ap_ip(void)      { return s_ap_ip; }
const char *web_provision_target_ssid(void){ return s_target; }

/* ---------------------------------------------------------------- captive hook */

bool web_provision_captive(web_req_t *r)
{
    if (!s_active) return false;

    if (!strcmp(r->path, "/prov/scan")) {
        if (strcmp(r->method, "GET")) { web_resp_error(r, 405, "bad_request", "GET"); return true; }
        handle_scan(r);
        return true;
    }
    if (!strcmp(r->path, "/prov/connect")) {
        if (strcmp(r->method, "POST")) { web_resp_error(r, 405, "bad_request", "POST"); return true; }
        handle_connect(r);
        return true;
    }
    if (!strcmp(r->path, "/prov/status")) {
        if (strcmp(r->method, "GET")) { web_resp_error(r, 405, "bad_request", "GET"); return true; }
        handle_status(r);
        return true;
    }

    /* Leave the JSON API alone, so the web desktop and screen endpoints keep working
     * while setup is open. Phone captive-detection probes are non-/api URLs. */
    if (!strncmp(r->path, "/api/", 5)) return false;

    /* Any other page request while the setup AP is up: the setup page (captive redirect). */
    if (!strcmp(r->method, "GET") || !strcmp(r->method, "HEAD")) {
        serve_page(r);
        return true;
    }
    web_resp_error(r, 405, "bad_request", "GET");
    return true;
}

#endif /* FACTORY_WEB */
