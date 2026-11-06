/**
 * @file      web_tool_sniffer.cpp
 * @brief     Passive Wi-Fi capture to a standard radiotap PCAP on the SD card.
 *
 * The Wi-Fi callback only queues frames. File I/O, channel hopping and lifecycle
 * work happen outside the callback. API paths (/sd/...) are resolved through the
 * normal filesystem layer so this works on both the watch and the simulator.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <mutex>
#include <sys/stat.h>
#include <errno.h>

#ifdef ARDUINO
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

#define SNIFFER_QUEUE_LEN 12
#define SNIFFER_FRAME_MAX 1500
#define SNIFFER_RADIO_OWNER "Wi-Fi Sniffer"
#define SNIFFER_AP_LIMIT 24
#define SNIFFER_HANDSHAKE_LIMIT 24

enum sniffer_filter_t { FILTER_ALL, FILTER_MANAGEMENT, FILTER_DATA, FILTER_BEACONS, FILTER_PROBES, FILTER_EAPOL };

static bool s_active = false;
static int s_channel = -1;
static int s_filter = FILTER_ALL;
static char s_path[256] = "";
static uint32_t s_max_bytes = 0;
static uint32_t s_packets = 0;
static uint32_t s_dropped = 0;
static uint32_t s_bytes = 0;
static uint32_t s_seen = 0;
static uint32_t s_mgmt = 0, s_ctrl = 0, s_data = 0;
static uint32_t s_beacons = 0, s_probes = 0, s_deauth = 0, s_eapol = 0, s_handshakes = 0;
static char s_error[128] = "";
static FILE *s_pcap = NULL;
static std::mutex s_file_mx;
static int s_hop_next = 1;
static uint32_t s_last_hop_ms = 0;

struct sniffer_ap_t {
    char bssid[18];
    char ssid[33];
    int channel;
    int rssi;
    uint32_t beacons;
};
static sniffer_ap_t s_aps[SNIFFER_AP_LIMIT];
static int s_ap_count = 0;

/* A complete four-way handshake seen for one AP/station pair, kept as a bitmask. */
struct sniffer_handshake_t {
    char a[18];
    char b[18];
    uint8_t mask;
};
static sniffer_handshake_t s_hs[SNIFFER_HANDSHAKE_LIMIT];
static int s_hs_count = 0;

struct capture_packet_t {
    uint8_t data[SNIFFER_FRAME_MAX];
    size_t len;
    int channel;
    int rssi;
    uint32_t ts_sec, ts_usec;
};
static capture_packet_t s_queue[SNIFFER_QUEUE_LEN];
static int s_queue_w = 0, s_queue_r = 0;
static std::mutex s_queue_mx;

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

static const char *filter_name(void)
{
    switch (s_filter) {
    case FILTER_MANAGEMENT: return "management";
    case FILTER_DATA: return "data";
    case FILTER_BEACONS: return "beacons";
    case FILTER_PROBES: return "probes";
    case FILTER_EAPOL: return "eapol";
    default: return "all";
    }
}

static bool filter_parse(const std::string &text, int *out)
{
    if (text.empty() || text == "all") *out = FILTER_ALL;
    else if (text == "management") *out = FILTER_MANAGEMENT;
    else if (text == "data") *out = FILTER_DATA;
    else if (text == "beacons") *out = FILTER_BEACONS;
    else if (text == "probes") *out = FILTER_PROBES;
    else if (text == "eapol") *out = FILTER_EAPOL;
    else return false;
    return true;
}

static void mac_text(const uint8_t *mac, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static size_t dot11_header_len(const uint8_t *f, size_t len)
{
    if (len < 24) return 0;
    uint16_t fc = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
    uint8_t type = (fc >> 2) & 0x03, subtype = (fc >> 4) & 0x0f;
    size_t n = 24;
    bool to_ds = (fc & 0x0100) != 0, from_ds = (fc & 0x0200) != 0;
    if (type == 2 && to_ds && from_ds) n += 6;
    if (type == 2 && (subtype & 0x08)) n += 2;       /* QoS data */
    if (fc & 0x8000) n += 4;                          /* +HTC */
    return n <= len ? n : 0;
}

static bool is_eapol_frame(const uint8_t *f, size_t len, size_t hdr, uint8_t *key_info, uint8_t *eapol_type)
{
    if (!hdr || len < hdr + 12) return false;
    const uint8_t *llc = f + hdr;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03 || llc[3] != 0x00 ||
        llc[4] != 0x00 || llc[5] != 0x00 || llc[6] != 0x88 || llc[7] != 0x8e) return false;
    const uint8_t *e = llc + 8;
    if (eapol_type) *eapol_type = e[1];
    if (e[1] != 3 || len < hdr + 8 + 7) return false;  /* EAPOL-Key */
    if (key_info) {
        /* Key info is big-endian; the high byte carries ACK/MIC/Secure/Install. */
        *key_info = e[5];
    }
    return true;
}

static int eapol_message(uint8_t key_info_hi)
{
    bool ack = (key_info_hi & 0x80) != 0;
    bool mic = (key_info_hi & 0x01) != 0;      /* bit 8: MIC */
    bool secure = (key_info_hi & 0x02) != 0;   /* bit 9: Secure */
    bool install = (key_info_hi & 0x40) != 0;  /* bit 6: Install */
    if (ack && mic && install) return 3;
    if (ack && mic) return 3;                   /* group-key handshake */
    if (ack) return 1;
    if (mic && secure) return 4;
    if (mic) return 2;
    return 0;
}

static void track_handshake(const uint8_t *f, size_t len, uint8_t key_info_hi)
{
    (void)len;
    int msg = eapol_message(key_info_hi);
    if (!msg || len < 16) return;
    char a[18], b[18];
    mac_text(f + 10, a);                             /* transmitter */
    mac_text(f + 4, b);                              /* receiver */
    if (strcmp(a, b) > 0) { char t[18]; memcpy(t, a, 18); memcpy(a, b, 18); memcpy(b, t, 18); }
    for (int i = 0; i < s_hs_count; i++) {
        if (!strcmp(s_hs[i].a, a) && !strcmp(s_hs[i].b, b)) {
            s_hs[i].mask |= (uint8_t)(1u << msg);
            if ((s_hs[i].mask & 0x1e) == 0x1e) { s_handshakes++; s_hs[i].mask = 0; }
            return;
        }
    }
    if (s_hs_count >= SNIFFER_HANDSHAKE_LIMIT) return;
    sniffer_handshake_t &h = s_hs[s_hs_count++];
    snprintf(h.a, sizeof(h.a), "%s", a);
    snprintf(h.b, sizeof(h.b), "%s", b);
    h.mask = (uint8_t)(1u << msg);
}

static void track_ap(const uint8_t *f, size_t len, int channel, int rssi, bool beacon)
{
    if (len < 36) return;
    char bssid[18];
    mac_text(f + 16, bssid);
    char ssid[33] = "";
    int ds_channel = 0;
    size_t off = 36;
    while (off + 2 <= len) {
        uint8_t tag = f[off], tag_len = f[off + 1];
        if (off + 2 + tag_len > len) break;
        if (tag == 0 && tag_len < sizeof(ssid)) {
            memcpy(ssid, f + off + 2, tag_len);
            ssid[tag_len] = 0;
        } else if (tag == 3 && tag_len == 1) {
            ds_channel = f[off + 2];
        }
        off += 2 + tag_len;
    }

    sniffer_ap_t *ap = NULL;
    for (int i = 0; i < s_ap_count; i++) {
        if (!strcmp(s_aps[i].bssid, bssid)) { ap = &s_aps[i]; break; }
    }
    if (!ap) {
        if (s_ap_count >= SNIFFER_AP_LIMIT) return;
        ap = &s_aps[s_ap_count++];
        memset(ap, 0, sizeof(*ap));
        snprintf(ap->bssid, sizeof(ap->bssid), "%s", bssid);
    }
    if (ssid[0] && !ap->ssid[0]) snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
    if (ds_channel > 0) ap->channel = ds_channel;
    else if (channel > 0) ap->channel = channel;
    ap->rssi = rssi;
    if (beacon) ap->beacons++;
}

static void analyse_frame(const uint8_t *f, size_t len, int channel, int rssi)
{
    if (len < 2) return;
    s_seen++;
    uint16_t fc = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
    uint8_t type = (fc >> 2) & 0x03, subtype = (fc >> 4) & 0x0f;
    if (type == 0) {
        s_mgmt++;
        if (subtype == 8) { s_beacons++; track_ap(f, len, channel, rssi, true); }
        else if (subtype == 4) s_probes++;
        else if (subtype == 5) track_ap(f, len, channel, rssi, false);
        if (subtype == 10 || subtype == 12) s_deauth++;
    } else if (type == 1) {
        s_ctrl++;
    } else if (type == 2) {
        s_data++;
        size_t hdr = dot11_header_len(f, len);
        uint8_t key_info = 0;
        if (is_eapol_frame(f, len, hdr, &key_info, NULL)) {
            s_eapol++;
            track_handshake(f, len, key_info);
        }
    }
}

static bool filter_match(const uint8_t *f, size_t len)
{
    if (s_filter == FILTER_ALL) return true;
    if (len < 2) return false;
    uint16_t fc = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
    uint8_t type = (fc >> 2) & 0x03, subtype = (fc >> 4) & 0x0f;
    if (s_filter == FILTER_MANAGEMENT) return type == 0;
    if (s_filter == FILTER_DATA) return type == 2;
    if (s_filter == FILTER_BEACONS) return type == 0 && subtype == 8;
    if (s_filter == FILTER_PROBES) return type == 0 && subtype == 4;
    if (s_filter == FILTER_EAPOL) return type == 2 && is_eapol_frame(f, len, dot11_header_len(f, len), NULL, NULL);
    return true;
}

static bool make_parent_dirs(const std::string &path)
{
    size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return true;
    std::string dir = path.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i != dir.size() && dir[i] != '/') continue;
        std::string part = dir.substr(0, i);
        if (part.empty()) continue;
        web_plat_io_lock();
        bool ok = mkdir(part.c_str(), 0775) == 0 || errno == EEXIST;
        web_plat_io_unlock();
        if (!ok) return false;
    }
    return true;
}

static bool pcap_open(const std::string &host_path)
{
    if (!make_parent_dirs(host_path)) return false;
    web_plat_io_lock();
    FILE *f = fopen(host_path.c_str(), "wb");
    web_plat_io_unlock();
    if (!f) return false;

    /* Little-endian PCAP 2.4, 65535-byte snaplen, LINKTYPE_IEEE802_11_RADIOTAP. */
    const uint8_t hdr[24] = {
        0xd4, 0xc3, 0xb2, 0xa1, 0x02, 0x00, 0x04, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0,
        0xff, 0xff, 0x00, 0x00, 127, 0, 0, 0
    };
    web_plat_io_lock();
    size_t wrote = fwrite(hdr, 1, sizeof(hdr), f);
    web_plat_io_unlock();
    if (wrote != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    std::lock_guard<std::mutex> lk(s_file_mx);
    s_pcap = f;
    return true;
}

/* 0 = written, 1 = byte limit reached, 2 = I/O error. */
static int pcap_write_packet(const uint8_t *frame, size_t frame_len, int channel, int rssi,
                             uint32_t ts_sec, uint32_t ts_usec)
{
    /* Radiotap present: CHANNEL (bit 3) + DBM_ANTSIGNAL (bit 5).
     * CHANNEL is two-byte frequency + one-byte flags + one-byte padding. */
    uint8_t radiotap[13] = {};
    radiotap[2] = sizeof(radiotap);
    radiotap[4] = 0x28;
    uint16_t freq = channel == 14 ? 2484 : (channel >= 1 && channel <= 13 ? 2412 + 5 * (channel - 1) : 0);
    radiotap[8] = freq & 0xff;
    radiotap[9] = freq >> 8;
    radiotap[12] = (uint8_t)(int8_t)rssi;
    uint32_t captured_len = (uint32_t)(sizeof(radiotap) + frame_len);
    uint32_t record_bytes = 16 + captured_len;

    std::lock_guard<std::mutex> lk(s_file_mx);
    if (!s_pcap) return 2;
    if (s_max_bytes && s_bytes + record_bytes > s_max_bytes) return 1;

    uint8_t ph[16] = {};
    memcpy(ph, &ts_sec, 4);
    memcpy(ph + 4, &ts_usec, 4);
    memcpy(ph + 8, &captured_len, 4);
    memcpy(ph + 12, &captured_len, 4);
    web_plat_io_lock();
    bool ok = fwrite(ph, 1, sizeof(ph), s_pcap) == sizeof(ph) &&
              fwrite(radiotap, 1, sizeof(radiotap), s_pcap) == sizeof(radiotap) &&
              fwrite(frame, 1, frame_len, s_pcap) == frame_len;
    web_plat_io_unlock();
    if (!ok) return 2;
    s_packets++;
    s_bytes += record_bytes;
    return 0;
}

static void close_pcap(void)
{
    std::lock_guard<std::mutex> lk(s_file_mx);
    if (!s_pcap) return;
    web_plat_io_lock();
    fflush(s_pcap);
    fclose(s_pcap);
    web_plat_io_unlock();
    s_pcap = NULL;
}

#ifdef ARDUINO
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)type;
    if (!s_active || !buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    size_t n = pkt->rx_ctrl.sig_len;
    if (!n) return;
    if (n > SNIFFER_FRAME_MAX) n = SNIFFER_FRAME_MAX;

    std::lock_guard<std::mutex> lk(s_queue_mx);
    int next = (s_queue_w + 1) % SNIFFER_QUEUE_LEN;
    if (next == s_queue_r) {
        s_dropped++;
        return;
    }
    capture_packet_t &slot = s_queue[s_queue_w];
    memcpy(slot.data, pkt->payload, n);
    slot.len = n;
    slot.channel = pkt->rx_ctrl.channel;
    slot.rssi = pkt->rx_ctrl.rssi;
    slot.ts_sec = (uint32_t)tools_epoch();
    slot.ts_usec = (uint32_t)((esp_timer_get_time() % 1000000LL));
    s_queue_w = next;
}
#endif

static int capture_frame(const uint8_t *f, size_t len, int channel, int rssi,
                         uint32_t ts_sec, uint32_t ts_usec)
{
    analyse_frame(f, len, channel, rssi);
    if (!filter_match(f, len)) return 0;
    return pcap_write_packet(f, len, channel, rssi, ts_sec, ts_usec);
}

static int drain_queue(void)
{
    int result = 0;
#ifdef ARDUINO
    std::lock_guard<std::mutex> lk(s_queue_mx);
    while (s_queue_r != s_queue_w) {
        capture_packet_t &p = s_queue[s_queue_r];
        result = capture_frame(p.data, p.len, p.channel, p.rssi, p.ts_sec, p.ts_usec);
        s_queue_r = (s_queue_r + 1) % SNIFFER_QUEUE_LEN;
        if (result) break;
    }
#endif
    return result;
}

static void publish_state(const char *reason = NULL)
{
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false");
    j += ",\"filter\":";
    web_json_str(j, filter_name());
    if (reason) {
        j += ",\"reason\":";
        web_json_str(j, reason);
    }
    j += "}";
    web_events_publish("sniffer", j.c_str());
}

static void finish_capture(const char *reason, bool publish)
{
    if (!s_active && !s_pcap) return;
    s_active = false;
#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
#endif
    int rc = drain_queue();
    if (rc == 2 && !s_error[0]) set_error("SD write failed; capture stopped");
    close_pcap();
    tools_wifi_release(SNIFFER_RADIO_OWNER);
    {
        std::lock_guard<std::mutex> lk(s_queue_mx);
        s_queue_r = s_queue_w = 0;
    }
    if (reason && !s_error[0]) {
        if (!strcmp(reason, "max_bytes")) set_error("Capture size limit reached");
        else if (!strcmp(reason, "io_error")) set_error("SD write failed; capture stopped");
    }
    if (publish) publish_state(reason);
}

/* 0 on success; otherwise an HTTP status suitable for the API response. */
static int begin_capture(int channel, uint32_t max_bytes, const char *api_path, int filter)
{
    if (s_active) {
        set_error("Capture already running");
        return 409;
    }
    if (channel < -1 || channel == 0 || channel > 13) {
        set_error("Channel must be -1 (all) or 1-13");
        return 400;
    }
    if (filter < FILTER_ALL || filter > FILTER_EAPOL) {
        set_error("Unknown capture filter");
        return 400;
    }

    char default_path[96];
    if (!api_path || !*api_path) {
        long long epoch = tools_epoch();
        time_t now = (time_t)epoch;
        struct tm *g = gmtime(&now);
        if (g) {
            snprintf(default_path, sizeof(default_path), "/sd/data/sniffer/capture-%04d%02d%02d-%02d%02d%02d.pcap",
                     g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
        } else {
            snprintf(default_path, sizeof(default_path), "/sd/data/sniffer/capture.pcap");
        }
        api_path = default_path;
    }
    size_t path_len = strlen(api_path);
    if (path_len < 6 || path_len >= sizeof(s_path) || strcmp(api_path + path_len - 5, ".pcap")) {
        set_error("Capture path must end in .pcap");
        return 400;
    }

    std::string host_path;
    int path_rc = web_fs_resolve(api_path, host_path);
    if (path_rc) {
        set_error(path_rc == 503 ? "Insert and mount an SD card" : "Capture path must be under /sd or /flash");
        return path_rc;
    }
    set_error("");
    if (!pcap_open(host_path)) {
        set_error("Cannot create capture file on the SD card");
        return 500;
    }

    std::string radio_error;
    if (!tools_wifi_claim(SNIFFER_RADIO_OWNER, radio_error)) {
        close_pcap();
        remove(host_path.c_str());
        set_error(radio_error.c_str());
        return 503;
    }

    s_channel = channel;
    s_filter = filter;
    snprintf(s_path, sizeof(s_path), "%s", api_path);
    s_max_bytes = max_bytes;
    s_packets = s_dropped = 0;
    s_bytes = 24;
    s_seen = s_mgmt = s_ctrl = s_data = 0;
    s_beacons = s_probes = s_deauth = s_eapol = s_handshakes = 0;
    s_ap_count = s_hs_count = 0;
    memset(s_aps, 0, sizeof(s_aps));
    memset(s_hs, 0, sizeof(s_hs));
    s_hop_next = channel > 0 ? channel : 1;
    s_last_hop_ms = 0;
    {
        std::lock_guard<std::mutex> lk(s_queue_mx);
        s_queue_r = s_queue_w = 0;
    }

#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_err_t rc = esp_wifi_set_channel(channel > 0 ? channel : 1, WIFI_SECOND_CHAN_NONE);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous(true);
    if (rc != ESP_OK) {
        finish_capture(NULL, false);
        set_error("Wi-Fi promiscuous mode could not start");
        return 500;
    }
#endif
    s_active = true;
    publish_state();
    return 0;
}

static void respond_begin(web_req_t *r, int rc)
{
    if (!rc) {
        web_resp_ok(r);
        return;
    }
    const char *code = rc == 400 ? "bad_request" : rc == 409 ? "conflict" :
                       rc == 503 && !web_fs_sd_mounted() ? "sd_missing" : rc == 503 ? "busy" : "io_error";
    web_resp_error(r, rc, code, s_error);
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "WiFi Sniffer", "Wi-Fi Sniffer", NULL };

static void get_status(web_req_t *r)
{
    std::string j = std::string("{\"active\":") + (s_active ? "true" : "false") +
        ",\"channel\":" + (s_channel > 0 ? tools_int(s_channel) : "null") +
        ",\"filter\":";
    web_json_str(j, filter_name());
    j += ",\"packets\":" + tools_int(s_packets) +
        ",\"drops\":" + tools_int(s_dropped) +
        ",\"bytes\":" + tools_int(s_bytes) +
        ",\"seen\":" + tools_int(s_seen) +
        ",\"management\":" + tools_int(s_mgmt) +
        ",\"control\":" + tools_int(s_ctrl) +
        ",\"data\":" + tools_int(s_data) +
        ",\"beacons\":" + tools_int(s_beacons) +
        ",\"probes\":" + tools_int(s_probes) +
        ",\"deauth\":" + tools_int(s_deauth) +
        ",\"eapol\":" + tools_int(s_eapol) +
        ",\"handshakes\":" + tools_int(s_handshakes) +
        ",\"aps\":[";
    for (int i = 0; i < s_ap_count; i++) {
        if (i) j += ",";
        j += "{\"bssid\":";
        web_json_str(j, s_aps[i].bssid);
        j += ",\"ssid\":";
        web_json_str(j, s_aps[i].ssid);
        j += ",\"channel\":" + tools_int(s_aps[i].channel) +
            ",\"rssi\":" + tools_int(s_aps[i].rssi) +
            ",\"beacons\":" + tools_int(s_aps[i].beacons) + "}";
    }
    j += "],\"error\":";
    if (s_error[0]) web_json_str(j, s_error); else j += "null";
    j += ",\"file\":";
    if (s_path[0]) web_json_str(j, s_path); else j += "null";
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_capture(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, path, filter_text;
    double channel = -1, max_bytes = 0;
    if (!tools_read_json(r, body)) return;
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    if (web_json_has(body, "max_bytes") && (!web_json_get_num(body, "max_bytes", &max_bytes) ||
        max_bytes < 0 || max_bytes > 4294967295.0)) {
        web_resp_error(r, 400, "bad_request", "max_bytes: 0-4294967295"); return;
    }
    web_json_get_str(body, "path", path);
    web_json_get_str(body, "filter", filter_text);
    int filter = FILTER_ALL;
    if (!filter_parse(filter_text, &filter)) {
        web_resp_error(r, 400, "bad_request", "filter must be all, management, data, beacons, probes or eapol"); return;
    }
    respond_begin(r, begin_capture((int)channel, (uint32_t)max_bytes, path.empty() ? NULL : path.c_str(), filter));
}

static void stop_capture(web_req_t *r)
{
    if (!s_active) {
        web_resp_error(r, 409, "conflict", "not capturing");
        return;
    }
    finish_capture(NULL, true);
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/sniffer")) {
        if (!s_active) { web_resp_error(r, 409, "conflict", "start a capture first"); return true; }
        std::string body, kind = "beacon";
        double count = 1, msg = 0;
        if (!tools_read_json(r, body)) return true;
        web_json_get_num(body, "count", &count);
        web_json_get_str(body, "kind", kind);
        web_json_get_num(body, "msg", &msg);
        if (count < 1) count = 1;
        if (count > 100) count = 100;
        uint8_t frame[96] = {};
        size_t flen = 36;
        frame[0] = 0x80;
        memset(frame + 4, 0xff, 6);
        memset(frame + 10, 0x02, 6);
        memset(frame + 16, 0x02, 6);
        frame[16] = 0x02; frame[17] = 0; frame[18] = 0; frame[19] = 0; frame[20] = 0; frame[21] = 1;
        if (kind == "beacon") {
            flen = 45;
            frame[36] = 0; frame[37] = 7;
            memcpy(frame + 38, "TestNet", 7);
        } else if (kind == "probe") {
            flen = 24; frame[0] = 0x40;
        } else if (kind == "deauth") {
            flen = 26; frame[0] = 0xc0;
        } else if (kind == "management") {
            flen = 24; frame[0] = 0x40;
        } else if (kind == "data") {
            flen = 24; frame[0] = 0x08; frame[1] = 0x01;
        } else if (kind == "eapol") {
            /* Data + LLC/SNAP + EAPOL-Key. msg 1-4 sets the key-info bits. */
            flen = 80; frame[0] = 0x08; frame[1] = 0x01;
            const uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };
            memcpy(frame + 24, llc, sizeof(llc));
            uint8_t *e = frame + 32;
            e[0] = 2; e[1] = 3; e[2] = 0; e[3] = 44; e[4] = 2;
            uint8_t hi = 0;
            int m = (int)msg;
            if (m == 1) hi = 0x80;
            else if (m == 2) hi = 0x01;
            else if (m == 3) hi = 0xc1;   /* ACK + MIC + Install */
            else hi = 0x03;               /* MIC + Secure */
            e[5] = hi;
        }
        for (int i = 0; i < (int)count && s_active; i++) {
            int rc = capture_frame(frame, flen, s_channel > 0 ? s_channel : 1, -50,
                                   (uint32_t)tools_epoch(), 0);
            if (rc) { finish_capture(rc == 1 ? "max_bytes" : "io_error", true); break; }
        }
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/sniffer") && strncmp(r->path, "/api/sniffer/", 13)) return false;
    if (tools_is(r, "GET", "/api/sniffer")) get_status(r);
    else if (tools_is(r, "POST", "/api/sniffer/start")) start_capture(r);
    else if (tools_is(r, "POST", "/api/sniffer/stop")) stop_capture(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void sniffer_tick(uint32_t now_ms)
{
    if (!s_active) return;
    int rc = drain_queue();
    if (rc) {
        finish_capture(rc == 1 ? "max_bytes" : "io_error", true);
        return;
    }
    if (s_channel < 0 && now_ms - s_last_hop_ms >= 250) {
        s_last_hop_ms = now_ms;
#ifdef ARDUINO
        esp_wifi_set_channel(s_hop_next, WIFI_SECOND_CHAN_NONE);
#endif
        s_hop_next = s_hop_next % 13 + 1;
    }
    if (s_max_bytes && s_bytes >= s_max_bytes) finish_capture("max_bytes", true);
}

static void sniffer_stop(void)
{
    finish_capture(NULL, false);
}

/* Native watch UI uses the same lifecycle as the HTTP routes. */
bool web_hw_sniffer_start_filter(int channel, uint32_t max_bytes, const char *path, const char *filter)
{
    int parsed = FILTER_ALL;
    if (!filter_parse(filter ? filter : "", &parsed)) return false;
    return begin_capture(channel, max_bytes, path, parsed) == 0;
}
bool web_hw_sniffer_start(int channel, uint32_t max_bytes, const char *path)
{
    return web_hw_sniffer_start_filter(channel, max_bytes, path, "all");
}
bool web_hw_sniffer_stop(void)
{
    finish_capture(NULL, true);
    return true;
}
void web_hw_sniffer_status(web_hw_sniffer_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->channel = s_channel;
    out->packet_count = s_packets;
    out->dropped = s_dropped;
    out->bytes_written = s_bytes;
    out->seen = s_seen;
    out->beacons = s_beacons;
    out->probes = s_probes;
    out->eapol = s_eapol;
    out->handshakes = s_handshakes;
    out->filter = filter_name();
    snprintf(out->file, sizeof(out->file), "%s", s_path);
    out->error = s_error[0] ? s_error : NULL;
}

static const char *const CAPS[] = { "wifi.sniffer", NULL };
const web_tool_t web_tool_sniffer = { CAPS, handle, sniffer_tick, NULL, sniffer_stop };

#else
const web_tool_t web_tool_sniffer = { NULL, NULL, NULL, NULL, NULL };
#endif
