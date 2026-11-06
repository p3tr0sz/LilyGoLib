/**
 * @file      web_transport_esp.cpp
 * @brief     Watch transport and platform for the web server: esp_http_server (HTTP + WebSocket),
 *            mDNS (watch.local), and the watch's SD card, flash, battery and Wi-Fi.
 *
 *            /sd    -> the SD card's VFS mount "/sd"
 *            /flash -> the internal FFat mount "/ffat"
 */
#include "web_internal.h"

#if FACTORY_WEB && defined(ARDUINO)

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <FFat.h>
#include <esp_http_server.h>
#include <esp_random.h>
#if __has_include("esp32s3/rom/miniz.h")
#include "esp32s3/rom/miniz.h"
#else
#include "rom/miniz.h"
#endif
#include <functional>
#include <LilyGoLib_Version.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

extern void instanceLockTake();
extern void instanceLockGive();

#define WEB_ESP_MAX_SOCKETS     12
#define WEB_ESP_BUFFERED_MAX    (64 * 1024)     /* answers up to this size get a Content-Length */

static httpd_handle_t s_server = NULL;
static std::mutex s_ws_lock;
static std::set<int> s_ws_fds;

/* ---------------------------------------------------------------- request adapter */

struct esp_req_t {
    httpd_req_t *req;
    long long left;
    std::string status;
    std::string type;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string buffer;
    bool buffered = false;
    bool chunked = false;
    bool begun = false;
    bool failed = false;
};

static int esp_read(web_req_t *r, char *buf, size_t n)
{
    esp_req_t *e = (esp_req_t *)r->ctx;
    if (e->left <= 0) return 0;
    if ((long long)n > e->left) n = (size_t)e->left;
    for (int tries = 0; tries < 5; tries++) {
        int k = httpd_req_recv(e->req, buf, n);
        if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (k <= 0) return -1;
        e->left -= k;
        return k;
    }
    return -1;
}

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 206: return "206 Partial Content";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 416: return "416 Range Not Satisfiable";
    case 429: return "429 Too Many Requests";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

static bool esp_begin(web_req_t *r, int status, const char *type, const char *headers, long long length)
{
    esp_req_t *e = (esp_req_t *)r->ctx;
    if (e->begun) return false;
    e->begun = true;
    /* esp_http_server keeps pointers to these strings until the answer is sent. */
    e->status = status_line(status);
    httpd_resp_set_status(e->req, e->status.c_str());
    if (type) {
        e->type = type;
        httpd_resp_set_type(e->req, e->type.c_str());
    }
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        std::string line = eol ? std::string(p, eol - p) : std::string(p);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            e->headers.emplace_back(line.substr(0, colon), v);
        }
        p = eol ? eol + 2 : NULL;
    }
    for (auto &h : e->headers) httpd_resp_set_hdr(e->req, h.first.c_str(), h.second.c_str());
    e->buffered = length >= 0 && length <= WEB_ESP_BUFFERED_MAX;
    e->chunked = !e->buffered;
    return true;
}

static bool esp_write(web_req_t *r, const char *d, size_t n)
{
    esp_req_t *e = (esp_req_t *)r->ctx;
    if (e->failed) return false;
    if (n == 0) return true;
    if (e->buffered) {
        e->buffer.append(d, n);
        return true;
    }
    if (httpd_resp_send_chunk(e->req, d, n) != ESP_OK) e->failed = true;
    return !e->failed;
}

static void esp_end(web_req_t *r)
{
    esp_req_t *e = (esp_req_t *)r->ctx;
    if (!e->begun) esp_begin(r, 500, "text/plain", NULL, 0);
    if (e->failed) return;
    if (e->buffered) httpd_resp_send(e->req, e->buffer.data(), e->buffer.size());
    else httpd_resp_send_chunk(e->req, NULL, 0);
}

static void get_header(httpd_req_t *req, const char *name, char *out, size_t n)
{
    out[0] = 0;
    if (httpd_req_get_hdr_value_len(req, name) > 0) httpd_req_get_hdr_value_str(req, name, out, n);
}

static const char *method_name(int m)
{
    switch (m) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PATCH: return "PATCH";
    case HTTP_HEAD: return "HEAD";
    default: return "OTHER";
    }
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t http_handler(httpd_req_t *req)
{
    /* Big buffers (paths, query, headers) live on the heap, not the httpd task's stack. */
    std::unique_ptr<web_req_t> r(new web_req_t());
    esp_req_t e;
    e.req = req;
    e.left = req->content_len;

    snprintf(r->method, sizeof(r->method), "%s", method_name(req->method));
    const char *uri = req->uri;
    const char *q = strchr(uri, '?');
    size_t plen = q ? (size_t)(q - uri) : strlen(uri);
    size_t o = 0;
    for (size_t i = 0; i < plen && o + 1 < sizeof(r->path); i++) {
        if (uri[i] == '%' && i + 2 < plen && hexv(uri[i + 1]) >= 0 && hexv(uri[i + 2]) >= 0) {
            r->path[o++] = (char)(hexv(uri[i + 1]) * 16 + hexv(uri[i + 2]));
            i += 2;
        } else {
            r->path[o++] = uri[i];
        }
    }
    r->path[o] = 0;
    if (q) snprintf(r->query, sizeof(r->query), "%s", q + 1);
    get_header(req, "Cookie", r->cookie, sizeof(r->cookie));
    get_header(req, "Range", r->range, sizeof(r->range));
    get_header(req, "Accept-Encoding", r->accept_encoding, sizeof(r->accept_encoding));
    get_header(req, "If-None-Match", r->if_none_match, sizeof(r->if_none_match));
    get_header(req, "User-Agent", r->user_agent, sizeof(r->user_agent));
    r->content_length = (long long)req->content_len;

    r->ctx = &e;
    r->read = esp_read;
    r->begin = esp_begin;
    r->write = esp_write;
    r->end = esp_end;
    web_api_handle(r.get());

    /* Drain what the handler did not read, so the connection can be reused. */
    char sink[256];
    while (e.left > 0 && esp_read(r.get(), sink, sizeof(sink)) > 0) {}
    return e.failed ? ESP_FAIL : ESP_OK;
}

/* ---------------------------------------------------------------- WebSocket */

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        /* The handshake is done; refuse unpaired browsers by closing. */
        char cookie[320];
        get_header(req, "Cookie", cookie, sizeof(cookie));
        {
            std::lock_guard<std::mutex> lk(s_ws_lock);
            s_ws_fds.insert(fd);
        }
        if (!web_api_ws_open(fd, cookie)) {
            std::lock_guard<std::mutex> lk(s_ws_lock);
            s_ws_fds.erase(fd);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    /* Frames from the browser are ignored in API v1; read and drop them. */
    httpd_ws_frame_t f = {};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
    if (f.len) {
        std::unique_ptr<uint8_t[]> buf(new uint8_t[f.len]);
        f.payload = buf.get();
        if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

struct ws_job_t {
    int fd;
    std::string text;
};

static void ws_send_work(void *arg)
{
    ws_job_t *job = (ws_job_t *)arg;
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    f.final = true;
    f.payload = (uint8_t *)job->text.data();
    f.len = job->text.size();
    if (s_server) httpd_ws_send_frame_async(s_server, job->fd, &f);
    delete job;
}

bool web_transport_ws_send(int conn, const char *text, size_t len)
{
    {
        std::lock_guard<std::mutex> lk(s_ws_lock);
        if (!s_server || !s_ws_fds.count(conn)) return false;
    }
    ws_job_t *job = new ws_job_t { conn, std::string(text, len) };
    if (httpd_queue_work(s_server, ws_send_work, job) != ESP_OK) {
        delete job;
        return false;
    }
    return true;
}

void web_transport_ws_close(int conn)
{
    if (s_server) httpd_sess_trigger_close(s_server, conn);
}

static void on_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    bool was_ws;
    {
        std::lock_guard<std::mutex> lk(s_ws_lock);
        was_ws = s_ws_fds.erase(fd) > 0;
    }
    if (was_ws) web_api_ws_closed(fd);
    close(fd);
}

/* ---------------------------------------------------------------- start / stop */

bool web_transport_start(uint16_t port)
{
    if (s_server) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port;
    cfg.ctrl_port = 32770;                  /* away from other httpd instances */
    cfg.max_open_sockets = WEB_ESP_MAX_SOCKETS;
    /* No LRU purge: it would close the desktop's idle event socket first. */
    cfg.lru_purge_enable = false;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    cfg.max_resp_headers = 12;
    cfg.stack_size = 12 * 1024;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.close_fn = on_close;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return false;
    }
    httpd_uri_t ws = {};
    ws.uri = "/api/events";
    ws.method = HTTP_GET;
    ws.handler = ws_handler;
    ws.is_websocket = true;
    httpd_register_uri_handler(s_server, &ws);

    static const httpd_method_t methods[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_PATCH, HTTP_HEAD };
    for (httpd_method_t m : methods) {
        httpd_uri_t u = {};
        u.uri = "/*";
        u.method = m;
        u.handler = http_handler;
        httpd_register_uri_handler(s_server, &u);
    }

    if (MDNS.begin("watch")) MDNS.addService("http", "tcp", port);
    return true;
}

void web_transport_stop(void)
{
    if (!s_server) return;
    MDNS.end();
    httpd_stop(s_server);
    s_server = NULL;
    std::lock_guard<std::mutex> lk(s_ws_lock);
    s_ws_fds.clear();
}

/* ---------------------------------------------------------------- platform (watch) */

static bool s_mount_failed = false;

const char *web_plat_root_dir(const char *root)
{
    if (!strcmp(root, "sd")) return "/sd";
    if (!strcmp(root, "flash")) return "/ffat";
    return NULL;
}

void web_plat_random(uint8_t *buf, size_t n)
{
    esp_fill_random(buf, n);
}

void web_plat_sd_info(web_sd_info_t *out, bool with_space)
{
    memset(out, 0, sizeof(*out));
    out->mounted = hw_is_sd_insert();
    out->present = hw_has_sd_detect_pin() ? hw_is_sd_card_inserted() : out->mounted;
    if (!out->present) s_mount_failed = false;
    if (out->mounted) {
        if (with_space) {
            out->total = SD.totalBytes();
            out->free = out->total - SD.usedBytes();
        }
        switch (SD.cardType()) {
        case CARD_MMC: out->card = "MMC"; break;
        case CARD_SD: out->card = "SDSC"; break;
        case CARD_SDHC: out->card = "SDHC"; break;
        default: out->card = "unknown"; break;
        }
    } else if (out->present) {
        out->card = "unknown";
    }
    if (out->present && !out->mounted && s_mount_failed) out->error = "mount_failed";
}

bool web_plat_sd_mount(void)
{
    if (hw_is_sd_insert()) return true;
    uint32_t freqs[8];
    uint8_t n = hw_get_sd_mount_freq_list(freqs, 8);
    for (uint8_t i = 0; i < n; i++) {
        if (hw_mount_sd(freqs[i]) && hw_is_sd_insert()) {
            s_mount_failed = false;
            return true;
        }
    }
    s_mount_failed = true;
    return false;
}

void web_plat_sd_unmount(void)
{
    hw_unmount_sd();
}

void web_plat_dev_info(web_dev_info_t *out)
{
    memset(out, 0, sizeof(*out));
    monitor_params_t m = {};
    hw_get_monitor_params(m);
    out->percent = m.battery_percent;
    out->charging = m.charging;
    out->mv = m.battery_voltage;
    wifi_mode_t mode = WiFi.getMode();
    if (WiFi.isConnected()) {
        snprintf(out->mode, sizeof(out->mode), "sta");
        snprintf(out->ssid, sizeof(out->ssid), "%s", WiFi.SSID().c_str());
        snprintf(out->ip, sizeof(out->ip), "%s", WiFi.localIP().toString().c_str());
        out->rssi = WiFi.RSSI();
    } else if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        snprintf(out->mode, sizeof(out->mode), "ap");
        snprintf(out->ssid, sizeof(out->ssid), "%s", WiFi.softAPSSID().c_str());
        snprintf(out->ip, sizeof(out->ip), "%s", WiFi.softAPIP().toString().c_str());
    } else {
        snprintf(out->mode, sizeof(out->mode), "off");
    }
    out->heap_free = ESP.getFreeHeap();
    out->psram_free = ESP.getFreePsram();
    out->flash_total = FFat.totalBytes();
    out->flash_free = out->flash_total - FFat.usedBytes();
}

const char *web_plat_fw_version(void)
{
    static char v[48];
    if (!v[0]) {
        const char *hash = hw_get_firmware_hash_string();
        snprintf(v, sizeof(v), "%s+%.8s", LILYGOLIB_VERSION_STR, hash ? hash : "");
    }
    return v;
}

void web_plat_reboot(void)
{
    delay(200);
    ESP.restart();
}

void web_plat_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.println(line);
    web_log_add(line);
}

/* ---------------------------------------------------------------- the watch's own access point */

#define WEB_AP_AFTER_MS     20000       /* no known network joined for this long: start the AP */

static bool s_ap_on = false;
static uint32_t s_sta_lost_since = 0;
static char s_ap_ssid[33];
static char s_ap_pass[24];

/* "T-Watch-AB12" from the MAC, and a password made once and kept on the flash. */
static void ap_credentials(void)
{
    if (s_ap_ssid[0]) return;
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "T-Watch-%02X%02X", mac[4], mac[5]);
    FILE *f = fopen("/ffat/.web/ap.txt", "rb");
    if (f) {
        size_t n = fread(s_ap_pass, 1, sizeof(s_ap_pass) - 1, f);
        s_ap_pass[n] = 0;
        fclose(f);
        s_ap_pass[strcspn(s_ap_pass, "\r\n")] = 0;
    }
    if (strlen(s_ap_pass) < 8) {
        static const char A[] = "abcdefghjkmnpqrstuvwxyz23456789";   /* no 0/o, 1/l/i */
        uint8_t r[10];
        esp_fill_random(r, sizeof(r));
        for (int i = 0; i < 10; i++) s_ap_pass[i] = A[r[i] % (sizeof(A) - 1)];
        s_ap_pass[10] = 0;
        mkdir("/ffat/.web", 0775);
        f = fopen("/ffat/.web/ap.txt", "wb");
        if (f) {
            fputs(s_ap_pass, f);
            fclose(f);
        }
    }
}

void web_plat_net_tick(bool server_running)
{
    if (!server_running) return;
    uint32_t now = millis();
    if (WiFi.isConnected()) {
        s_sta_lost_since = 0;
        /* Back on a known network: drop the access point once nobody uses it. */
        if (s_ap_on && WiFi.softAPgetStationNum() == 0) web_plat_net_stop();
        return;
    }
    if (!s_sta_lost_since) s_sta_lost_since = now ? now : 1;
    if (s_ap_on || now - s_sta_lost_since < WEB_AP_AFTER_MS) return;
    ap_credentials();
    WiFi.mode(WIFI_AP_STA);
    if (WiFi.softAP(s_ap_ssid, s_ap_pass)) {
        s_ap_on = true;
        web_plat_log("[web] no Wi-Fi joined: access point %s on %s", s_ap_ssid, WiFi.softAPIP().toString().c_str());
    }
}

void web_plat_net_stop(void)
{
    if (!s_ap_on) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap_on = false;
}

bool web_plat_ap_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (!s_ap_on) return false;
    snprintf(ssid, ssid_len, "%s", s_ap_ssid);
    snprintf(pass, pass_len, "%s", s_ap_pass);
    return true;
}

/* ---------------------------------------------------------------- desktop self-install (watch) */

#ifndef WEB_DESKTOP_URL_BASE
/* Releases of this fork. The ZIP's SHA-256 is pinned in the firmware (web_desktop_pin.h). */
#define WEB_DESKTOP_URL_BASE "https://github.com/p3tr0sz/LilyGoLib/releases/download/desktop-v"
#endif

struct spawn_t {
    void (*fn)(void *);
    void *arg;
};

static void spawn_trampoline(void *p)
{
    spawn_t *s = (spawn_t *)p;
    s->fn(s->arg);
    delete s;
    vTaskDelete(NULL);
}

void web_plat_spawn(void (*fn)(void *), void *arg)
{
    spawn_t *s = new spawn_t { fn, arg };
    if (xTaskCreatePinnedToCore(spawn_trampoline, "web_desktop", 12 * 1024, s, 1, NULL, 0) != pdPASS) {
        delete s;
        fn(arg);                    /* no memory for a task: run here */
    }
}

bool web_plat_inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    /* The ROM's tinfl; flags 0 = raw DEFLATE, no zlib header. */
    size_t n = tinfl_decompress_mem_to_mem(out, out_len, in, in_len, 0);
    return n == out_len;
}

bool web_plat_online(void)
{
    return WiFi.isConnected();
}

std::string web_plat_desktop_url(const char *version, const char *zip_name)
{
    return std::string(WEB_DESKTOP_URL_BASE) + version + "/" + zip_name;
}

int web_plat_download(const char *url, const char *path, std::function<void(long long, long long)> progress)
{
    long long have = 0;
    struct stat st;
    web_plat_io_lock();
    if (stat(path, &st) == 0) have = (long long)st.st_size;
    web_plat_io_unlock();

    WiFiClientSecure secure;
    secure.setInsecure();           /* integrity comes from the pinned SHA-256, checked after */
    WiFiClient plain;
    bool https = !strncmp(url, "https://", 8);
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.setUserAgent("T-Watch-Ultra");
    if (!(https ? http.begin(secure, url) : http.begin(plain, url))) return -1;
    if (have > 0) http.addHeader("Range", String("bytes=") + String((long)have) + "-");
    int code = http.GET();
    if (code == 416 && have > 0) { http.end(); return 0; }
    if (code != 200 && code != 206) { http.end(); return code > 0 ? code : -1; }

    long long len = http.getSize();     /* -1 when unknown */
    long long done = code == 206 ? have : 0, total = len >= 0 ? done + len : -1;
    web_plat_io_lock();
    FILE *f = fopen(path, code == 206 ? "ab" : "wb");
    web_plat_io_unlock();
    if (!f) { http.end(); return -1; }
    WiFiClient *stream = http.getStreamPtr();
    std::unique_ptr<uint8_t[]> buf(new uint8_t[4096]);
    long long got = 0;
    uint32_t last = millis();
    while (http.connected() && (len < 0 || got < len)) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - last > 15000) break;
            delay(5);
            continue;
        }
        int n = stream->readBytes(buf.get(), avail < 4096 ? avail : 4096);
        if (n <= 0) break;
        web_plat_io_lock();
        size_t w = fwrite(buf.get(), 1, (size_t)n, f);
        web_plat_io_unlock();
        if (w != (size_t)n) break;
        got += n;
        done += n;
        last = millis();
        progress(done, total);
    }
    web_plat_io_lock();
    fclose(f);
    web_plat_io_unlock();
    http.end();
    return (len >= 0 && got < len) ? -1 : 0;
}

void web_plat_io_lock(void)
{
    instanceLockTake();
}

void web_plat_io_unlock(void)
{
    instanceLockGive();
}

#endif /* FACTORY_WEB && ARDUINO */
