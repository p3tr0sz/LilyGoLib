/**
 * @file      web_transport_posix.cpp
 * @brief     Simulator transport and platform for the web server: a small HTTP/1.1 + WebSocket
 *            server on localhost (one thread per connection, at most WEB_SIM_MAX_CONN at once,
 *            like the watch's socket limit), and folders standing in for the SD card and flash.
 *
 *            /sd    -> $TWATCH_SIM_SD    or ./sim_sd
 *            /flash -> $TWATCH_SIM_FLASH or ./sim_flash
 *            F3 in the simulator window inserts or removes the card (main.cpp).
 */
#include "web_internal.h"

#if FACTORY_WEB && !defined(ARDUINO)

#include <lvgl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netdb.h>
#include <zlib.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <thread>
#include <mutex>
#include <map>
#include <memory>
#include <atomic>
#include <string>

#define WEB_SIM_MAX_CONN    10
#define WEB_SIM_HDR_MAX     8192

/* ---------------------------------------------------------------- SHA-1 + base64 (WebSocket handshake) */

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    std::unique_ptr<uint8_t[]> msg(new uint8_t[total]());
    memcpy(msg.get(), data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) msg[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = msg.get() + off + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static std::string base64(const uint8_t *d, size_t n)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += i + 1 < n ? T[(v >> 6) & 63] : '=';
        o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

/* ---------------------------------------------------------------- connections */

struct sim_conn_t {
    int fd;
    int id;
    std::mutex wlock;
    bool ws = false;
};

static std::mutex s_conns_lock;
static std::map<int, std::shared_ptr<sim_conn_t>> s_conns;
static std::atomic<int> s_next_id { 1 };
static std::atomic<bool> s_running { false };
static int s_listen_fd = -1;
static std::thread s_accept_thread;

static bool send_all(sim_conn_t *c, const char *d, size_t n)
{
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(c->fd, d, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(c->fd, d, n, 0);
#endif
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        d += w;
        n -= (size_t)w;
    }
    return true;
}

/* Per-request state behind web_req_t.ctx. */
struct sim_req_t {
    sim_conn_t *conn;
    std::string *pending;       /* bytes already read past the headers */
    long long body_left;
    bool head;
    bool chunked;
    bool begun;
    bool failed;
    bool keep_alive;
};

static int req_read(web_req_t *r, char *buf, size_t n)
{
    sim_req_t *s = (sim_req_t *)r->ctx;
    if (s->body_left <= 0) return 0;
    if (n > (size_t)s->body_left) n = (size_t)s->body_left;
    if (!s->pending->empty()) {
        size_t k = n < s->pending->size() ? n : s->pending->size();
        memcpy(buf, s->pending->data(), k);
        s->pending->erase(0, k);
        s->body_left -= (long long)k;
        return (int)k;
    }
    for (;;) {
        ssize_t k = recv(s->conn->fd, buf, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        s->body_left -= k;
        return (int)k;
    }
}

static const char *reason(int status)
{
    switch (status) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

static bool req_begin(web_req_t *r, int status, const char *type, const char *headers, long long length)
{
    sim_req_t *s = (sim_req_t *)r->ctx;
    if (s->begun) return false;
    s->begun = true;
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    if (type) h += std::string("Content-Type: ") + type + "\r\n";
    if (headers) h += headers;
    if (status == 304 || status == 204) {
        s->chunked = false;
    } else if (length >= 0) {
        h += "Content-Length: " + std::to_string(length) + "\r\n";
        s->chunked = false;
    } else {
        h += "Transfer-Encoding: chunked\r\n";
        s->chunked = true;
    }
    h += s->keep_alive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (!send_all(s->conn, h.data(), h.size())) s->failed = true;
    return !s->failed;
}

static bool req_write(web_req_t *r, const char *d, size_t n)
{
    sim_req_t *s = (sim_req_t *)r->ctx;
    if (s->failed) return false;
    if (s->head || n == 0) return true;
    if (s->chunked) {
        char hdr[24];
        int k = snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
        if (!send_all(s->conn, hdr, (size_t)k) || !send_all(s->conn, d, n) || !send_all(s->conn, "\r\n", 2)) s->failed = true;
    } else if (!send_all(s->conn, d, n)) {
        s->failed = true;
    }
    return !s->failed;
}

static void req_end(web_req_t *r)
{
    sim_req_t *s = (sim_req_t *)r->ctx;
    if (!s->begun) req_begin(r, 500, "text/plain", NULL, 0);
    if (s->chunked && !s->failed && !s->head) {
        if (!send_all(s->conn, "0\r\n\r\n", 5)) s->failed = true;
    }
}

static void copy_header(char *dst, size_t n, const std::string &v)
{
    snprintf(dst, n, "%s", v.c_str());
}

static void percent_decode(char *dst, size_t n, const std::string &src)
{
    size_t o = 0;
    for (size_t i = 0; i < src.size() && o + 1 < n; i++) {
        if (src[i] == '%' && i + 2 < src.size() && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            dst[o++] = (char)strtol(src.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
}

/* ---------------------------------------------------------------- WebSocket */

static void ws_loop(std::shared_ptr<sim_conn_t> c, std::string &pending)
{
    std::string buf = pending;
    for (;;) {
        while (buf.size() >= 2) {
            uint8_t b0 = (uint8_t)buf[0], b1 = (uint8_t)buf[1];
            size_t hl = 2;
            uint64_t len = b1 & 0x7F;
            if (len == 126) { if (buf.size() < 4) break; len = (uint8_t)buf[2] << 8 | (uint8_t)buf[3]; hl = 4; }
            else if (len == 127) { if (buf.size() < 10) break; len = 0; for (int i = 0; i < 8; i++) len = len << 8 | (uint8_t)buf[2 + i]; hl = 10; }
            bool masked = b1 & 0x80;
            size_t need = hl + (masked ? 4 : 0) + (size_t)len;
            if (buf.size() < need) break;
            std::string payload = buf.substr(hl + (masked ? 4 : 0), (size_t)len);
            if (masked) {
                for (size_t i = 0; i < payload.size(); i++) payload[i] ^= buf[hl + (i & 3)];
            }
            buf.erase(0, need);
            int op = b0 & 0x0F;
            if (op == 0x8) return;                       /* close */
            if (op == 0x9) {                             /* ping -> pong */
                std::string f;
                f += (char)0x8A;
                f += (char)payload.size();
                f += payload;
                std::lock_guard<std::mutex> lk(c->wlock);
                send_all(c.get(), f.data(), f.size());
            }
            /* Text from the browser is ignored in API v1. */
        }
        char tmp[1024];
        ssize_t k = recv(c->fd, tmp, sizeof(tmp), 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return;
        buf.append(tmp, (size_t)k);
    }
}

bool web_transport_ws_send(int conn, const char *text, size_t len)
{
    std::shared_ptr<sim_conn_t> c;
    {
        std::lock_guard<std::mutex> lk(s_conns_lock);
        auto it = s_conns.find(conn);
        if (it == s_conns.end() || !it->second->ws) return false;
        c = it->second;
    }
    std::string f;
    f += (char)0x81;
    if (len < 126) {
        f += (char)len;
    } else if (len < 65536) {
        f += (char)126;
        f += (char)(len >> 8);
        f += (char)(len & 0xFF);
    } else {
        f += (char)127;
        for (int i = 7; i >= 0; i--) f += (char)(((uint64_t)len >> (8 * i)) & 0xFF);
    }
    f.append(text, len);
    std::lock_guard<std::mutex> lk(c->wlock);
    return send_all(c.get(), f.data(), f.size());
}

void web_transport_ws_close(int conn)
{
    std::lock_guard<std::mutex> lk(s_conns_lock);
    auto it = s_conns.find(conn);
    if (it != s_conns.end()) {
        std::lock_guard<std::mutex> wl(it->second->wlock);
        const char close_frame[2] = { (char)0x88, 0 };
        send_all(it->second.get(), close_frame, 2);
        shutdown(it->second->fd, SHUT_RDWR);
    }
}

/* ---------------------------------------------------------------- HTTP connection */

static void conn_thread(std::shared_ptr<sim_conn_t> c)
{
    std::string buf;
    for (;;) {
        size_t hdr_end;
        while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > WEB_SIM_HDR_MAX) goto done;
            char tmp[2048];
            ssize_t k = recv(c->fd, tmp, sizeof(tmp), 0);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) goto done;
            buf.append(tmp, (size_t)k);
        }
        {
            std::string head = buf.substr(0, hdr_end);
            std::string pending = buf.substr(hdr_end + 4);
            buf.clear();

            web_req_t r = {};
            sim_req_t s = {};
            s.conn = c.get();
            s.pending = &pending;
            r.content_length = -1;

            size_t le = head.find("\r\n");
            std::string line = head.substr(0, le);
            size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
            if (sp1 == std::string::npos || sp2 == sp1) goto done;
            std::string method = line.substr(0, sp1);
            std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            snprintf(r.method, sizeof(r.method), "%s", method.c_str());
            size_t q = target.find('?');
            percent_decode(r.path, sizeof(r.path), target.substr(0, q));
            if (q != std::string::npos) snprintf(r.query, sizeof(r.query), "%s", target.substr(q + 1).c_str());

            std::string upgrade, ws_key, connection;
            s.keep_alive = true;
            size_t pos = le == std::string::npos ? head.size() : le + 2;
            while (pos < head.size()) {
                size_t e = head.find("\r\n", pos);
                if (e == std::string::npos) e = head.size();
                std::string h = head.substr(pos, e - pos);
                pos = e + 2;
                size_t colon = h.find(':');
                if (colon == std::string::npos) continue;
                std::string k = h.substr(0, colon), v = h.substr(colon + 1);
                while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                if (!strcasecmp(k.c_str(), "cookie")) copy_header(r.cookie, sizeof(r.cookie), v);
                else if (!strcasecmp(k.c_str(), "range")) copy_header(r.range, sizeof(r.range), v);
                else if (!strcasecmp(k.c_str(), "accept-encoding")) copy_header(r.accept_encoding, sizeof(r.accept_encoding), v);
                else if (!strcasecmp(k.c_str(), "if-none-match")) copy_header(r.if_none_match, sizeof(r.if_none_match), v);
                else if (!strcasecmp(k.c_str(), "user-agent")) copy_header(r.user_agent, sizeof(r.user_agent), v);
                else if (!strcasecmp(k.c_str(), "content-length")) r.content_length = atoll(v.c_str());
                else if (!strcasecmp(k.c_str(), "upgrade")) upgrade = v;
                else if (!strcasecmp(k.c_str(), "sec-websocket-key")) ws_key = v;
                else if (!strcasecmp(k.c_str(), "connection")) connection = v;
            }
            if (strcasestr(connection.c_str(), "close")) s.keep_alive = false;

            if (!strcmp(r.path, "/api/events") && !strcasecmp(upgrade.c_str(), "websocket") && !ws_key.empty()) {
                std::string k = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
                uint8_t dig[20];
                sha1((const uint8_t *)k.data(), k.size(), dig);
                std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: " + base64(dig, 20) + "\r\n\r\n";
                {
                    std::lock_guard<std::mutex> lk(c->wlock);
                    if (!send_all(c.get(), resp.data(), resp.size())) goto done;
                }
                c->ws = true;
                if (!web_api_ws_open(c->id, r.cookie)) {
                    web_transport_ws_close(c->id);
                    goto done;
                }
                ws_loop(c, pending);
                web_api_ws_closed(c->id);
                goto done;
            }

            s.body_left = r.content_length > 0 ? r.content_length : 0;
            s.head = !strcmp(r.method, "HEAD");
            r.ctx = &s;
            r.read = req_read;
            r.begin = req_begin;
            r.write = req_write;
            r.end = req_end;
            web_api_handle(&r);

            /* Skip any body the handler did not read, then serve the next request. */
            char sink[2048];
            while (s.body_left > 0 && req_read(&r, sink, sizeof(sink)) > 0) {}
            if (s.failed || !s.keep_alive) goto done;
            buf = pending;
        }
    }
done:
    {
        std::lock_guard<std::mutex> lk(s_conns_lock);
        s_conns.erase(c->id);
    }
    close(c->fd);
}

static void accept_thread(void)
{
    while (s_running) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int fd = accept(s_listen_fd, (struct sockaddr *)&a, &al);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        int nd = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        auto c = std::make_shared<sim_conn_t>();
        c->fd = fd;
        c->id = s_next_id++;
        {
            std::lock_guard<std::mutex> lk(s_conns_lock);
            if (s_conns.size() >= WEB_SIM_MAX_CONN) {
                /* Same limit as the watch: refuse instead of queueing forever. */
                close(fd);
                continue;
            }
            s_conns[c->id] = c;
        }
        std::thread(conn_thread, c).detach();
    }
}

bool web_transport_start(uint16_t port)
{
    if (s_running) return true;
    signal(SIGPIPE, SIG_IGN);
    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) return false;
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    const char *bind_all = getenv("TWATCH_SIM_WEB_ANY");     /* set to reach the simulator from a phone */
    a.sin_addr.s_addr = bind_all && *bind_all == '1' ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    if (bind(s_listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s_listen_fd, 16) < 0) {
        perror("[web] bind");
        close(s_listen_fd);
        s_listen_fd = -1;
        return false;
    }
    s_running = true;
    s_accept_thread = std::thread(accept_thread);
    s_accept_thread.detach();
    return true;
}

void web_transport_stop(void)
{
    if (!s_running) return;
    s_running = false;
    shutdown(s_listen_fd, SHUT_RDWR);
    close(s_listen_fd);
    s_listen_fd = -1;
    std::lock_guard<std::mutex> lk(s_conns_lock);
    for (auto &kv : s_conns) shutdown(kv.second->fd, SHUT_RDWR);
}

/* ---------------------------------------------------------------- platform (simulator) */

static std::string s_sd_dir, s_flash_dir;
static bool s_sd_present = true;
static bool s_sd_mounted = false;

static const std::string &dir_for(std::string &cache, const char *env, const char *fallback)
{
    if (cache.empty()) {
        const char *e = getenv(env);
        if (e && *e) {
            cache = e;
        } else {
            char cwd[1024];
            cache = std::string(getcwd(cwd, sizeof(cwd)) ? cwd : ".") + "/" + fallback;
        }
        while (cache.size() > 1 && cache.back() == '/') cache.pop_back();
        mkdir(cache.c_str(), 0775);
    }
    return cache;
}

const char *web_plat_root_dir(const char *root)
{
    if (!strcmp(root, "sd")) return dir_for(s_sd_dir, "TWATCH_SIM_SD", "sim_sd").c_str();
    if (!strcmp(root, "flash")) return dir_for(s_flash_dir, "TWATCH_SIM_FLASH", "sim_flash").c_str();
    return NULL;
}

void web_plat_random(uint8_t *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t k = fread(buf, 1, n, f);
        fclose(f);
        if (k == n) return;
    }
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rand();
}

/* The simulator's card slot (F3, /api/_dev/sd). */
void web_sim_set_sd_present(bool present)
{
    s_sd_present = present;
}

void web_sim_toggle_sd(void)
{
    s_sd_present = !s_sd_present;
    web_plat_log("[web] simulator SD card %s", s_sd_present ? "inserted" : "removed");
}

void web_plat_sd_info(web_sd_info_t *out, bool with_space)
{
    memset(out, 0, sizeof(*out));
    out->present = s_sd_present;
    out->mounted = s_sd_mounted;
    if (s_sd_present) out->card = "SDHC";
    if (s_sd_mounted && with_space) {
        struct statvfs v;
        if (statvfs(web_plat_root_dir("sd"), &v) == 0) {
            out->total = (uint64_t)v.f_blocks * v.f_frsize;
            out->free = (uint64_t)v.f_bavail * v.f_frsize;
        }
    }
}

bool web_plat_sd_mount(void)
{
    if (!s_sd_present) return false;
    web_plat_root_dir("sd");
    s_sd_mounted = true;
    return true;
}

void web_plat_sd_unmount(void)
{
    s_sd_mounted = false;
}

void web_plat_dev_info(web_dev_info_t *out)
{
    memset(out, 0, sizeof(*out));
    monitor_params_t m = {};
    hw_get_monitor_params(m);
    out->percent = m.battery_percent > 0 ? m.battery_percent : 76;
    out->charging = m.charging;
    out->mv = m.battery_voltage ? m.battery_voltage : 3968;
    snprintf(out->mode, sizeof(out->mode), "sta");
    snprintf(out->ssid, sizeof(out->ssid), "simulator");
    snprintf(out->ip, sizeof(out->ip), "127.0.0.1");
    out->rssi = -55;
    out->heap_free = 182000;
    out->psram_free = 6100000;
    struct statvfs v;
    if (statvfs(web_plat_root_dir("flash"), &v) == 0) {
        out->flash_total = 8257536;
        uint64_t f = (uint64_t)v.f_bavail * v.f_frsize;
        out->flash_free = f < out->flash_total ? f : out->flash_total;
    }
}

const char *web_plat_fw_version(void)
{
    return "sim";
}

void web_plat_reboot(void)
{
    web_plat_log("[web] reboot requested (ignored in the simulator)");
}

void web_plat_log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
    web_log_add(line);
}

void web_plat_net_tick(bool server_running) { (void)server_running; }
void web_plat_net_stop(void) {}
bool web_plat_ap_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    (void)ssid; (void)ssid_len; (void)pass; (void)pass_len;
    return false;
}

/* ---------------------------------------------------------------- desktop self-install (simulator) */

void web_plat_spawn(void (*fn)(void *), void *arg)
{
    std::thread(fn, arg).detach();
}

bool web_plat_inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    z_stream z = {};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)in_len;
    z.next_out = out;
    z.avail_out = (uInt)out_len;
    int rc = inflate(&z, Z_FINISH);
    bool ok = rc == Z_STREAM_END && z.total_out == out_len;
    inflateEnd(&z);
    return ok;
}

/* The simulator downloads only when pointed at a server (plain HTTP), e.g. the mock watch:
 * TWATCH_SIM_DESKTOP_URL=http://localhost:5620/dl */
bool web_plat_online(void)
{
    const char *u = getenv("TWATCH_SIM_DESKTOP_URL");
    return u && *u;
}

std::string web_plat_desktop_url(const char *version, const char *zip_name)
{
    (void)version;
    const char *u = getenv("TWATCH_SIM_DESKTOP_URL");
    std::string base = u && *u ? u : "http://localhost:5620/dl";
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + zip_name;
}

static long long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : 0;
}

int web_plat_download(const char *url, const char *path, std::function<void(long long, long long)> progress)
{
    std::string u = url;
    for (int hop = 0; hop < 4; hop++) {
        if (u.compare(0, 7, "http://") != 0) {
            web_plat_log("[web] the simulator downloads over plain HTTP only: %s", u.c_str());
            return -1;
        }
        std::string rest = u.substr(7);
        size_t slash = rest.find('/');
        std::string hostport = rest.substr(0, slash), target = slash == std::string::npos ? "/" : rest.substr(slash);
        std::string host = hostport, port = "80";
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }

        struct addrinfo hints = {}, *ai = NULL;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &ai) != 0 || !ai) return -1;
        int fd = -1;
        for (struct addrinfo *a = ai; a; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(ai);
        if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        struct timeval tv = { 15, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        long long have = file_size(path);
        std::string req = "GET " + target + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\nUser-Agent: twatch-sim\r\n";
        if (have > 0) req += "Range: bytes=" + std::to_string(have) + "-\r\n";
        req += "\r\n";
        if (send(fd, req.data(), req.size(), 0) != (ssize_t)req.size()) { close(fd); return -1; }

        std::string head;
        char buf[16384];
        size_t he;
        while ((he = head.find("\r\n\r\n")) == std::string::npos) {
            ssize_t k = recv(fd, buf, sizeof(buf), 0);
            if (k <= 0 || head.size() > 16384) { close(fd); return -1; }
            head.append(buf, (size_t)k);
        }
        std::string body = head.substr(he + 4);
        head.resize(he);
        int status = atoi(head.c_str() + head.find(' ') + 1);
        auto header = [&](const char *name) -> std::string {
            std::string h = "\r\n" + std::string(name) + ":";
            for (size_t i = 0; i + h.size() <= head.size(); i++) {
                if (strncasecmp(head.c_str() + i, h.c_str(), h.size()) == 0) {
                    size_t v = i + h.size(), e = head.find("\r\n", v);
                    std::string val = head.substr(v, e == std::string::npos ? std::string::npos : e - v);
                    while (!val.empty() && val[0] == ' ') val.erase(0, 1);
                    return val;
                }
            }
            return "";
        };
        if (status >= 300 && status < 400 && !header("Location").empty()) {
            close(fd);
            std::string loc = header("Location");
            u = loc[0] == '/' ? "http://" + hostport + loc : loc;
            continue;
        }
        if (status == 416 && have > 0) { close(fd); return 0; }     /* already complete */
        if (status != 200 && status != 206) { close(fd); return status; }

        long long len = header("Content-Length").empty() ? -1 : atoll(header("Content-Length").c_str());
        long long done = 0, total = len;
        FILE *f;
        if (status == 206) {
            std::string cr = header("Content-Range");
            size_t sl = cr.find('/');
            total = sl != std::string::npos ? atoll(cr.c_str() + sl + 1) : have + len;
            done = have;
            f = fopen(path, "ab");
        } else {
            f = fopen(path, "wb");          /* the server ignored the Range: start over */
        }
        if (!f) { close(fd); return -1; }
        long long got = 0;
        auto put = [&](const char *d, size_t n) {
            fwrite(d, 1, n, f);
            got += (long long)n;
            done += (long long)n;
            progress(done, total);
        };
        if (!body.empty()) put(body.data(), body.size());
        while (len < 0 || got < len) {
            ssize_t k = recv(fd, buf, sizeof(buf), 0);
            if (k <= 0) break;
            put(buf, (size_t)k);
        }
        fclose(f);
        close(fd);
        return (len >= 0 && got < len) ? -1 : 0;
    }
    return -1;
}

void web_plat_io_lock(void) {}
void web_plat_io_unlock(void) {}

#endif /* FACTORY_WEB && !ARDUINO */
