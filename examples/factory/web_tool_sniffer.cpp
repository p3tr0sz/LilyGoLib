/**
 * @file      web_tool_sniffer.cpp
 * @brief     Wi-Fi sniffer (P9 NucleoOS parity): passive promiscuous capture → .pcap on the SD.
 *            Documentation: docs/web-api.md, "Wi-Fi sniffer".
 *
 *            Passive/observe only — the watch captures packets it can hear without transmitting
 *            anything. The resulting pcap can be opened in Wireshark or any pcap-aware tool.
 *
 *            On the watch this uses esp_wifi_set_promiscuous(true) with a callback; each
 *            802.11 packet is written to a pcap file on the SD card with a radiotap header.
 *            In the simulator fake packets are generated.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#ifdef ARDUINO
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

static bool s_active = false;
static int s_channel = -1;          /* -1 = hop all (1-13) */
static char s_path[256] = "";
static uint32_t s_max_bytes = 0;    /* 0 = no limit */
static uint32_t s_packets = 0;
static uint32_t s_bytes = 0;
static const char *s_error = NULL;

/* For pcap: global header (24 bytes) then each packet has a 16-byte packet header + radiotap + 802.11 */
static const uint8_t PCAP_MAGIC[4] = { 0xd4, 0xc3, 0xb2, 0xa1 };  /* little-endian */
static const uint8_t PCAP_MAGIC_BIG[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };

/* Pcap file handle — abstracted so the simulator can use stdio and the watch uses SD file API. */
static FILE *s_pcap = NULL;

static bool pcap_open(const char *path)
{
#ifdef ARDUINO
    s_pcap = fopen(path, "wb");
#else
    s_pcap = fopen(path, "wb");
#endif
    if (!s_pcap) return false;

    /* Write pcap global header (24 bytes) — link type 127 = IEEE 802.11 with radiotap */
    uint8_t hdr[24];
    memcpy(hdr, PCAP_MAGIC, 4);             /* magic */
    hdr[4] = 2; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;   /* version 2.0 */
    memset(hdr + 8, 0, 4);                  /* thiszone (GMT) */
    memset(hdr + 12, 0, 4);                 /* sigfigs */
    hdr[16] = 0xff; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0; /* snaplen 65535 */
    hdr[20] = 127; hdr[21] = 0; hdr[22] = 0; hdr[23] = 0;  /* link type = IEEE 802.11 with radiotap */
    fwrite(hdr, 1, 24, s_pcap);
    fflush(s_pcap);
    return true;
}

static void pcap_write_packet(const uint8_t *frame, size_t frame_len, int channel, int rssi, uint32_t ts_sec, uint32_t ts_usec)
{
    if (!s_pcap || (s_max_bytes > 0 && s_bytes >= s_max_bytes)) return;

    /* Build radiotap header: version(1) + pad(1) + len(2) + present(4) = 8 bytes
     * Present flags: bit 0 = TSFT, bit 2 = channel, bit 4 = RSSI (dBm), bit 5 = noise + signal
     * We'll keep it simple: just channel (7) + antenna signal (RSSI) */
    uint8_t radiotap[12];  /* 8 hdr + 4 payload */
    radiotap[0] = 0;                       /* version 0 */
    radiotap[1] = 0;                       /* pad */
    radiotap[2] = 12; radiotap[3] = 0;     /* header length = 12 */
    radiotap[4] = 0x04; radiotap[5] = 0x00; radiotap[6] = 0x00; radiotap[7] = 0x80; /* present: channel + db_antenna_signal */
    radiotap[8] = channel & 0xFF;          /* frequency (low byte, simplified) */
    radiotap[9] = 0;                       /* padding */
    radiotap[10] = (uint8_t)rssi;          /* antenna signal (dBm, unsigned) */
    radiotap[11] = 0;                      /* noise (0 = unknown) */

    size_t total = 12 + frame_len;         /* radiotap + 802.11 frame */

    /* Packet header (16 bytes) */
    uint8_t pkt_hdr[16];
    memcpy(pkt_hdr, &ts_sec, 4);
    memcpy(pkt_hdr + 4, &ts_usec, 4);
    memcpy(pkt_hdr + 8, &total, 4);       /* saved length */
    memcpy(pkt_hdr + 12, &total, 4);      /* original length */

    fwrite(pkt_hdr, 1, 16, s_pcap);
    fwrite(radiotap, 1, 12, s_pcap);
    fwrite(frame, 1, frame_len, s_pcap);
    fflush(s_pcap);

    s_packets++;
    s_bytes += 16 + 12 + frame_len;
}

/* Channel hopping: call every ~100ms when active to cycle through channels */
static int s_hop_next = 1;

static void hop_channel(void)
{
    if (s_channel > 0) return;             /* fixed channel */
#ifdef ARDUINO
    esp_wifi_set_channel(s_hop_next, WIFI_SECOND_CHAN_NONE);
#endif
    s_hop_next = (s_hop_next % 13) + 1;
}

#ifdef ARDUINO
/* Promiscuous callback — runs in the Wi-Fi task context. We buffer packets to a small queue
 * and drain in the UI thread to avoid doing SD writes inside the ESP callback. */
#define SNIFFER_BUF_MAX 32

static struct {
    uint8_t data[1536];
    size_t len;
    int channel;
    int rssi;
    uint32_t ts_sec, ts_usec;
} s_pkt_buf[SNIFFER_BUF_MAX];
static int s_pkt_buf_w = 0, s_pkt_buf_r = 0;
static std::mutex s_buf_mx;

static void promisc_cb(uint8_t *buf, wifi_promiscuous_pkt_t *pkt)
{
    if (!s_active) return;

    std::lock_guard<std::mutex> lk(s_buf_mx);
    int w = (s_pkt_buf_w + 1) % SNIFFER_BUF_MAX;
    if (w == s_pkt_buf_r) return;  /* full */
    int slot = s_pkt_buf_w;
    size_t n = pkt->payload_length > 1500 ? 1500 : pkt->payload_length;
    memcpy(s_pkt_buf[slot].data, pkt->payload, n);
    s_pkt_buf[slot].len = n;
    s_pkt_buf[slot].channel = s_hop_next > 0 ? s_hop_next : 1;
    s_pkt_buf[slot].rssi = pkt->rssi;
    uint32_t now = (uint32_t)(millis() / 1000);
    s_pkt_buf[slot].ts_sec = now;
    s_pkt_buf[slot].ts_usec = (millis() % 1000) * 1000;
    s_pkt_buf_w = w;
}
#endif

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Wi-Fi Sniffer", NULL };

static void get_status(web_req_t *r)
{
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false") +
                    ",\"channel\":" + (s_channel > 0 ? tools_int(s_channel) : "null") +
                    ",\"packets\":" + tools_int(s_packets) +
                    ",\"bytes\":" + tools_int(s_bytes);
    if (s_error) {
        j += ",\"error\":";
        web_json_str(j, s_error);
    }
    if (s_path[0]) {
        j += ",\"file\":";
        web_json_str(j, s_path);
    }
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_capture(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, path_str;
    double ch = 0, max = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_num(body, "channel", &ch);
    web_json_get_num(body, "max_bytes", &max);
    if (!web_json_get_str(body, "path", path_str)) {
        /* default: /sd/data/sniffer/capture-YYYYMMDD-HHMMSS.pcap */
        long long epoch = tools_epoch();
        char buf[64];
        struct tm *g = gmtime((time_t *)&epoch);
        if (g) snprintf(buf, sizeof(buf), "/sd/data/sniffer/capture-%04d%02d%02d-%02d%02d%02d.pcap",
                        g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, g->tm_sec);
        else snprintf(buf, sizeof(buf), "/sd/data/sniffer/capture.pcap");
        path_str = buf;
    }
    if (path_str.size() > 240) { web_resp_error(r, 400, "bad_request", "path too long"); return; }

    if (s_active) { web_resp_error(r, 409, "conflict", "already capturing"); return; }

    int channel = (int)ch;
    uint32_t max_bytes = (uint32_t)max;
    if (channel < -1 || channel == 0 || channel > 13) {
        web_resp_error(r, 400, "bad_request", "channel -1 (all) or 1-13");
        return;
    }

    /* Open the file */
    if (!pcap_open(path_str.c_str())) {
        web_resp_error(r, 500, "io_error", "cannot open file for writing");
        return;
    }

    s_active = true;
    s_channel = channel;
    snprintf(s_path, sizeof(s_path), "%s", path_str.c_str());
    s_max_bytes = max_bytes > 0 ? max_bytes : 0;
    s_packets = 0;
    s_bytes = 24; /* pcap header */
    s_error = NULL;
    s_hop_next = channel > 0 ? channel : 1;

#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    esp_wifi_set_promiscuous(true);
#endif

    web_resp_ok(r);
}

static void stop_capture(web_req_t *r)
{
    if (!s_active) { web_resp_error(r, 409, "conflict", "not capturing"); return; }
    s_active = false;

#ifdef ARDUINO
    esp_wifi_set_promiscuous(false);
#endif

    if (s_pcap) { fclose(s_pcap); s_pcap = NULL; }

    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/sniffer") && strncmp(r->path, "/api/sniffer/", 13)) return false;
    if (tools_is(r, "GET", "/api/sniffer")) get_status(r);
    else if (tools_is(r, "POST", "/api/sniffer/start")) start_capture(r);
    else if (tools_is(r, "POST", "/api/sniffer/stop")) stop_capture(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

/* ---------------------------------------------------------------- tick / second */

static void sniffer_second(uint32_t now_ms)
{
    (void)now_ms;
    if (!s_active) return;

    /* Drain the buffer from the promiscuous callback into the pcap file */
#ifdef ARDUINO
    int r;
    std::lock_guard<std::mutex> lk(s_buf_mx);
    while (s_pkt_buf_r != s_pkt_buf_w) {
        r = s_pkt_buf_r;
        pcap_write_packet(s_pkt_buf[r].data, s_pkt_buf[r].len,
                          s_pkt_buf[r].channel, s_pkt_buf[r].rssi,
                          s_pkt_buf[r].ts_sec, s_pkt_buf[r].ts_usec);
        s_pkt_buf_r = (r + 1) % SNIFFER_BUF_MAX;
    }
    lk.unlock();
#endif

    /* Channel hop */
    if (s_active && s_channel < 0) hop_channel();

    /* Check max_bytes */
    if (s_max_bytes > 0 && s_bytes >= s_max_bytes) {
        s_error = "max_bytes reached";
        stop_capture(NULL);
        /* Publish event */
        web_events_publish("sniffer", "{\"active\":false,\"reason\":\"max_bytes\"}");
    }
}

static void sniffer_tick(uint32_t now_ms)
{
    (void)now_ms;
}

static void sniffer_stop(void)
{
    if (s_active) {
        s_active = false;
#ifdef ARDUINO
        esp_wifi_set_promiscuous(false);
#endif
        if (s_pcap) { fclose(s_pcap); s_pcap = NULL; }
    }
}

static const char *const CAPS[] = { "wifi.sniffer", NULL };
const web_tool_t web_tool_sniffer = { CAPS, handle, sniffer_tick, sniffer_second, sniffer_stop };

#else /* !FACTORY_WEB */

/* Simulator stub — the mock device handles this */
const web_tool_t web_tool_sniffer = { NULL, NULL, NULL, NULL, NULL };

#endif /* FACTORY_WEB */