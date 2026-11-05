/**
 * @file      web_tools_hw2.cpp
 * @brief     Hardware for the P5 tools (web_tools.h, web_hw_* for link, HID, net, OTA): the
 *            watch's hardware on the watch, fakes in the simulator, continuing web_tools_hw.cpp.
 *
 *            Link keeps all SD I/O on the UI thread (web_hw_link_poll): the ESP-NOW receive
 *            callback only copies packets into a small queue, so file writes never run inside the
 *            Wi-Fi task or under its lock. The transfer is a simple stop-and-wait with a CRC.
 *
 *            Untested on a watch until its build links (no second watch, no USB host, no real
 *            flash here); the simulator fakes drive the tests.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <mutex>
#include <vector>
#include <string>

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "usb/USB_Service.h"
#endif

static uint32_t hw2_now_ms(void)
{
    return lv_tick_get();
}

static long long hw2_epoch(void)
{
    struct tm t;
    hw_get_date_time(t);
    t.tm_isdst = -1;
    return (long long)mktime(&t);
}

/* ================================================================ Link: ESP-NOW ================= */
/* Wire format, first byte a type:
 *   'H' hello   : name (<=31 bytes)
 *   'B' begin   : u32 size, name
 *   'D' data    : u32 seq, bytes
 *   'E' end     : u32 crc
 *   'A' ack     : u32 seq  (seq 0xffffffff acks begin/end)
 */
#define LINK_HELLO_MS   2000
#define LINK_CHUNK      200
#define LINK_RETRIES    8
#define LINK_ACK_MS     300
#define LINK_PEER_TTL   12          /* seconds before a silent peer drops */
#define LINK_ACK_ALL    0xFFFFFFFFu

struct link_peer_t { uint8_t mac[6]; char name[32]; int rssi; long long seen; };

static std::mutex s_link_mx;
static bool s_link_on = false;
static std::vector<link_peer_t> s_link_peers;
static char s_link_name[32];
static uint32_t s_link_hello_ms = 0;

/* Transfer (UI thread owns the file; cb only sets s_link_rx_* flags). */
static web_hw_link_transfer_t s_xfer;
static bool s_xfer_active = false;
static FILE *s_xfer_file = NULL;
static uint8_t s_xfer_peer[6];
static uint32_t s_xfer_seq = 0;             /* next data seq to send/expect */
static uint32_t s_xfer_crc = 0;
static uint32_t s_xfer_retries = 0;
static uint32_t s_xfer_sent_ms = 0;
static bool s_xfer_acked = true;            /* last chunk acknowledged */
static std::string s_xfer_srcpath;

static uint32_t crc32_up(uint32_t crc, const uint8_t *d, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static const uint8_t LINK_BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void mac_str(const uint8_t *mac, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool mac_parse(const char *s, uint8_t *mac)
{
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

#ifdef ARDUINO

/* A small queue of received packets, filled in the Wi-Fi task, drained on the UI thread. */
struct link_rx_t { uint8_t mac[6]; int rssi; uint8_t data[250]; int len; };
static std::mutex s_rxq_mx;
static std::vector<link_rx_t> s_rxq;

/* This core's ESP-NOW receive callback: (mac, data, len), without an RSSI. */
static void link_recv_cb(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len <= 0 || len > 250) return;
    link_rx_t p;
    memcpy(p.mac, mac, 6);
    p.rssi = -50;                       /* this callback carries no RSSI */
    memcpy(p.data, data, len);
    p.len = len;
    std::lock_guard<std::mutex> lk(s_rxq_mx);
    if (s_rxq.size() < 24) s_rxq.push_back(p);
}

static bool link_add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;                       /* the current STA channel: do not disturb the browser's link */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer) == ESP_OK;
}

static bool link_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    link_add_peer(mac);
    return esp_now_send(mac, data, len) == ESP_OK;
}

bool web_hw_link_set_enabled(bool on)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (on == s_link_on) return true;
    if (on) {
        if (!WiFi.isConnected()) return false;          /* ESP-NOW rides the STA channel */
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(s_link_name, sizeof(s_link_name), "T-Watch-%02X%02X", mac[4], mac[5]);
        if (esp_now_init() != ESP_OK) return false;
        esp_now_register_recv_cb(link_recv_cb);
        link_add_peer(LINK_BCAST);
        s_link_on = true;
        s_link_hello_ms = 0;
    } else {
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        s_link_peers.clear();
        s_link_on = false;
    }
    return true;
}

#else   /* simulator */

bool web_hw_link_set_enabled(bool on)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    s_link_on = on;
    if (!on) s_link_peers.clear();
    if (s_link_name[0] == 0) snprintf(s_link_name, sizeof(s_link_name), "T-Watch-SIM");
    return true;
}

#endif

bool web_hw_link_enabled(void)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    return s_link_on;
}

void web_hw_link_self(char *mac, size_t mac_len, char *name, size_t name_len)
{
#ifdef ARDUINO
    uint8_t m[6] = {0};
    WiFi.macAddress(m);
    char b[18];
    mac_str(m, b);
    snprintf(mac, mac_len, "%s", b);
#else
    snprintf(mac, mac_len, "02:00:00:00:00:01");
#endif
    std::lock_guard<std::mutex> lk(s_link_mx);
    snprintf(name, name_len, "%s", s_link_name[0] ? s_link_name : "T-Watch");
}

int web_hw_link_peers(web_hw_peer_t *out, int max)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    int n = 0;
    for (auto &p : s_link_peers) {
        if (n >= max) break;
        mac_str(p.mac, out[n].mac);
        snprintf(out[n].name, sizeof(out[n].name), "%s", p.name);
        out[n].rssi = p.rssi;
        out[n].last_seen = p.seen;
        n++;
    }
    return n;
}

static void xfer_fail(const char *err)
{
    if (s_xfer_file) {
        fclose(s_xfer_file);
        s_xfer_file = NULL;
    }
    s_xfer.state = "error";
    s_xfer.error = err;
}

int web_hw_link_send(const char *host_path, const char *name, const char *to_mac)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (!s_link_on) return 500;
    if (s_xfer_active && s_xfer.state && (!strcmp(s_xfer.state, "sending") || !strcmp(s_xfer.state, "receiving")))
        return 409;
    uint8_t mac[6];
    if (!mac_parse(to_mac, mac)) return 404;
    bool bcast = memcmp(mac, LINK_BCAST, 6) == 0;
    if (!bcast) {
        bool known = false;
        for (auto &p : s_link_peers) if (!memcmp(p.mac, mac, 6)) known = true;
        if (!known) return 404;
    }
    web_plat_io_lock();
    FILE *f = fopen(host_path, "rb");
    long size = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    web_plat_io_unlock();
    if (!f) return 503;
    s_xfer_file = f;
    s_xfer_srcpath = host_path;
    memcpy(s_xfer_peer, mac, 6);
    memset(&s_xfer, 0, sizeof(s_xfer));
    s_xfer.active = true;
    s_xfer.tx = true;
    mac_str(mac, s_xfer.peer);
    snprintf(s_xfer.name, sizeof(s_xfer.name), "%s", name);
    s_xfer.size = (uint32_t)size;
    s_xfer.state = "sending";
    s_xfer_active = true;
    s_xfer_seq = 0;
    s_xfer_crc = 0;
    s_xfer_retries = 0;
    s_xfer_acked = false;
    s_xfer_sent_ms = 0;            /* poll() sends the begin packet */
    return 0;
}

void web_hw_link_cancel(void)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (!s_xfer_active) return;
    if (s_xfer_file) {
        fclose(s_xfer_file);
        s_xfer_file = NULL;
    }
    if (!s_xfer.tx && s_xfer_srcpath.size()) remove(s_xfer_srcpath.c_str());   /* drop the .part */
    s_xfer.state = "error";
    s_xfer.error = "cancelled";
}

bool web_hw_link_transfer(web_hw_link_transfer_t *out)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (!s_xfer_active) return false;
    *out = s_xfer;
    return true;
}

#ifdef ARDUINO

/* A free path under /sd/data/link for an incoming <name> (adds " (2)" and so on). */
static std::string link_rx_path(const char *name)
{
    const char *sd = web_plat_root_dir("sd");
    std::string dir = std::string(sd ? sd : "/sd") + "/data/link";
    for (size_t i = 1; i < dir.size(); i++) if (dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0775);
    mkdir(dir.c_str(), 0775);
    std::string base = name, ext;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) {
        ext = base.substr(dot);
        base = base.substr(0, dot);
    }
    for (int i = 1; i < 1000; i++) {
        std::string p = dir + "/" + base + (i == 1 ? "" : " (" + std::to_string(i) + ")") + ext;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) return p;
    }
    return dir + "/" + name;
}

static void link_handle_packet(const link_rx_t &p)
{
    /* hellos keep the peer list fresh */
    if (p.data[0] == 'H') {
        char nm[32] = "";
        int n = p.len - 1;
        if (n > 31) n = 31;
        memcpy(nm, p.data + 1, n > 0 ? n : 0);
        bool found = false;
        for (auto &e : s_link_peers) {
            if (!memcmp(e.mac, p.mac, 6)) {
                e.rssi = p.rssi;
                e.seen = hw2_epoch();
                snprintf(e.name, sizeof(e.name), "%s", nm);
                found = true;
            }
        }
        if (!found) {
            link_peer_t e = {};
            memcpy(e.mac, p.mac, 6);
            snprintf(e.name, sizeof(e.name), "%s", nm);
            e.rssi = p.rssi;
            e.seen = hw2_epoch();
            s_link_peers.push_back(e);
        }
        return;
    }
    if (p.data[0] == 'A') {                         /* ack for our tx */
        if (s_xfer_active && s_xfer.tx) {
            s_xfer_acked = true;
        }
        return;
    }
    if (p.data[0] == 'B' && p.len > 5) {            /* begin: someone sends us a file */
        if (s_xfer_active && s_xfer.state && !strcmp(s_xfer.state, "receiving")) return;
        uint32_t size;
        memcpy(&size, p.data + 1, 4);
        char nm[64] = "";
        int n = p.len - 5;
        if (n > 63) n = 63;
        memcpy(nm, p.data + 5, n > 0 ? n : 0);
        std::string path = link_rx_path(nm);
        web_plat_io_lock();
        s_xfer_file = fopen(path.c_str(), "wb");
        web_plat_io_unlock();
        if (!s_xfer_file) return;
        s_xfer_srcpath = path;
        memcpy(s_xfer_peer, p.mac, 6);
        memset(&s_xfer, 0, sizeof(s_xfer));
        s_xfer.active = true;
        s_xfer.tx = false;
        mac_str(p.mac, s_xfer.peer);
        snprintf(s_xfer.name, sizeof(s_xfer.name), "%s", nm);
        s_xfer.size = size;
        s_xfer.state = "receiving";
        s_xfer_active = true;
        s_xfer_seq = 0;
        uint8_t ack[5] = { 'A' };
        uint32_t all = LINK_ACK_ALL;
        memcpy(ack + 1, &all, 4);
        link_send(p.mac, ack, 5);
        return;
    }
    if (p.data[0] == 'D' && p.len > 5) {            /* data chunk */
        if (!s_xfer_active || s_xfer.tx || memcmp(s_xfer_peer, p.mac, 6)) return;
        uint32_t seq;
        memcpy(&seq, p.data + 1, 4);
        if (seq == s_xfer_seq && s_xfer_file) {
            web_plat_io_lock();
            fwrite(p.data + 5, 1, p.len - 5, s_xfer_file);
            web_plat_io_unlock();
            s_xfer.done += p.len - 5;
            s_xfer_seq++;
        }
        uint8_t ack[5] = { 'A' };                   /* ack the seq we now expect - 1 */
        uint32_t a = s_xfer_seq ? s_xfer_seq - 1 : 0;
        memcpy(ack + 1, &a, 4);
        link_send(p.mac, ack, 5);
        return;
    }
    if (p.data[0] == 'E') {                         /* end */
        if (!s_xfer_active || s_xfer.tx) return;
        if (s_xfer_file) {
            fclose(s_xfer_file);
            s_xfer_file = NULL;
        }
        s_xfer.state = "done";
        uint8_t ack[5] = { 'A' };
        uint32_t all = LINK_ACK_ALL;
        memcpy(ack + 1, &all, 4);
        link_send(p.mac, ack, 5);
    }
}

void web_hw_link_poll(void)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (!s_link_on) return;

    /* drain received packets */
    std::vector<link_rx_t> q;
    {
        std::lock_guard<std::mutex> rl(s_rxq_mx);
        q.swap(s_rxq);
    }
    for (auto &p : q) link_handle_packet(p);

    /* prune silent peers */
    long long now = hw2_epoch();
    for (size_t i = 0; i < s_link_peers.size();) {
        if (now - s_link_peers[i].seen > LINK_PEER_TTL) s_link_peers.erase(s_link_peers.begin() + i);
        else i++;
    }

    /* announce ourselves */
    if (lv_tick_diff(hw2_now_ms(), s_link_hello_ms) >= LINK_HELLO_MS) {
        s_link_hello_ms = hw2_now_ms();
        uint8_t hello[32] = { 'H' };
        int n = snprintf((char *)hello + 1, sizeof(hello) - 1, "%s", s_link_name);
        link_send(LINK_BCAST, hello, 1 + (n > 0 ? n : 0));
    }

    /* drive an outgoing transfer, one step per poll */
    if (!s_xfer_active || !s_xfer.tx || !s_xfer.state || strcmp(s_xfer.state, "sending")) return;
    if (!s_xfer_acked) {
        if (lv_tick_diff(hw2_now_ms(), s_xfer_sent_ms) < LINK_ACK_MS) return;   /* wait for the ack */
        if (++s_xfer_retries > LINK_RETRIES) {
            xfer_fail("no answer from the peer");
            return;
        }
    } else {
        s_xfer_retries = 0;
    }
    /* (re)send the current step */
    if (s_xfer.done == 0 && s_xfer_seq == 0 && !s_xfer_acked && s_xfer_retries == 0 && s_xfer_sent_ms == 0) {
        /* begin */
        uint8_t pkt[6 + 64] = { 'B' };
        memcpy(pkt + 1, &s_xfer.size, 4);
        int n = snprintf((char *)pkt + 5, 64, "%s", s_xfer.name);
        link_send(s_xfer_peer, pkt, 5 + (n > 0 ? n : 0));
        s_xfer_sent_ms = hw2_now_ms();
        return;
    }
    if (s_xfer_acked && s_xfer.done < s_xfer.size) {
        uint8_t pkt[5 + LINK_CHUNK] = { 'D' };
        memcpy(pkt + 1, &s_xfer_seq, 4);
        web_plat_io_lock();
        size_t got = s_xfer_file ? fread(pkt + 5, 1, LINK_CHUNK, s_xfer_file) : 0;
        web_plat_io_unlock();
        s_xfer_crc = crc32_up(s_xfer_crc, pkt + 5, got);
        link_send(s_xfer_peer, pkt, 5 + got);
        s_xfer.done += got;
        s_xfer_seq++;
        s_xfer_acked = false;
        s_xfer_sent_ms = hw2_now_ms();
        return;
    }
    if (s_xfer_acked && s_xfer.done >= s_xfer.size) {
        uint8_t pkt[5] = { 'E' };
        memcpy(pkt + 1, &s_xfer_crc, 4);
        link_send(s_xfer_peer, pkt, 5);
        if (s_xfer_file) {
            fclose(s_xfer_file);
            s_xfer_file = NULL;
        }
        s_xfer.state = "done";
    } else if (!s_xfer_acked && s_xfer_sent_ms && s_xfer.done >= LINK_CHUNK) {
        /* resend the last data chunk: step back one seq and rewind the file */
        web_plat_io_lock();
        if (s_xfer_file && fseek(s_xfer_file, (long)(s_xfer.done - (s_xfer.done % LINK_CHUNK ? s_xfer.done % LINK_CHUNK : LINK_CHUNK)), SEEK_SET) == 0) {}
        web_plat_io_unlock();
    }
}

#else   /* simulator: a fake transfer that just advances, and _dev hooks for a peer and an rx */

static uint32_t s_sim_step_ms = 0;

void web_hw_link_poll(void)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    if (!s_xfer_active || !s_xfer.state) return;
    if (strcmp(s_xfer.state, "sending") && strcmp(s_xfer.state, "receiving")) return;
    if (lv_tick_diff(hw2_now_ms(), s_sim_step_ms) < 150) return;
    s_sim_step_ms = hw2_now_ms();
    uint32_t step = s_xfer.size / 8 + 1;
    s_xfer.done += step;
    if (s_xfer.done >= s_xfer.size) {
        s_xfer.done = s_xfer.size;
        s_xfer.state = "done";
    }
}

void web_hw_sim_link_peer(const char *name)
{
    std::lock_guard<std::mutex> lk(s_link_mx);
    s_link_on = true;
    link_peer_t e = {};
    e.mac[0] = 0x02;
    e.mac[5] = (uint8_t)(0x10 + s_link_peers.size());
    snprintf(e.name, sizeof(e.name), "%s", name);
    e.rssi = -40 - (int)s_link_peers.size() * 7;
    e.seen = hw2_epoch();
    s_link_peers.push_back(e);
}

void web_hw_sim_link_rx(const char *name, uint32_t size)
{
    const char *sd = web_plat_root_dir("sd");
    std::string dir = std::string(sd ? sd : "sim_sd") + "/data/link";
    for (size_t i = 1; i < dir.size(); i++) if (dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0775);
    mkdir(dir.c_str(), 0775);
    std::string path = dir + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    if (f) {
        for (uint32_t i = 0; i < size; i++) fputc('x', f);
        fclose(f);
    }
    std::lock_guard<std::mutex> lk(s_link_mx);
    s_link_on = true;
    memset(&s_xfer, 0, sizeof(s_xfer));
    s_xfer.active = true;
    s_xfer.tx = false;
    snprintf(s_xfer.peer, sizeof(s_xfer.peer), "02:00:00:00:00:10");
    snprintf(s_xfer.name, sizeof(s_xfer.name), "%s", name);
    s_xfer.size = size;
    s_xfer.done = size;
    s_xfer.state = "done";
    s_xfer_active = true;
}

#endif

/* ================================================================ USB HID keyboard ============== */

#ifdef ARDUINO

static bool s_hid_began = false;

static uint8_t hid_key_of(const char *name)
{
    if (strlen(name) == 1) {
        char c = name[0];
        if (isalpha((unsigned char)c)) return (uint8_t)tolower((unsigned char)c);
        if (isdigit((unsigned char)c)) return (uint8_t)c;
        if (c == ' ') return ' ';
    }
    struct { const char *n; uint8_t k; } M[] = {
        { "ENTER", LILYGO_USB_KEY_ENTER }, { "RETURN", LILYGO_USB_KEY_ENTER }, { "TAB", LILYGO_USB_KEY_TAB },
        { "ESC", LILYGO_USB_KEY_ESC }, { "ESCAPE", LILYGO_USB_KEY_ESC }, { "SPACE", (uint8_t)' ' },
        { "BACKSPACE", LILYGO_USB_KEY_BACKSPACE }, { "DEL", LILYGO_USB_KEY_DELETE }, { "DELETE", LILYGO_USB_KEY_DELETE },
        { "UP", LILYGO_USB_KEY_UP }, { "DOWN", LILYGO_USB_KEY_DOWN }, { "LEFT", LILYGO_USB_KEY_LEFT },
        { "RIGHT", LILYGO_USB_KEY_RIGHT }, { "HOME", LILYGO_USB_KEY_HOME }, { "END", LILYGO_USB_KEY_END },
        { "PAGEUP", LILYGO_USB_KEY_PAGE_UP }, { "PAGEDOWN", LILYGO_USB_KEY_PAGE_DOWN },
        { "INSERT", LILYGO_USB_KEY_INSERT }, { "INS", LILYGO_USB_KEY_INSERT },
    };
    for (auto &m : M) if (!strcasecmp(name, m.n)) return m.k;
    if ((name[0] == 'F' || name[0] == 'f') && name[1]) {
        int f = atoi(name + 1);
        if (f >= 1 && f <= 12) return (uint8_t)(LILYGO_USB_KEY_F1 + (f - 1));
    }
    return LILYGO_USB_KEY_NONE;
}

bool web_hw_hid_available(void)
{
    if (!s_hid_began) {
        lilygo_usb_service_begin();
        s_hid_began = true;
    }
    return lilygo_usb_hid_keyboard_available();
}

bool web_hw_hid_text(const char *s)
{
    web_hw_hid_available();
    return lilygo_usb_hid_keyboard_send_text(s, 5);
}

bool web_hw_hid_key(const char *name, int modifiers)
{
    web_hw_hid_available();
    uint8_t key = hid_key_of(name);
    if (key == LILYGO_USB_KEY_NONE) return false;
    if (modifiers) return lilygo_usb_hid_keyboard_combo((uint8_t)modifiers, key);
    return lilygo_usb_hid_keyboard_tap_key(key);
}

void web_hw_hid_release(void)
{
    lilygo_usb_hid_keyboard_release_all();
}

#else   /* simulator: a fake keyboard, available only when a test turns it on */

static bool s_hid_available = false;

void web_hw_sim_hid_available(bool on) { s_hid_available = on; }
bool web_hw_hid_available(void) { return s_hid_available; }
bool web_hw_hid_text(const char *s) { (void)s; return s_hid_available; }
bool web_hw_hid_key(const char *name, int modifiers) { (void)name; (void)modifiers; return s_hid_available; }
void web_hw_hid_release(void) {}

#endif

/* ================================================================ Wi-Fi tools =================== */

#ifdef ARDUINO

void web_hw_net_info(web_hw_net_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = WiFi.isConnected();
    snprintf(out->mac, sizeof(out->mac), "%s", WiFi.macAddress().c_str());
    if (!out->connected) return;
    snprintf(out->ssid, sizeof(out->ssid), "%s", WiFi.SSID().c_str());
    snprintf(out->ip, sizeof(out->ip), "%s", WiFi.localIP().toString().c_str());
    snprintf(out->gateway, sizeof(out->gateway), "%s", WiFi.gatewayIP().toString().c_str());
    snprintf(out->netmask, sizeof(out->netmask), "%s", WiFi.subnetMask().toString().c_str());
    snprintf(out->dns, sizeof(out->dns), "%s", WiFi.dnsIP().toString().c_str());
    out->rssi = WiFi.RSSI();
    out->channel = WiFi.channel();
}

int web_hw_net_resolve(const char *host, char *ip, size_t ip_len, int *ms)
{
    if (!WiFi.isConnected()) return -1;
    IPAddress addr;
    uint32_t t0 = millis();
    bool ok = WiFi.hostByName(host, addr);
    *ms = (int)(millis() - t0);
    if (ok) snprintf(ip, ip_len, "%s", addr.toString().c_str());
    return ok ? 1 : 0;
}

int web_hw_net_tcp(const char *host, int port, int *ms)
{
    if (!WiFi.isConnected()) return -1;
    WiFiClient c;
    c.setTimeout(2);
    uint32_t t0 = millis();
    bool ok = c.connect(host, (uint16_t)port, 2000);
    *ms = (int)(millis() - t0);
    c.stop();
    return ok ? 1 : 0;
}

#else   /* simulator */

void web_hw_net_info(web_hw_net_t *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = true;
    snprintf(out->ssid, sizeof(out->ssid), "simulator");
    snprintf(out->ip, sizeof(out->ip), "127.0.0.1");
    snprintf(out->gateway, sizeof(out->gateway), "127.0.0.1");
    snprintf(out->netmask, sizeof(out->netmask), "255.255.255.0");
    snprintf(out->dns, sizeof(out->dns), "127.0.0.1");
    snprintf(out->mac, sizeof(out->mac), "02:00:00:00:00:01");
    out->rssi = -55;
    out->channel = 6;
}

int web_hw_net_resolve(const char *host, char *ip, size_t ip_len, int *ms)
{
    *ms = 12;
    if (strchr(host, ' ') || !strchr(host, '.')) return 0;      /* "fails" for names without a dot */
    snprintf(ip, ip_len, "93.184.216.34");
    return 1;
}

int web_hw_net_tcp(const char *host, int port, int *ms)
{
    (void)host;
    *ms = 24;
    return (port == 80 || port == 443 || port == 22) ? 1 : 0;
}

#endif

/* ================================================================ OTA firmware update =========== */

static const char *s_ota_state = "idle";
static const char *s_ota_error = NULL;
static uint32_t s_ota_done = 0, s_ota_total = 0;

#ifdef ARDUINO

void web_hw_ota_status(web_hw_ota_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    out->slot = run ? run->label : "?";
    out->other_slot = next ? next->label : "?";
    out->state = s_ota_state;
    out->error = s_ota_error;
    out->done = s_ota_done;
    out->total = s_ota_total;
    esp_ota_img_states_t st;
    out->can_rollback = next && esp_ota_get_state_partition(next, &st) == ESP_OK && st == ESP_OTA_IMG_VALID;
}

bool web_hw_ota_begin(long long total)
{
    if (!Update.begin(total > 0 ? (size_t)total : UPDATE_SIZE_UNKNOWN)) return false;
    s_ota_state = "writing";
    s_ota_error = NULL;
    s_ota_done = 0;
    s_ota_total = (uint32_t)total;
    return true;
}

bool web_hw_ota_write(const uint8_t *data, size_t len)
{
    if (Update.write((uint8_t *)data, len) != len) {
        s_ota_state = "error";
        s_ota_error = "write failed";
        return false;
    }
    s_ota_done += len;
    return true;
}

int web_hw_ota_end(void)
{
    if (!Update.end(true)) {
        s_ota_state = "error";
        s_ota_error = Update.errorString();
        return -1;
    }
    s_ota_state = "ready";
    return 0;
}

bool web_hw_ota_commit(void)
{
    delay(1000);
    esp_restart();
    return true;
}

bool web_hw_ota_rollback(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    if (!next || esp_ota_get_state_partition(next, &st) != ESP_OK || st != ESP_OTA_IMG_VALID) return false;
    if (esp_ota_set_boot_partition(next) != ESP_OK) return false;
    delay(1000);
    esp_restart();
    return true;
}

#else   /* simulator: validate size and progress, never reboot */

#define SIM_OTA_MAX (4 * 1024 * 1024)

void web_hw_ota_status(web_hw_ota_t *out)
{
    out->slot = "app0";
    out->other_slot = "app1";
    out->state = s_ota_state;
    out->error = s_ota_error;
    out->done = s_ota_done;
    out->total = s_ota_total;
    out->can_rollback = true;
}

bool web_hw_ota_begin(long long total)
{
    if (total <= 0 || total > SIM_OTA_MAX) return false;
    s_ota_state = "writing";
    s_ota_error = NULL;
    s_ota_done = 0;
    s_ota_total = (uint32_t)total;
    return true;
}

bool web_hw_ota_write(const uint8_t *data, size_t len)
{
    (void)data;
    s_ota_done += len;
    return true;
}

int web_hw_ota_end(void)
{
    if (s_ota_done < 5) {               /* too small to be an image */
        s_ota_state = "error";
        s_ota_error = "bad_image";
        return -1;
    }
    s_ota_state = "ready";
    return 0;
}

bool web_hw_ota_commit(void)
{
    web_plat_log("[web] OTA commit (simulator: no reboot)");
    s_ota_state = "idle";
    s_ota_done = s_ota_total = 0;
    return true;
}

bool web_hw_ota_rollback(void)
{
    web_plat_log("[web] OTA rollback (simulator: no reboot)");
    return true;
}

#endif

#endif /* FACTORY_WEB */
