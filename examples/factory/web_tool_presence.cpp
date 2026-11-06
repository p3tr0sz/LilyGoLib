/**
 * @file      web_tool_presence.cpp
 * @brief     Wi-Fi CSI motion/presence sensing and nearby ESP-NOW peer tracking.
 *
 * CSI processing compares the normalized L-LTF subcarrier amplitude profile of
 * frames from the connected AP with a calibrated reference. It reports motion
 * and a short presence hold, not a person count or a location.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <mutex>
#include <atomic>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <fcntl.h>
#include <unistd.h>
#include <lwip/sockets.h>
#include <lwip/inet_chksum.h>
#endif

#define PRESENCE_MAX_PEERS 32
#define PRESENCE_TIMEOUT_S 300
#define PRESENCE_HISTORY_MAX 50

#define CSI_RADIO_OWNER "Presence"
#define CSI_TONES 52
#define CSI_CALIB_FRAMES 32
#define CSI_WINDOW 16
#define CSI_HOLD_MS 10000
#define CSI_QUEUE_LEN 8
#define CSI_RAW_MAX 128

static bool s_active = false;
static long long s_started = 0;
static web_hw_presence_peer_t s_peers[PRESENCE_MAX_PEERS];
static int s_peer_count = 0;
static uint32_t s_peer_total = 0;
static web_hw_presence_history_t s_history[PRESENCE_HISTORY_MAX];
static int s_history_count = 0;
static uint32_t s_poll_last_ms = 0;
static char s_error[128] = "";
static std::mutex s_mx;

/* ---------------------------------------------------------------- CSI detector */

struct csi_detector_t {
    float reference[CSI_TONES];
    float deviations[CSI_WINDOW];
    float threshold;
    float level;
    float noise;
    float score;
    uint32_t frames;
    uint32_t calibration_frames;
    int window_count;
    int window_head;
    int rssi;
    int over_threshold;
    bool reference_ready;
    bool calibrating;
    bool motion;
    bool present;
    uint32_t last_motion_ms;
};

static csi_detector_t s_csi;
static std::atomic<bool> s_csi_active{false};
static long long s_csi_started = 0;
static uint32_t s_csi_samples = 0;
static uint32_t s_csi_dropped = 0;
static uint32_t s_csi_rate = 0;
static uint32_t s_csi_rate_samples = 0;
static uint32_t s_csi_rate_ms = 0;
static int s_csi_channel = 0;
static char s_csi_ssid[33] = "";
static char s_csi_error[160] = "";
static std::mutex s_csi_mx;
#ifndef ARDUINO
static uint32_t s_csi_sim_ms = 0;
#endif

static void csi_set_error(const char *text)
{
    snprintf(s_csi_error, sizeof(s_csi_error), "%s", text ? text : "");
}

static const char *csi_state(void)
{
    if (!s_csi_active) return "stopped";
    if (s_csi.calibrating) return "calibrating";
    if (s_csi.motion) return "motion";
    return s_csi.present ? "present" : "clear";
}

static void publish_csi(const char *event)
{
    std::string j = "{\"event\":";
    web_json_str(j, event);
    j += ",\"state\":";
    web_json_str(j, csi_state());
    j += ",\"present\":" + std::string(s_csi.present ? "true" : "false") +
        ",\"motion\":" + (s_csi.motion ? "true" : "false") +
        ",\"score\":" + tools_num(s_csi.score, 3) + "}";
    web_events_publish("presence.csi", j.c_str());
}

/* s_csi_mx held. Resets learning while preserving session counters/settings. */
static void csi_calibrate_locked(uint32_t now_ms)
{
    float threshold = s_csi.threshold > 0 ? s_csi.threshold : 0.35f;
    uint32_t frames = s_csi.frames;
    memset(&s_csi, 0, sizeof(s_csi));
    s_csi.threshold = threshold;
    s_csi.frames = frames;
    s_csi.rssi = -90;
    s_csi.calibrating = true;
    s_csi.noise = 0.005f;
    s_csi.last_motion_ms = now_ms;
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* s_csi_mx held. The time argument is explicit so simulator frames can test
 * the hold timer without sleeping. */
static void csi_step_locked(uint32_t now_ms)
{
    if (!s_csi_active || s_csi.calibrating) return;
    if (s_csi.present && !s_csi.motion && now_ms - s_csi.last_motion_ms > CSI_HOLD_MS) {
        s_csi.present = false;
        publish_csi("clear");
    }
}

/* s_csi_mx held. */
static void csi_feed_locked(const int8_t *raw, int len, int rssi, bool first_word_invalid, uint32_t now_ms)
{
    if (!s_csi_active || !raw || len < 16) return;
    float amplitude[CSI_TONES];
    int pairs = len / 2;
    if (pairs > 64) pairs = 64;       /* this pass handles the legacy L-LTF */
    int count = 0;
    float sum = 0;
    for (int i = first_word_invalid ? 2 : 0; i < pairs && count < CSI_TONES; i++) {
        int subcarrier = i < 32 ? i : i - 64;
        if (subcarrier == 0 || subcarrier > 26 || subcarrier < -26) continue;
        float im = raw[i * 2], re = raw[i * 2 + 1];
        float a = sqrtf(im * im + re * re);
        amplitude[count++] = a;
        sum += a;
    }
    if (count < 8 || sum < 1.0f) return;

    /* A common AGC gain must not look like movement: normalize profile shape. */
    float inverse_mean = count / sum;
    for (int i = 0; i < count; i++) amplitude[i] *= inverse_mean;

    bool was_calibrating = s_csi.calibrating;
    bool was_motion = s_csi.motion;
    bool was_present = s_csi.present;
    float deviation = 0;
    if (!s_csi.reference_ready) {
        memcpy(s_csi.reference, amplitude, sizeof(float) * count);
        s_csi.reference_ready = true;
    } else {
        float reference_mean = 0;
        for (int i = 0; i < count; i++) reference_mean += s_csi.reference[i];
        reference_mean /= count;
        float cross = 0, current_energy = 0, reference_energy = 0;
        for (int i = 0; i < count; i++) {
            float x = amplitude[i] - 1.0f;
            float y = s_csi.reference[i] - reference_mean;
            cross += x * y;
            current_energy += x * x;
            reference_energy += y * y;
        }
        float correlation = 1.0f;
        if (current_energy > 1e-8f && reference_energy > 1e-8f)
            correlation = cross / sqrtf(current_energy * reference_energy);
        deviation = correlation < 1 ? 1.0f - correlation : 0;
    }

    s_csi.frames++;
    s_csi_samples++;
    s_csi.rssi = rssi;
    if (s_csi.calibrating) {
        float alpha = s_csi.calibration_frames ? 0.10f : 1.0f;
        for (int i = 0; i < count; i++)
            s_csi.reference[i] += (amplitude[i] - s_csi.reference[i]) * alpha;
        s_csi.calibration_frames++;
        s_csi.noise = s_csi.noise * 0.95f + deviation * 0.05f;
        if (s_csi.calibration_frames >= CSI_CALIB_FRAMES) {
            s_csi.calibrating = false;
            s_csi.noise = s_csi.noise < 0.005f ? 0.005f : s_csi.noise;
            s_csi.level = s_csi.score = 0;
            s_csi.window_count = s_csi.window_head = 0;
            memset(s_csi.deviations, 0, sizeof(s_csi.deviations));
        }
    } else {
        s_csi.deviations[s_csi.window_head] = deviation;
        s_csi.window_head = (s_csi.window_head + 1) % CSI_WINDOW;
        if (s_csi.window_count < CSI_WINDOW) s_csi.window_count++;
        float level = 0;
        for (int i = 0; i < s_csi.window_count; i++) level += s_csi.deviations[i];
        s_csi.level = level / s_csi.window_count;
        s_csi.score = clamp01((s_csi.level - s_csi.noise) / (s_csi.noise * 3.0f + 0.02f));

        if (s_csi.score >= s_csi.threshold) {
            if (++s_csi.over_threshold >= 3) s_csi.motion = true;
        } else {
            s_csi.over_threshold = 0;
            s_csi.motion = false;
        }
        if (s_csi.motion) {
            s_csi.present = true;
            s_csi.last_motion_ms = now_ms;
        } else {
            /* Slow adaptation only in quiet periods; never learn away motion. */
            for (int i = 0; i < count; i++)
                s_csi.reference[i] += (amplitude[i] - s_csi.reference[i]) * 0.005f;
            s_csi.noise = s_csi.noise * 0.999f + s_csi.level * 0.001f;
        }
    }
    csi_step_locked(now_ms);
    if (was_calibrating != s_csi.calibrating) publish_csi("calibrated");
    if (was_motion != s_csi.motion) publish_csi(s_csi.motion ? "motion" : "still");
    if (was_present != s_csi.present) publish_csi(s_csi.present ? "present" : "clear");
}

#ifdef ARDUINO
struct csi_queued_t {
    int8_t data[CSI_RAW_MAX];
    uint16_t len;
    int8_t rssi;
    bool first_word_invalid;
};
static csi_queued_t s_csi_queue[CSI_QUEUE_LEN];
static int s_csi_qread = 0, s_csi_qwrite = 0;
static uint32_t s_csi_qdrops = 0;
static std::mutex s_csi_qmx;
static uint8_t s_csi_bssid[6] = {};
static bool s_csi_ps_saved = false;
static std::atomic<bool> s_csi_radio{false};
static wifi_ps_type_t s_csi_previous_ps = WIFI_PS_MIN_MODEM;
static int s_csi_socket = -1;
static uint32_t s_csi_gateway = 0;
static uint16_t s_csi_ping_sequence = 0;
static uint32_t s_csi_last_ping_ms = 0;
static uint32_t s_csi_link_check_ms = 0;

static void csi_rx_cb(void *, wifi_csi_info_t *info)
{
    if (!s_csi_active || !s_csi_radio || !info || !info->buf || !info->len) return;
    if (memcmp(info->mac, s_csi_bssid, sizeof(s_csi_bssid))) return;
    std::lock_guard<std::mutex> lk(s_csi_qmx);
    int next = (s_csi_qwrite + 1) % CSI_QUEUE_LEN;
    if (next == s_csi_qread) {
        s_csi_qdrops++;
        return;
    }
    csi_queued_t &sample = s_csi_queue[s_csi_qwrite];
    sample.len = info->len > CSI_RAW_MAX ? CSI_RAW_MAX : info->len;
    memcpy(sample.data, info->buf, sample.len);
    sample.rssi = info->rx_ctrl.rssi;
    sample.first_word_invalid = info->first_word_invalid;
    s_csi_qwrite = next;
}

static void csi_promiscuous_cb(void *, wifi_promiscuous_pkt_type_t) {}

static void csi_pump_close(void)
{
    if (s_csi_socket >= 0) close(s_csi_socket);
    s_csi_socket = -1;
    s_csi_gateway = 0;
}

static void csi_pump_open(void)
{
    csi_pump_close();
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || !ip.gw.addr) return;
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) return;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    s_csi_socket = fd;
    s_csi_gateway = ip.gw.addr;
    s_csi_last_ping_ms = 0;
}

static void csi_pump_tick(uint32_t now_ms)
{
    if (s_csi_socket < 0 || now_ms - s_csi_last_ping_ms < 50) return;
    s_csi_last_ping_ms = now_ms;
    struct {
        uint8_t type, code;
        uint16_t checksum, id, sequence;
        uint8_t payload[24];
    } request = {};
    request.type = 8;
    request.id = htons(0x5457);
    request.sequence = htons(++s_csi_ping_sequence);
    request.checksum = inet_chksum(&request, sizeof(request));
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = s_csi_gateway;
    sendto(s_csi_socket, &request, sizeof(request), 0, (sockaddr *)&destination, sizeof(destination));
    uint8_t replies[96];
    while (recv(s_csi_socket, replies, sizeof(replies), 0) > 0) {}
}

static void csi_radio_disable(void)
{
    if (!s_csi_radio) return;
    s_csi_radio = false;               /* stop new queue writes before tearing down the driver */
    esp_wifi_set_csi(false);
    esp_wifi_set_csi_rx_cb(NULL, NULL);
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    if (s_csi_ps_saved) esp_wifi_set_ps(s_csi_previous_ps);
    s_csi_ps_saved = false;
    csi_pump_close();
}

static bool csi_radio_enable(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        csi_set_error("Connect to a 2.4 GHz Wi-Fi network before starting CSI sensing");
        return false;
    }
    memcpy(s_csi_bssid, ap.bssid, sizeof(s_csi_bssid));
    snprintf(s_csi_ssid, sizeof(s_csi_ssid), "%s", (const char *)ap.ssid);
    s_csi_channel = ap.primary;
    if (!s_csi_ps_saved && esp_wifi_get_ps(&s_csi_previous_ps) == ESP_OK) s_csi_ps_saved = true;
    s_csi_radio = true;                /* also makes partial-initialization cleanup run */
    esp_wifi_set_ps(WIFI_PS_NONE);       /* modem sleep bursts make CSI rates unusable */
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_err_t rc = esp_wifi_set_promiscuous_filter(&filter);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous_rx_cb(csi_promiscuous_cb);
    if (rc == ESP_OK) rc = esp_wifi_set_promiscuous(true);
    wifi_csi_config_t config = {};
    config.lltf_en = true;
    config.htltf_en = true;
    config.stbc_htltf2_en = true;
    config.ltf_merge_en = true;
    config.channel_filter_en = true;
    if (rc == ESP_OK) rc = esp_wifi_set_csi_config(&config);
    if (rc == ESP_OK) rc = esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL);
    if (rc == ESP_OK) rc = esp_wifi_set_csi(true);
    if (rc != ESP_OK) {
        csi_radio_disable();
        csi_set_error("The Wi-Fi driver could not enable CSI on this connection");
        return false;
    }
    csi_pump_open();
    csi_set_error("");
    return true;
}

static void csi_link_check(uint32_t now_ms)
{
    if (!s_csi_active || now_ms - s_csi_link_check_ms < 1000) return;
    s_csi_link_check_ms = now_ms;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        if (s_csi_radio) csi_radio_disable();
        csi_set_error("Wi-Fi disconnected; sensing resumes after reconnection");
        return;
    }
    bool changed = memcmp(ap.bssid, s_csi_bssid, sizeof(s_csi_bssid)) != 0;
    if (!s_csi_radio || changed) {
        csi_radio_disable();
        std::lock_guard<std::mutex> lk(s_csi_mx);
        csi_calibrate_locked(now_ms);
        csi_radio_enable();
    }
}
#endif

static int csi_begin(float threshold)
{
    if (threshold < 0.05f || threshold > 0.95f) return 400;
    {
        std::lock_guard<std::mutex> lk(s_csi_mx);
        if (s_csi_active) return 0;
    }
    std::string radio_error;
    if (!tools_wifi_claim(CSI_RADIO_OWNER, radio_error)) {
        std::lock_guard<std::mutex> lk(s_csi_mx);
        csi_set_error(radio_error.c_str());
        return 503;
    }
    {
        std::lock_guard<std::mutex> lk(s_csi_mx);
        s_csi_active = true;
        s_csi_started = tools_epoch();
        s_csi_samples = s_csi_dropped = s_csi_rate = s_csi_rate_samples = 0;
        s_csi_rate_ms = tools_now_ms();
        s_csi.last_motion_ms = 0;
        s_csi_channel = 0;
        s_csi_ssid[0] = 0;
        csi_set_error("");
        s_csi.threshold = threshold;
        csi_calibrate_locked(tools_now_ms());
#ifndef ARDUINO
        s_csi_sim_ms = tools_now_ms();
        snprintf(s_csi_ssid, sizeof(s_csi_ssid), "%s", "Simulator-WiFi");
        s_csi_channel = 1;
#endif
    }
#ifdef ARDUINO
    {
        std::lock_guard<std::mutex> lk(s_csi_qmx);
        s_csi_qread = s_csi_qwrite = 0;
        s_csi_qdrops = 0;
    }
    s_csi_link_check_ms = 0;
    if (!csi_radio_enable()) {
        s_csi_active = false;
        tools_wifi_release(CSI_RADIO_OWNER);
        return 503;
    }
#endif
    return 0;
}

static void csi_finish(void)
{
#ifdef ARDUINO
    csi_radio_disable();
#endif
    tools_wifi_release(CSI_RADIO_OWNER);
    std::lock_guard<std::mutex> lk(s_csi_mx);
    s_csi_active = false;
    s_csi.calibrating = s_csi.motion = s_csi.present = false;
    s_csi.score = s_csi.level = 0;
    s_csi_rate = 0;
}

/* ---------------------------------------------------------------- nearby peers */

static int find_peer(const char *mac)
{
    for (int i = 0; i < s_peer_count; i++) if (!strcasecmp(s_peers[i].mac, mac)) return i;
    return -1;
}

static void publish_change(const char *event, const web_hw_presence_peer_t &p)
{
    std::string j = "{\"event\":";
    web_json_str(j, event);
    j += ",\"mac\":";
    web_json_str(j, p.mac);
    j += ",\"name\":";
    web_json_str(j, p.name);
    j += ",\"rssi\":" + tools_int(p.rssi) + "}";
    web_events_publish("presence", j.c_str());
}

static void add_history(const web_hw_presence_peer_t &p, const char *event)
{
    if (s_history_count >= PRESENCE_HISTORY_MAX) {
        memmove(s_history, s_history + 1, sizeof(s_history[0]) * (PRESENCE_HISTORY_MAX - 1));
        s_history_count--;
    }
    auto &h = s_history[s_history_count++];
    h.ts = tools_epoch();
    snprintf(h.mac, sizeof(h.mac), "%s", p.mac);
    snprintf(h.name, sizeof(h.name), "%s", p.name);
    snprintf(h.event, sizeof(h.event), "%s", event);
    publish_change(event, p);
}

/* s_mx held. */
static void peer_seen(const char *mac, const char *name, int rssi)
{
    long long now = tools_epoch();
    int idx = find_peer(mac);
    if (idx < 0) {
        if (s_peer_count >= PRESENCE_MAX_PEERS) return;
        idx = s_peer_count++;
        memset(&s_peers[idx], 0, sizeof(s_peers[idx]));
        snprintf(s_peers[idx].mac, sizeof(s_peers[idx].mac), "%s", mac);
        s_peers[idx].first_seen = now;
        s_peers[idx].present = true;
        s_peer_total++;
        auto &p = s_peers[idx];
        snprintf(p.name, sizeof(p.name), "%s", name ? name : "");
        p.rssi = rssi;
        p.last_seen = now;
        add_history(p, "appeared");
        return;
    }
    auto &p = s_peers[idx];
    bool returned = !p.present;
    p.present = true;
    p.last_seen = now;
    p.rssi = rssi;
    if (name && *name) snprintf(p.name, sizeof(p.name), "%s", name);
    if (returned) add_history(p, "appeared");
}

/* s_mx held. */
static void check_timeouts(void)
{
    long long now = tools_epoch();
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].present && now - s_peers[i].last_seen > PRESENCE_TIMEOUT_S) {
            s_peers[i].present = false;
            add_history(s_peers[i], "disappeared");
        }
    }
}

static void poll_peers(void)
{
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) {
        std::lock_guard<std::mutex> lk(s_mx);
        snprintf(s_error, sizeof(s_error), "%s", "ESP-NOW could not start");
        return;
    }
    web_hw_peer_t found[16];
    int n = web_hw_link_peers(found, 16);
    std::lock_guard<std::mutex> lk(s_mx);
    if (n < 0) {
        snprintf(s_error, sizeof(s_error), "%s", "Could not read nearby peers");
        return;
    }
    s_error[0] = 0;
    for (int i = 0; i < n; i++) peer_seen(found[i].mac, found[i].name, found[i].rssi);
    check_timeouts();
}

/* ---------------------------------------------------------------- API */

static const char *const APPS[] = { "Presence", "Nearby Peers", NULL };

static std::string csi_json_text(void)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    std::string j = std::string("{\"active\":") + (s_csi_active ? "true" : "false") +
        ",\"state\":";
    web_json_str(j, csi_state());
    j += ",\"calibrating\":" + std::string(s_csi_active && s_csi.calibrating ? "true" : "false") +
        ",\"present\":" + (s_csi_active && s_csi.present ? "true" : "false") +
        ",\"motion\":" + (s_csi_active && s_csi.motion ? "true" : "false") +
        ",\"score\":" + tools_num(s_csi_active ? s_csi.score : 0, 3) +
        ",\"level\":" + tools_num(s_csi.level, 5) +
        ",\"noise\":" + tools_num(s_csi.noise, 5) +
        ",\"threshold\":" + tools_num(s_csi.threshold > 0 ? s_csi.threshold : 0.35f, 2) +
        ",\"samples\":" + tools_int(s_csi_samples) +
        ",\"rate\":" + tools_int(s_csi_rate) +
        ",\"drops\":" + tools_int(s_csi_dropped) +
        ",\"rssi\":" + tools_int(s_csi.rssi) +
        ",\"channel\":" + (s_csi_channel ? tools_int(s_csi_channel) : "null") +
        ",\"ssid\":";
    if (s_csi_ssid[0]) web_json_str(j, s_csi_ssid); else j += "null";
    j += ",\"started\":" + tools_int(s_csi_started) + ",\"last_motion_ms\":" + tools_int(s_csi.last_motion_ms) + ",\"error\":";
    if (s_csi_error[0]) web_json_str(j, s_csi_error); else j += "null";
    return j + "}";
}

static std::string status_json_text(void)
{
    std::string j;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        j = std::string("{\"active\":") + (s_active ? "true" : "false") +
            ",\"started\":" + tools_int(s_started) +
            ",\"peers_ever\":" + tools_int(s_peer_total) + ",\"error\":";
        if (s_error[0]) web_json_str(j, s_error); else j += "null";
        j += ",\"peers\":[";
        for (int i = 0; i < s_peer_count; i++) {
            if (i) j += ",";
            j += "{\"mac\":";
            web_json_str(j, s_peers[i].mac);
            j += ",\"name\":";
            web_json_str(j, s_peers[i].name);
            j += ",\"rssi\":" + tools_int(s_peers[i].rssi) +
                ",\"first_seen\":" + tools_int(s_peers[i].first_seen) +
                ",\"last_seen\":" + tools_int(s_peers[i].last_seen) +
                ",\"present\":" + (s_peers[i].present ? "true" : "false") + "}";
        }
        j += "],\"history\":[";
        for (int i = 0; i < s_history_count; i++) {
            if (i) j += ",";
            j += "{\"ts\":" + tools_int(s_history[i].ts) + ",\"mac\":";
            web_json_str(j, s_history[i].mac);
            j += ",\"name\":";
            web_json_str(j, s_history[i].name);
            j += ",\"event\":";
            web_json_str(j, s_history[i].event);
            j += "}";
        }
        j += "],\"csi\":";
    }
    return j + csi_json_text() + "}";
}

static void get_status(web_req_t *r) { web_resp_json(r, 200, status_json_text()); }

static void start_monitor(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) {
        web_resp_error(r, 503, "internal", "ESP-NOW could not start"); return;
    }
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_active) {
            s_active = true;
            s_started = tools_epoch();
            s_peer_count = s_history_count = 0;
            s_peer_total = 0;
            s_error[0] = 0;
            s_poll_last_ms = 0;
        }
    }
    web_resp_ok(r);
}

static void stop_monitor(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
    web_resp_ok(r);
}

static void start_csi(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body;
    double threshold = 0.35;
    if (!tools_read_json(r, body)) return;
    if (web_json_has(body, "threshold") && !web_json_get_num(body, "threshold", &threshold)) {
        web_resp_error(r, 400, "bad_request", "threshold must be a number"); return;
    }
    int rc = csi_begin((float)threshold);
    if (rc == 400) web_resp_error(r, 400, "bad_request", "threshold: 0.05-0.95");
    else if (rc) {
        std::lock_guard<std::mutex> lk(s_csi_mx);
        const char *code = strstr(s_csi_error, "Stop ") ? "busy" : strstr(s_csi_error, "Connect to") ? "wifi_required" : "csi_unavailable";
        web_resp_error(r, rc, code, s_csi_error);
    } else web_resp_ok(r);
}

static void stop_csi(web_req_t *r)
{
    csi_finish();
    web_resp_ok(r);
}

static void calibrate_csi(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    if (!s_csi_active) { web_resp_error(r, 409, "conflict", "start CSI sensing first"); return; }
    csi_calibrate_locked(tools_now_ms());
    csi_set_error("");
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/presence-csi")) {
        std::string body, pattern = "stable";
        double count = 1, advance_ms = 0, rssi = -50;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "pattern", pattern);
        web_json_get_num(body, "count", &count);
        web_json_get_num(body, "advance_ms", &advance_ms);
        web_json_get_num(body, "rssi", &rssi);
        if (!s_csi_active) { web_resp_error(r, 409, "conflict", "start CSI sensing first"); return true; }
        if (pattern != "stable" && pattern != "motion") { web_resp_error(r, 400, "bad_request", "pattern must be stable or motion"); return true; }
        if (count < 0 || count > 500 || advance_ms < 0 || advance_ms > 600000) {
            web_resp_error(r, 400, "bad_request", "count: 0-500; advance_ms: 0-600000"); return true;
        }
        std::lock_guard<std::mutex> lk(s_csi_mx);
        for (int frame = 0; frame < (int)count; frame++) {
            int8_t raw[CSI_RAW_MAX] = {};
            for (int tone = 0; tone < 26; tone++) {
                /* Fill both positive and negative frequency bins. */
                int indices[2] = { tone + 1, 32 + 6 + tone };
                for (int side = 0; side < 2; side++) {
                    int i = indices[side];
                    float shape = 12 + (tone % 7) * 2 + (side ? 3 : 0);
                    if (pattern == "motion") {
                        float phase = (frame % 20) * 0.314159f;
                        shape *= 0.55f + 0.9f * (0.5f + 0.5f * sinf(phase + tone * 0.7f + side));
                    }
                    float gain = 1.0f + (frame % 5) * 0.15f;  /* AGC changes are not motion */
                    raw[i * 2] = (int8_t)(shape * gain * 0.6f);
                    raw[i * 2 + 1] = (int8_t)(shape * gain * 0.8f);
                }
            }
            s_csi_sim_ms += 20;
            csi_feed_locked(raw, sizeof(raw), (int)rssi, false, s_csi_sim_ms);
        }
        s_csi_sim_ms += (uint32_t)advance_ms;
        csi_step_locked(s_csi_sim_ms);
        web_resp_ok(r);
        return true;
    }
    if (tools_is(r, "POST", "/api/_dev/presence")) {
        std::string body, mac, name;
        double rssi = -50;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "mac", mac);
        web_json_get_str(body, "name", name);
        web_json_get_num(body, "rssi", &rssi);
        if (!s_active) { web_resp_error(r, 409, "conflict", "start discovery first"); return true; }
        if (mac.empty()) mac = "02:00:00:00:00:01";
        if (name.empty()) name = "Test Watch";
        {
            std::lock_guard<std::mutex> lk(s_mx);
            peer_seen(mac.c_str(), name.c_str(), (int)rssi);
        }
        web_resp_ok(r);
        return true;
    }
#endif
    if (strcmp(r->path, "/api/presence") && strncmp(r->path, "/api/presence/", 14)) return false;
    if (tools_is(r, "GET", "/api/presence")) get_status(r);
    else if (tools_is(r, "POST", "/api/presence/start")) start_monitor(r);
    else if (tools_is(r, "POST", "/api/presence/stop")) stop_monitor(r);
    else if (tools_is(r, "POST", "/api/presence/csi/start")) start_csi(r);
    else if (tools_is(r, "POST", "/api/presence/csi/stop")) stop_csi(r);
    else if (tools_is(r, "POST", "/api/presence/csi/calibrate")) calibrate_csi(r);
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void presence_tick(uint32_t now_ms)
{
#ifdef ARDUINO
    csi_link_check(now_ms);
    if (s_csi_active) {
        csi_pump_tick(now_ms);
        std::lock_guard<std::mutex> state_lock(s_csi_mx);
        for (int n = 0; n < CSI_QUEUE_LEN; n++) {
            csi_queued_t sample;
            {
                std::lock_guard<std::mutex> lk(s_csi_qmx);
                if (s_csi_qread == s_csi_qwrite) {
                    s_csi_dropped += s_csi_qdrops;
                    s_csi_qdrops = 0;
                    break;
                }
                sample = s_csi_queue[s_csi_qread];
                s_csi_qread = (s_csi_qread + 1) % CSI_QUEUE_LEN;
            }
            csi_feed_locked(sample.data, sample.len, sample.rssi, sample.first_word_invalid, now_ms);
        }
    }
#endif
    if (!s_csi_active) return;
    std::lock_guard<std::mutex> lk(s_csi_mx);
#ifndef ARDUINO
    csi_step_locked(s_csi_sim_ms);
#else
    csi_step_locked(now_ms);
#endif
    if (now_ms - s_csi_rate_ms >= 1000) {
        s_csi_rate = s_csi_samples - s_csi_rate_samples;
        s_csi_rate_samples = s_csi_samples;
        s_csi_rate_ms = now_ms;
    }
}

static void presence_second(uint32_t now_ms)
{
    if (s_active && now_ms - s_poll_last_ms >= 5000) {
        s_poll_last_ms = now_ms;
        poll_peers();
    }
}

static void presence_stop(void)
{
    csi_finish();
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
}

bool web_hw_presence_start(void)
{
    if (!web_hw_link_enabled() && !web_hw_link_set_enabled(true)) return false;
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_active) {
        s_active = true;
        s_started = tools_epoch();
        s_peer_count = s_history_count = 0;
        s_peer_total = 0;
        s_error[0] = 0;
        s_poll_last_ms = 0;
    }
    return true;
}
bool web_hw_presence_stop(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_active = false;
    return true;
}
void web_hw_presence_status(web_hw_presence_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_mx);
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->started_epoch = s_started;
    out->peers_ever = (int)s_peer_total;
    out->peer_count = s_peer_count;
    out->history_count = s_history_count;
    out->error = s_error[0] ? s_error : NULL;
    memcpy(out->peers, s_peers, sizeof(s_peers));
    memcpy(out->history, s_history, sizeof(s_history));
}

bool web_hw_presence_csi_start(float threshold)
{
    return csi_begin(threshold) == 0;
}
bool web_hw_presence_csi_stop(void)
{
    csi_finish();
    return true;
}
bool web_hw_presence_csi_calibrate(void)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    if (!s_csi_active) return false;
    csi_calibrate_locked(tools_now_ms());
    return true;
}
void web_hw_presence_csi_status(web_hw_csi_presence_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_csi_mx);
    memset(out, 0, sizeof(*out));
    out->active = s_csi_active;
    out->calibrating = s_csi_active && s_csi.calibrating;
    out->present = s_csi_active && s_csi.present;
    out->motion = s_csi_active && s_csi.motion;
    out->score = s_csi.score;
    out->threshold = s_csi.threshold;
    out->samples = s_csi_samples;
    out->rate = s_csi_rate;
    out->drops = s_csi_dropped;
    out->rssi = s_csi.rssi;
    out->channel = s_csi_channel;
    out->started_epoch = s_csi_started;
    out->error = s_csi_error[0] ? s_csi_error : NULL;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_csi_ssid);
}

static const char *const CAPS[] = { "presence.peers", "presence.csi", NULL };
const web_tool_t web_tool_presence = { CAPS, handle, presence_tick, presence_second, presence_stop };

#else
const web_tool_t web_tool_presence = { NULL, NULL, NULL, NULL, NULL };
#endif
