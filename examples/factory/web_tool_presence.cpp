/**
 * @file      web_tool_presence.cpp
 * @brief     Wi-Fi CSI motion/presence, optional Faro link/transmitter, alarm and nearby peers.
 *
 * CSI processing compares normalized L-LTF subcarrier amplitude profiles with a
 * calibrated reference. Router and Faro links have independent detectors. The
 * result is a motion/presence estimate, never a person count or a location.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <string>
#include <mutex>
#include <atomic>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_now.h>
#include <esp_timer.h>
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
#define CSI_FARO_TTL_MS 3000
#define CSI_HISTORY_POINTS 120
#define CSI_HISTORY_BUCKET_MS 500
#define FARO_INTERVAL_US 10000
#define ALARM_GRACE_MS 1500

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

/* ---------------------------------------------------------------- CSI detectors */

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

struct csi_history_sample_t {
    uint32_t t;
    uint8_t router;
    uint8_t faro;
};

static csi_detector_t s_csi, s_faro_zone;
static std::atomic<bool> s_csi_active{false};
static long long s_csi_started = 0;
static uint32_t s_csi_samples = 0, s_faro_samples = 0;
static uint32_t s_csi_dropped = 0;
static uint32_t s_csi_rate = 0, s_faro_rate = 0;
static uint32_t s_csi_rate_samples = 0, s_faro_rate_samples = 0;
static uint32_t s_csi_rate_ms = 0;
static int s_csi_channel = 0;
static char s_csi_ssid[33] = "";
static char s_csi_error[160] = "";
static bool s_faro_discovered = false, s_faro_linked = false;
static uint32_t s_faro_seen_ms = 0;
static char s_faro_mac[18] = "";
static csi_history_sample_t s_csi_history[CSI_HISTORY_POINTS];
static int s_csi_history_count = 0, s_csi_history_head = 0;
static uint32_t s_history_bucket_ms = 0;
static uint8_t s_history_router_max = 0, s_history_faro_max = 0;
static std::mutex s_csi_mx;
#ifndef ARDUINO
static uint32_t s_csi_sim_ms = 0;
#endif

static uint32_t presence_now_ms(void)
{
#ifndef ARDUINO
    return s_csi_sim_ms;
#else
    return tools_now_ms();
#endif
}

static void csi_set_error(const char *text)
{
    snprintf(s_csi_error, sizeof(s_csi_error), "%s", text ? text : "");
}

static bool faro_fresh_locked(uint32_t now_ms)
{
    return s_csi_active && s_faro_discovered && (uint32_t)(now_ms - s_faro_seen_ms) <= CSI_FARO_TTL_MS;
}

static const char *csi_state(void)
{
    if (!s_csi_active) return "stopped";
    if (s_csi.calibrating) return "calibrating";
    if (s_csi.motion) return "motion";
    return s_csi.present ? "present" : "clear";
}

static const char *zone_state_locked(bool faro, uint32_t now_ms)
{
    if (!faro) return csi_state();
    if (!s_faro_discovered) return "waiting";
    if (!faro_fresh_locked(now_ms)) return "absent";
    if (s_faro_zone.calibrating) return "calibrating";
    if (s_faro_zone.motion) return "motion";
    return s_faro_zone.present ? "present" : "clear";
}

static void publish_csi(const char *event, const csi_detector_t &d, const char *zone, uint32_t now_ms)
{
    bool faro = !strcmp(zone, "faro");
    std::string j = "{\"event\":";
    web_json_str(j, event);
    j += ",\"zone\":";
    web_json_str(j, zone);
    j += ",\"state\":";
    web_json_str(j, zone_state_locked(faro, now_ms));
    j += ",\"present\":" + std::string(d.present ? "true" : "false") +
        ",\"motion\":" + (d.motion ? "true" : "false") +
        ",\"score\":" + tools_num(d.score, 3) + "}";
    web_events_publish("presence.csi", j.c_str());
}

/* Reset learning while preserving the frame counter and threshold. */
static void csi_reset_detector(csi_detector_t &d, uint32_t now_ms)
{
    float threshold = d.threshold > 0 ? d.threshold : 0.35f;
    uint32_t frames = d.frames;
    memset(&d, 0, sizeof(d));
    d.threshold = threshold;
    d.frames = frames;
    d.rssi = -90;
    d.calibrating = true;
    d.noise = 0.005f;
    d.last_motion_ms = now_ms;
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static uint8_t score_percent(float score)
{
    int n = (int)(clamp01(score) * 100.0f + 0.5f);
    return (uint8_t)n;
}

/* s_csi_mx held. */
static void csi_step_detector_locked(csi_detector_t &d, bool enabled, const char *zone, uint32_t now_ms)
{
    if (!enabled || d.calibrating) return;
    if (d.present && !d.motion && (uint32_t)(now_ms - d.last_motion_ms) > CSI_HOLD_MS) {
        d.present = false;
        publish_csi("clear", d, zone, now_ms);
    }
}

/* s_csi_mx held. Returns false for an invalid/empty CSI sample. */
static bool csi_feed_detector_locked(csi_detector_t &d, const char *zone, const int8_t *raw, int len,
                                     int rssi, bool first_word_invalid, uint32_t now_ms)
{
    if (!raw || len < 16) return false;
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
    if (count < 8 || sum < 1.0f) return false;

    /* A common AGC gain must not look like movement: normalize profile shape. */
    float inverse_mean = count / sum;
    for (int i = 0; i < count; i++) amplitude[i] *= inverse_mean;

    bool was_calibrating = d.calibrating;
    bool was_motion = d.motion;
    bool was_present = d.present;
    float deviation = 0;
    if (!d.reference_ready) {
        memcpy(d.reference, amplitude, sizeof(float) * count);
        d.reference_ready = true;
    } else {
        float reference_mean = 0;
        for (int i = 0; i < count; i++) reference_mean += d.reference[i];
        reference_mean /= count;
        float cross = 0, current_energy = 0, reference_energy = 0;
        for (int i = 0; i < count; i++) {
            float x = amplitude[i] - 1.0f;
            float y = d.reference[i] - reference_mean;
            cross += x * y;
            current_energy += x * x;
            reference_energy += y * y;
        }
        float correlation = 1.0f;
        if (current_energy > 1e-8f && reference_energy > 1e-8f)
            correlation = cross / sqrtf(current_energy * reference_energy);
        deviation = correlation < 1 ? 1.0f - correlation : 0;
    }

    d.frames++;
    d.rssi = rssi;
    if (d.calibrating) {
        float alpha = d.calibration_frames ? 0.10f : 1.0f;
        for (int i = 0; i < count; i++)
            d.reference[i] += (amplitude[i] - d.reference[i]) * alpha;
        d.calibration_frames++;
        d.noise = d.noise * 0.95f + deviation * 0.05f;
        if (d.calibration_frames >= CSI_CALIB_FRAMES) {
            d.calibrating = false;
            d.noise = d.noise < 0.005f ? 0.005f : d.noise;
            d.level = d.score = 0;
            d.window_count = d.window_head = 0;
            memset(d.deviations, 0, sizeof(d.deviations));
        }
    } else {
        d.deviations[d.window_head] = deviation;
        d.window_head = (d.window_head + 1) % CSI_WINDOW;
        if (d.window_count < CSI_WINDOW) d.window_count++;
        float level = 0;
        for (int i = 0; i < d.window_count; i++) level += d.deviations[i];
        d.level = level / d.window_count;
        d.score = clamp01((d.level - d.noise) / (d.noise * 3.0f + 0.02f));

        if (d.score >= d.threshold) {
            if (++d.over_threshold >= 3) d.motion = true;
        } else {
            d.over_threshold = 0;
            d.motion = false;
        }
        if (d.motion) {
            d.present = true;
            d.last_motion_ms = now_ms;
        } else {
            /* Slow adaptation only in quiet periods; never learn away motion. */
            for (int i = 0; i < count; i++)
                d.reference[i] += (amplitude[i] - d.reference[i]) * 0.005f;
            d.noise = d.noise * 0.999f + d.level * 0.001f;
        }
    }
    csi_step_detector_locked(d, true, zone, now_ms);
    if (was_calibrating != d.calibrating) publish_csi("calibrated", d, zone, now_ms);
    if (was_motion != d.motion) publish_csi(d.motion ? "motion" : "still", d, zone, now_ms);
    if (was_present != d.present) publish_csi(d.present ? "present" : "clear", d, zone, now_ms);
    return true;
}

/* ---------------------------------------------------------------- alarm */

enum presence_alarm_state_t { ALARM_OFF, ALARM_ARMING, ALARM_ARMED, ALARM_TRIGGERED };
static presence_alarm_state_t s_alarm_state = ALARM_OFF;
static uint32_t s_alarm_state_ms = 0, s_alarm_triggers = 0, s_alarm_last_beep_s = 0xffffffff;
static int s_alarm_exit_s = 10, s_alarm_duration_s = 30;
static bool s_alarm_loaded = false;
static char s_alarm_zone[8] = "";
static char s_alarm_error[128] = "";

static std::string alarm_cfg_path(void)
{
    const char *dir = web_plat_root_dir("flash");
    return std::string(dir ? dir : ".") + "/.web/presence-alarm.txt";
}

static void alarm_cfg_load(void)
{
    if (s_alarm_loaded) return;
    s_alarm_loaded = true;
    FILE *f = fopen(alarm_cfg_path().c_str(), "rb");
    if (!f) return;
    int exit_s, duration_s;
    if (fscanf(f, "%d %d", &exit_s, &duration_s) == 2 &&
        exit_s >= 5 && exit_s <= 120 && duration_s >= 10 && duration_s <= 300) {
        s_alarm_exit_s = exit_s;
        s_alarm_duration_s = duration_s;
    }
    fclose(f);
}

static void alarm_cfg_save(void)
{
    std::string p = alarm_cfg_path(), tmp = p + ".tmp";
    mkdir(p.substr(0, p.rfind('/')).c_str(), 0775);
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "%d %d\n", s_alarm_exit_s, s_alarm_duration_s);
    fclose(f);
    remove(p.c_str());
    rename(tmp.c_str(), p.c_str());
}

static const char *alarm_state_name(void)
{
    switch (s_alarm_state) {
        case ALARM_ARMING: return "arming";
        case ALARM_ARMED: return "armed";
        case ALARM_TRIGGERED: return "triggered";
        default: return "off";
    }
}

static int alarm_remaining_locked(uint32_t now_ms)
{
    int total = s_alarm_state == ALARM_ARMING ? s_alarm_exit_s :
                s_alarm_state == ALARM_TRIGGERED ? s_alarm_duration_s : 0;
    if (!total) return 0;
    uint32_t elapsed = now_ms - s_alarm_state_ms;
    uint32_t duration = (uint32_t)total * 1000;
    if (elapsed >= duration) return 0;
    return (int)((duration - elapsed + 999) / 1000);
}

static void publish_alarm(const char *event, uint32_t now_ms)
{
    std::string j = "{\"event\":";
    web_json_str(j, event);
    j += ",\"state\":";
    web_json_str(j, alarm_state_name());
    j += ",\"remaining_s\":" + tools_int(alarm_remaining_locked(now_ms)) +
        ",\"triggers\":" + tools_int(s_alarm_triggers) + ",\"zone\":";
    if (s_alarm_zone[0]) web_json_str(j, s_alarm_zone); else j += "null";
    j += "}";
    web_events_publish("presence.alarm", j.c_str());
}

/* s_csi_mx held. */
static void alarm_disarm_locked(uint32_t now_ms, const char *reason)
{
    if (s_alarm_state == ALARM_OFF) return;
    s_alarm_state = ALARM_OFF;
    s_alarm_state_ms = now_ms;
    s_alarm_zone[0] = 0;
    s_alarm_last_beep_s = 0xffffffff;
    publish_alarm(reason ? reason : "disarmed", now_ms);
}

/* s_csi_mx held. Motion, not the presence hold, triggers the alarm. */
static void alarm_step_locked(uint32_t now_ms)
{
    if (s_alarm_state == ALARM_ARMING &&
        (uint32_t)(now_ms - s_alarm_state_ms) >= (uint32_t)s_alarm_exit_s * 1000) {
        s_alarm_state = ALARM_ARMED;
        s_alarm_state_ms = now_ms;
        publish_alarm("armed", now_ms);
        web_hw_audio_beep(700, 80);
    }
    if (s_alarm_state == ALARM_ARMED &&
        (uint32_t)(now_ms - s_alarm_state_ms) >= ALARM_GRACE_MS) {
        bool router_motion = s_csi_active && !s_csi.calibrating && s_csi.motion;
        bool faro_motion = s_csi_active && faro_fresh_locked(now_ms) &&
                           !s_faro_zone.calibrating && s_faro_zone.motion;
        if (router_motion || faro_motion) {
            snprintf(s_alarm_zone, sizeof(s_alarm_zone), "%s", faro_motion && !router_motion ? "faro" : "router");
            s_alarm_state = ALARM_TRIGGERED;
            s_alarm_state_ms = now_ms;
            s_alarm_last_beep_s = 0xffffffff;
            s_alarm_triggers++;
            publish_alarm("triggered", now_ms);
            web_hw_notify("Presence alarm", faro_motion && !router_motion ?
                          "Motion on the Faro link" : "Motion on the router link", 2, true);
            web_hw_audio_beep(1100, 300);
        }
    }
    if (s_alarm_state == ALARM_TRIGGERED) {
        uint32_t elapsed = now_ms - s_alarm_state_ms;
        if (elapsed >= (uint32_t)s_alarm_duration_s * 1000) {
            alarm_disarm_locked(now_ms, "timeout");
            web_hw_audio_beep(350, 100);
        } else {
            uint32_t second = elapsed / 1000;
            if (second && second != s_alarm_last_beep_s) {
                s_alarm_last_beep_s = second;
                web_hw_audio_beep(1100, 180);
            }
        }
    }
}

/* ---------------------------------------------------------------- bounded graph */

static void history_reset_locked(uint32_t now_ms)
{
    memset(s_csi_history, 0, sizeof(s_csi_history));
    s_csi_history_count = s_csi_history_head = 0;
    s_history_bucket_ms = now_ms;
    s_history_router_max = s_history_faro_max = 0;
}

/* s_csi_mx held. Runtime-only by design: no stale motion after a reboot. */
static void history_step_locked(uint32_t now_ms)
{
    if (!s_csi_active) return;
    uint8_t router = score_percent(s_csi.score);
    uint8_t faro = faro_fresh_locked(now_ms) ? score_percent(s_faro_zone.score) : 0;
    if (router > s_history_router_max) s_history_router_max = router;
    if (faro > s_history_faro_max) s_history_faro_max = faro;
    uint32_t elapsed = now_ms - s_history_bucket_ms;
    if (elapsed < CSI_HISTORY_BUCKET_MS) return;
    uint32_t buckets = elapsed / CSI_HISTORY_BUCKET_MS;
    for (uint32_t n = 0; n < buckets; n++) {
        csi_history_sample_t &h = s_csi_history[s_csi_history_head];
        h.t = s_history_bucket_ms + (n + 1) * CSI_HISTORY_BUCKET_MS;
        h.router = s_history_router_max;
        h.faro = s_history_faro_max;
        s_csi_history_head = (s_csi_history_head + 1) % CSI_HISTORY_POINTS;
        if (s_csi_history_count < CSI_HISTORY_POINTS) s_csi_history_count++;
    }
    s_history_bucket_ms += buckets * CSI_HISTORY_BUCKET_MS;
    s_history_router_max = router;
    s_history_faro_max = faro;
}

/* s_csi_mx held. */
static void update_faro_link_locked(uint32_t now_ms)
{
    bool fresh = faro_fresh_locked(now_ms);
    if (fresh == s_faro_linked) return;
    s_faro_linked = fresh;
    publish_csi(fresh ? "faro_found" : "faro_lost", s_faro_zone, "faro", now_ms);
}

/* s_csi_mx held. */
static void csi_step_locked(uint32_t now_ms)
{
    csi_step_detector_locked(s_csi, s_csi_active, "router", now_ms);
    update_faro_link_locked(now_ms);
    csi_step_detector_locked(s_faro_zone, s_csi_active && s_faro_linked, "faro", now_ms);
    history_step_locked(now_ms);
    alarm_step_locked(now_ms);
}

#ifdef ARDUINO
struct csi_queued_t {
    int8_t data[CSI_RAW_MAX];
    uint16_t len;
    int8_t rssi;
    bool first_word_invalid;
    bool faro;
    uint8_t mac[6];
    uint32_t received_ms;
};
static csi_queued_t s_csi_queue[CSI_QUEUE_LEN];
static int s_csi_qread = 0, s_csi_qwrite = 0;
static uint32_t s_csi_qdrops = 0;
static std::mutex s_csi_qmx;
static uint8_t s_csi_bssid[6] = {};
static uint8_t s_faro_radio_mac[6] = {};
static bool s_faro_radio_known = false;
static bool s_csi_ps_saved = false;
static std::atomic<bool> s_csi_radio{false};
static wifi_ps_type_t s_csi_previous_ps = WIFI_PS_MIN_MODEM;
static int s_csi_socket = -1;
static uint32_t s_csi_gateway = 0;
static uint16_t s_csi_ping_sequence = 0;
static uint32_t s_csi_last_ping_ms = 0;
static uint32_t s_csi_link_check_ms = 0;

static bool faro_magic(const uint8_t *payload, int len)
{
    if (!payload || len < 8) return false;
    for (int i = 0; i + 8 <= len; i++) if (!memcmp(payload + i, "TWF1", 4)) return true;
    return false;
}

static void csi_rx_cb(void *, wifi_csi_info_t *info)
{
    if (!s_csi_active || !s_csi_radio || !info || !info->buf || !info->len) return;
    bool router = !memcmp(info->mac, s_csi_bssid, sizeof(s_csi_bssid));
    std::lock_guard<std::mutex> lk(s_csi_qmx);
    bool faro = !router && s_faro_radio_known && !memcmp(info->mac, s_faro_radio_mac, sizeof(s_faro_radio_mac));
    if (!router && !faro) return;
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
    sample.faro = faro;
    memcpy(sample.mac, info->mac, sizeof(sample.mac));
    sample.received_ms = tools_now_ms();
    s_csi_qwrite = next;
}

static void csi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !buf) return;
    const wifi_promiscuous_pkt_t *packet = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = packet->payload;
    int len = packet->rx_ctrl.sig_len;
    if (len < 24 || (frame[0] & 0xfc) != 0xd0 || !faro_magic(frame + 24, len - 24)) return;
    std::lock_guard<std::mutex> lk(s_csi_qmx);
    memcpy(s_faro_radio_mac, frame + 10, sizeof(s_faro_radio_mac));
    s_faro_radio_known = true;
}

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
    s_csi_radio = false;
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
    s_faro_radio_known = false;
    memset(s_faro_radio_mac, 0, sizeof(s_faro_radio_mac));
    snprintf(s_csi_ssid, sizeof(s_csi_ssid), "%s", (const char *)ap.ssid);
    s_csi_channel = ap.primary;
    if (!s_csi_ps_saved && esp_wifi_get_ps(&s_csi_previous_ps) == ESP_OK) s_csi_ps_saved = true;
    s_csi_radio = true;
    esp_wifi_set_ps(WIFI_PS_NONE);
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
        csi_reset_detector(s_csi, now_ms);
        s_faro_discovered = s_faro_linked = false;
        s_faro_mac[0] = 0;
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
        s_csi_samples = s_faro_samples = s_csi_dropped = 0;
        s_csi_rate = s_faro_rate = s_csi_rate_samples = s_faro_rate_samples = 0;
        s_csi_rate_ms = tools_now_ms();
        s_csi_channel = 0;
        s_csi_ssid[0] = 0;
        csi_set_error("");
        s_csi.threshold = threshold;
        s_faro_zone.threshold = threshold;
        s_faro_discovered = s_faro_linked = false;
        s_faro_seen_ms = 0;
        s_faro_mac[0] = 0;
        memset(&s_faro_zone, 0, sizeof(s_faro_zone));
        s_faro_zone.threshold = threshold;
#ifndef ARDUINO
        s_csi_sim_ms = tools_now_ms();
        snprintf(s_csi_ssid, sizeof(s_csi_ssid), "%s", "Simulator-WiFi");
        s_csi_channel = 1;
#endif
        uint32_t now = presence_now_ms();
        csi_reset_detector(s_csi, now);
        history_reset_locked(now);
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
    bool was_active = s_csi_active;
#ifdef ARDUINO
    csi_radio_disable();
#endif
    if (was_active) tools_wifi_release(CSI_RADIO_OWNER);
    std::lock_guard<std::mutex> lk(s_csi_mx);
    uint32_t now = presence_now_ms();
    alarm_disarm_locked(now, "sensing_stopped");
    s_csi_active = false;
    s_csi.calibrating = s_csi.motion = s_csi.present = false;
    s_csi.score = s_csi.level = 0;
    s_faro_zone.calibrating = s_faro_zone.motion = s_faro_zone.present = false;
    s_faro_zone.score = s_faro_zone.level = 0;
    s_faro_linked = false;
    s_csi_rate = s_faro_rate = 0;
}

/* ---------------------------------------------------------------- Faro transmitter */

static std::atomic<bool> s_faro_tx_active{false};
static std::atomic<uint32_t> s_faro_sent{0}, s_faro_tx_errors{0};
static uint32_t s_faro_tx_rate = 0, s_faro_tx_rate_sent = 0, s_faro_tx_rate_ms = 0;
static uint32_t s_faro_tx_last_ms = 0;
static long long s_faro_tx_started = 0;
static int s_faro_tx_channel = 0;
static char s_faro_tx_mac[18] = "";
static char s_faro_tx_error[160] = "";
static std::mutex s_faro_tx_mx;
#ifdef ARDUINO
static esp_timer_handle_t s_faro_timer = NULL;
static bool s_faro_espnow_owner = false, s_faro_peer_added = false;
static bool s_faro_ps_saved = false;
static wifi_ps_type_t s_faro_previous_ps = WIFI_PS_MIN_MODEM;
static const uint8_t FARO_BROADCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void faro_timer_cb(void *)
{
    if (!s_faro_tx_active) return;
    uint8_t payload[8] = { 'T', 'W', 'F', '1', 0, 0, 0, 0 };
    uint32_t sequence = s_faro_sent.load() + s_faro_tx_errors.load() + 1;
    memcpy(payload + 4, &sequence, sizeof(sequence));
    if (esp_now_send(FARO_BROADCAST, payload, sizeof(payload)) == ESP_OK) s_faro_sent++;
    else s_faro_tx_errors++;
}
#endif

static void faro_tx_set_error(const char *text)
{
    snprintf(s_faro_tx_error, sizeof(s_faro_tx_error), "%s", text ? text : "");
}

static int faro_tx_begin(int channel)
{
    if (channel < 0 || channel > 13) return 400;
    if (s_csi_active) {
        faro_tx_set_error("Stop CSI sensing before enabling the Faro transmitter");
        return 409;
    }
    std::lock_guard<std::mutex> lk(s_faro_tx_mx);
    if (s_faro_tx_active) return 0;
    std::string radio_error;
    if (!tools_wifi_claim(CSI_RADIO_OWNER, radio_error)) {
        faro_tx_set_error(radio_error.c_str());
        return 503;
    }
    s_faro_sent = s_faro_tx_errors = 0;
    s_faro_tx_rate = s_faro_tx_rate_sent = 0;
    s_faro_tx_rate_ms = s_faro_tx_last_ms = tools_now_ms();
    s_faro_tx_started = tools_epoch();
    faro_tx_set_error("");
#ifdef ARDUINO
    wifi_ap_record_t ap;
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (connected) {
        if (channel && channel != ap.primary) {
            char b[128];
            snprintf(b, sizeof(b), "Connected Wi-Fi uses channel %u; stop Wi-Fi or select that channel", ap.primary);
            faro_tx_set_error(b);
            tools_wifi_release(CSI_RADIO_OWNER);
            return 409;
        }
        channel = ap.primary;
    } else {
        if (!channel) channel = 1;
        WiFi.mode(WIFI_STA);
        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
            faro_tx_set_error("Could not select the Faro Wi-Fi channel");
            tools_wifi_release(CSI_RADIO_OWNER);
            return 503;
        }
    }
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_faro_tx_mac, sizeof(s_faro_tx_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (!s_faro_ps_saved && esp_wifi_get_ps(&s_faro_previous_ps) == ESP_OK) s_faro_ps_saved = true;
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_err_t rc = esp_now_init();
    s_faro_espnow_owner = rc == ESP_OK;
    if (rc != ESP_OK && rc != ESP_ERR_ESPNOW_EXIST) {
        faro_tx_set_error("ESP-NOW could not start");
        if (s_faro_ps_saved) esp_wifi_set_ps(s_faro_previous_ps);
        s_faro_ps_saved = false;
        tools_wifi_release(CSI_RADIO_OWNER);
        return 503;
    }
    if (!esp_now_is_peer_exist(FARO_BROADCAST)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, FARO_BROADCAST, 6);
        peer.channel = connected ? 0 : channel;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        rc = esp_now_add_peer(&peer);
        s_faro_peer_added = rc == ESP_OK;
        if (rc != ESP_OK) {
            faro_tx_set_error("Could not add the Faro broadcast peer");
            if (s_faro_espnow_owner) esp_now_deinit();
            s_faro_espnow_owner = false;
            if (s_faro_ps_saved) esp_wifi_set_ps(s_faro_previous_ps);
            s_faro_ps_saved = false;
            tools_wifi_release(CSI_RADIO_OWNER);
            return 503;
        }
    }
    s_faro_tx_active = true;
    esp_timer_create_args_t args = {};
    args.callback = faro_timer_cb;
    args.name = "presence_faro";
    rc = esp_timer_create(&args, &s_faro_timer);
    if (rc == ESP_OK) rc = esp_timer_start_periodic(s_faro_timer, FARO_INTERVAL_US);
    if (rc != ESP_OK) {
        s_faro_tx_active = false;
        if (s_faro_timer) esp_timer_delete(s_faro_timer);
        s_faro_timer = NULL;
        if (s_faro_peer_added) esp_now_del_peer(FARO_BROADCAST);
        s_faro_peer_added = false;
        if (s_faro_espnow_owner) esp_now_deinit();
        s_faro_espnow_owner = false;
        if (s_faro_ps_saved) esp_wifi_set_ps(s_faro_previous_ps);
        s_faro_ps_saved = false;
        tools_wifi_release(CSI_RADIO_OWNER);
        faro_tx_set_error("Could not start the 100 Hz Faro timer");
        return 503;
    }
#else
    if (!channel) channel = 1;
    snprintf(s_faro_tx_mac, sizeof(s_faro_tx_mac), "%s", "02:00:00:00:00:f0");
    s_faro_tx_active = true;
#endif
    s_faro_tx_channel = channel;
    web_events_publish("presence.faro", "{\"active\":true}");
    return 0;
}

static void faro_tx_finish(void)
{
    std::lock_guard<std::mutex> lk(s_faro_tx_mx);
    bool was_active = s_faro_tx_active;
    s_faro_tx_active = false;
#ifdef ARDUINO
    if (s_faro_timer) {
        esp_timer_stop(s_faro_timer);
        esp_timer_delete(s_faro_timer);
        s_faro_timer = NULL;
    }
    if (s_faro_peer_added) esp_now_del_peer(FARO_BROADCAST);
    s_faro_peer_added = false;
    if (s_faro_espnow_owner) esp_now_deinit();
    s_faro_espnow_owner = false;
    if (s_faro_ps_saved) esp_wifi_set_ps(s_faro_previous_ps);
    s_faro_ps_saved = false;
#endif
    if (was_active) tools_wifi_release(CSI_RADIO_OWNER);
    s_faro_tx_rate = 0;
    if (was_active) web_events_publish("presence.faro", "{\"active\":false}");
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

/* ---------------------------------------------------------------- JSON/API */

static const char *const APPS[] = { "Presence", "Nearby Peers", NULL };

static std::string zone_json_locked(bool faro, uint32_t now_ms)
{
    csi_detector_t &d = faro ? s_faro_zone : s_csi;
    bool linked = faro ? faro_fresh_locked(now_ms) : s_csi_active.load();
    std::string j = std::string("{\"id\":") + (faro ? "\"faro\"" : "\"router\"") +
        ",\"linked\":" + (linked ? "true" : "false") +
        ",\"calibrating\":" + (linked && d.calibrating ? "true" : "false") +
        ",\"present\":" + (linked && d.present ? "true" : "false") +
        ",\"motion\":" + (linked && d.motion ? "true" : "false") +
        ",\"score\":" + tools_num(linked ? d.score : 0, 3) +
        ",\"level\":" + tools_num(d.level, 5) +
        ",\"noise\":" + tools_num(d.noise, 5) +
        ",\"threshold\":" + tools_num(d.threshold > 0 ? d.threshold : 0.35f, 2) +
        ",\"samples\":" + tools_int(faro ? s_faro_samples : s_csi_samples) +
        ",\"rate\":" + tools_int(faro ? s_faro_rate : s_csi_rate) +
        ",\"drops\":" + tools_int(faro ? 0 : s_csi_dropped) +
        ",\"rssi\":" + tools_int(d.rssi) +
        ",\"channel\":" + (s_csi_channel ? tools_int(s_csi_channel) : "null") +
        ",\"last_motion_ms\":" + tools_int(d.last_motion_ms) + ",\"state\":";
    web_json_str(j, zone_state_locked(faro, now_ms));
    if (faro) {
        j += ",\"discovered\":" + std::string(s_faro_discovered ? "true" : "false") +
            ",\"last_seen_ms\":" + tools_int(s_faro_seen_ms) + ",\"mac\":";
        if (s_faro_mac[0]) web_json_str(j, s_faro_mac); else j += "null";
    }
    return j + "}";
}

static std::string csi_json_text(void)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    uint32_t now = presence_now_ms();
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
    j += ",\"zones\":[" + zone_json_locked(false, now) + "," + zone_json_locked(true, now) + "]";
    j += ",\"history\":";
    j += "[";
    int first = (s_csi_history_head + CSI_HISTORY_POINTS - s_csi_history_count) % CSI_HISTORY_POINTS;
    for (int i = 0; i < s_csi_history_count; i++) {
        const csi_history_sample_t &h = s_csi_history[(first + i) % CSI_HISTORY_POINTS];
        if (i) j += ",";
        j += "{\"t\":" + tools_int(h.t) + ",\"router\":" + tools_int(h.router) +
            ",\"faro\":" + tools_int(h.faro) + "}";
    }
    return j + "]}";
}

static std::string faro_json_text(void)
{
    std::lock_guard<std::mutex> lk(s_faro_tx_mx);
    std::string j = std::string("{\"active\":") + (s_faro_tx_active ? "true" : "false") +
        ",\"channel\":" + (s_faro_tx_channel ? tools_int(s_faro_tx_channel) : "null") +
        ",\"mac\":";
    if (s_faro_tx_mac[0]) web_json_str(j, s_faro_tx_mac); else j += "null";
    j += ",\"sent\":" + tools_int(s_faro_sent.load()) +
        ",\"rate\":" + tools_int(s_faro_tx_rate) +
        ",\"tx_errors\":" + tools_int(s_faro_tx_errors.load()) +
        ",\"started\":" + tools_int(s_faro_tx_started) + ",\"error\":";
    if (s_faro_tx_error[0]) web_json_str(j, s_faro_tx_error); else j += "null";
    return j + "}";
}

static std::string alarm_json_text(void)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    alarm_cfg_load();
    uint32_t now = presence_now_ms();
    std::string j = "{\"state\":";
    web_json_str(j, alarm_state_name());
    j += ",\"remaining_s\":" + tools_int(alarm_remaining_locked(now)) +
        ",\"exit_s\":" + tools_int(s_alarm_exit_s) +
        ",\"duration_s\":" + tools_int(s_alarm_duration_s) +
        ",\"triggers\":" + tools_int(s_alarm_triggers) + ",\"zone\":";
    if (s_alarm_zone[0]) web_json_str(j, s_alarm_zone); else j += "null";
    j += ",\"error\":";
    if (s_alarm_error[0]) web_json_str(j, s_alarm_error); else j += "null";
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
    return j + csi_json_text() + ",\"faro\":" + faro_json_text() + ",\"alarm\":" + alarm_json_text() + "}";
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
        const char *code = rc == 409 ? "conflict" : strstr(s_csi_error, "Stop ") ? "busy" :
                           strstr(s_csi_error, "Connect to") ? "wifi_required" : "csi_unavailable";
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
    std::string body, zone = "all";
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "zone", zone);
    if (zone != "all" && zone != "router" && zone != "faro") {
        web_resp_error(r, 400, "bad_request", "zone must be router, faro or all"); return;
    }
    std::lock_guard<std::mutex> lk(s_csi_mx);
    if (!s_csi_active) { web_resp_error(r, 409, "conflict", "start CSI sensing first"); return; }
    uint32_t now = presence_now_ms();
    if (zone == "faro" && !faro_fresh_locked(now)) {
        web_resp_error(r, 409, "conflict", "no live Faro link to calibrate"); return;
    }
    if (zone != "faro") csi_reset_detector(s_csi, now);
    if (zone != "router" && s_faro_discovered) csi_reset_detector(s_faro_zone, now);
    csi_set_error("");
    web_resp_ok(r);
}

static void start_faro(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body;
    double channel = 0;
    if (!tools_read_json(r, body)) return;
    if (web_json_has(body, "channel") && !web_json_get_num(body, "channel", &channel)) {
        web_resp_error(r, 400, "bad_request", "channel must be a number"); return;
    }
    if (channel != (int)channel || channel < 0 || channel > 13 ||
        (channel == 0 && web_json_has(body, "channel"))) {
        web_resp_error(r, 400, "bad_request", "channel: 1-13, or omit for the current/1"); return;
    }
    int rc = faro_tx_begin((int)channel);
    if (rc == 400) web_resp_error(r, 400, "bad_request", "channel: 1-13");
    else if (rc) web_resp_error(r, rc, rc == 409 ? "conflict" : strstr(s_faro_tx_error, "Stop ") ? "busy" : "faro_unavailable", s_faro_tx_error);
    else web_resp_ok(r);
}

static void stop_faro(web_req_t *r)
{
    faro_tx_finish();
    web_resp_ok(r);
}

static int alarm_arm(uint32_t now_ms, int exit_s, int duration_s)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    if (s_faro_tx_active || !s_csi_active || s_csi.calibrating) {
        snprintf(s_alarm_error, sizeof(s_alarm_error), "%s", "Finish router calibration before arming; Faro transmitters cannot sense");
        return 409;
    }
    if (s_alarm_state != ALARM_OFF) {
        snprintf(s_alarm_error, sizeof(s_alarm_error), "%s", "Disarm the alarm before changing its settings");
        return 409;
    }
    s_alarm_exit_s = exit_s;
    s_alarm_duration_s = duration_s;
    alarm_cfg_save();
    s_alarm_state = ALARM_ARMING;
    s_alarm_state_ms = now_ms;
    s_alarm_zone[0] = 0;
    s_alarm_error[0] = 0;
    publish_alarm("arming", now_ms);
    return 0;
}

static void start_alarm(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    alarm_cfg_load();
    std::string body;
    double exit_s = s_alarm_exit_s, duration_s = s_alarm_duration_s;
    if (!tools_read_json(r, body)) return;
    if (web_json_has(body, "exit_s") && !web_json_get_num(body, "exit_s", &exit_s)) {
        web_resp_error(r, 400, "bad_request", "exit_s must be a number"); return;
    }
    if (web_json_has(body, "duration_s") && !web_json_get_num(body, "duration_s", &duration_s)) {
        web_resp_error(r, 400, "duration_s must be a number"); return;
    }
    if (exit_s != (int)exit_s || exit_s < 5 || exit_s > 120 ||
        duration_s != (int)duration_s || duration_s < 10 || duration_s > 300) {
        web_resp_error(r, 400, "bad_request", "exit_s: 5-120; duration_s: 10-300"); return;
    }
    int rc = alarm_arm(presence_now_ms(), (int)exit_s, (int)duration_s);
    if (rc) web_resp_error(r, rc, "conflict", s_alarm_error);
    else web_resp_ok(r);
}

static void disarm_alarm(web_req_t *r)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    alarm_disarm_locked(presence_now_ms(), "disarmed");
    s_alarm_error[0] = 0;
    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
#ifndef ARDUINO
    if (tools_is(r, "POST", "/api/_dev/presence-csi")) {
        std::string body, pattern = "stable", zone = "router", mac;
        double count = 1, advance_ms = 0, rssi = -50;
        if (!tools_read_json(r, body)) return true;
        web_json_get_str(body, "pattern", pattern);
        web_json_get_str(body, "zone", zone);
        web_json_get_str(body, "mac", mac);
        web_json_get_num(body, "count", &count);
        web_json_get_num(body, "advance_ms", &advance_ms);
        web_json_get_num(body, "rssi", &rssi);
        if (!s_csi_active) { web_resp_error(r, 409, "conflict", "start CSI sensing first"); return true; }
        if (pattern != "stable" && pattern != "motion") { web_resp_error(r, 400, "bad_request", "pattern must be stable or motion"); return true; }
        if (zone != "router" && zone != "faro") { web_resp_error(r, 400, "bad_request", "zone must be router or faro"); return true; }
        if (count < 0 || count > 500 || advance_ms < 0 || advance_ms > 600000) {
            web_resp_error(r, 400, "bad_request", "count: 0-500; advance_ms: 0-600000"); return true;
        }
        std::lock_guard<std::mutex> lk(s_csi_mx);
        if (zone == "faro") {
            if (mac.empty()) mac = "02:00:00:00:00:f1";
            if (!s_faro_discovered || strcmp(s_faro_mac, mac.c_str())) {
                snprintf(s_faro_mac, sizeof(s_faro_mac), "%s", mac.c_str());
                memset(&s_faro_zone, 0, sizeof(s_faro_zone));
                s_faro_zone.threshold = s_csi.threshold;
                s_faro_samples = s_faro_rate = s_faro_rate_samples = 0;
                csi_reset_detector(s_faro_zone, s_csi_sim_ms);
                s_faro_discovered = true;
                s_faro_linked = false;
            }
        }
        for (int frame = 0; frame < (int)count; frame++) {
            int8_t raw[CSI_RAW_MAX] = {};
            for (int tone = 0; tone < 26; tone++) {
                int indices[2] = { tone + 1, 32 + 6 + tone };
                for (int side = 0; side < 2; side++) {
                    int i = indices[side];
                    float shape = 12 + (tone % 7) * 2 + (side ? 3 : 0);
                    if (pattern == "motion") {
                        float phase = (frame % 20) * 0.314159f;
                        shape *= 0.55f + 0.9f * (0.5f + 0.5f * sinf(phase + tone * 0.7f + side));
                    }
                    float gain = 1.0f + (frame % 5) * 0.15f;
                    raw[i * 2] = (int8_t)(shape * gain * 0.6f);
                    raw[i * 2 + 1] = (int8_t)(shape * gain * 0.8f);
                }
            }
            s_csi_sim_ms += 20;
            if (zone == "faro") {
                s_faro_seen_ms = s_csi_sim_ms;
                if (csi_feed_detector_locked(s_faro_zone, "faro", raw, sizeof(raw), (int)rssi, false, s_csi_sim_ms)) s_faro_samples++;
            } else if (csi_feed_detector_locked(s_csi, "router", raw, sizeof(raw), (int)rssi, false, s_csi_sim_ms)) {
                s_csi_samples++;
            }
            csi_step_locked(s_csi_sim_ms);
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
    else if (tools_is(r, "POST", "/api/presence/faro/start")) start_faro(r);
    else if (tools_is(r, "POST", "/api/presence/faro/stop")) stop_faro(r);
    else if (tools_is(r, "POST", "/api/presence/alarm/arm")) start_alarm(r);
    else if (tools_is(r, "POST", "/api/presence/alarm/disarm")) disarm_alarm(r);
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
            if (sample.faro) {
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         sample.mac[0], sample.mac[1], sample.mac[2], sample.mac[3], sample.mac[4], sample.mac[5]);
                if (!s_faro_discovered || strcmp(s_faro_mac, mac)) {
                    snprintf(s_faro_mac, sizeof(s_faro_mac), "%s", mac);
                    memset(&s_faro_zone, 0, sizeof(s_faro_zone));
                    s_faro_zone.threshold = s_csi.threshold;
                    s_faro_samples = s_faro_rate = s_faro_rate_samples = 0;
                    csi_reset_detector(s_faro_zone, sample.received_ms);
                    s_faro_discovered = true;
                    s_faro_linked = false;
                }
                s_faro_seen_ms = sample.received_ms;
                if (csi_feed_detector_locked(s_faro_zone, "faro", sample.data, sample.len, sample.rssi,
                                             sample.first_word_invalid, sample.received_ms)) s_faro_samples++;
            } else if (csi_feed_detector_locked(s_csi, "router", sample.data, sample.len, sample.rssi,
                                                sample.first_word_invalid, sample.received_ms)) {
                s_csi_samples++;
            }
            csi_step_locked(sample.received_ms);
        }
    }
#else
    if (s_faro_tx_active) {
        uint32_t elapsed = now_ms - s_faro_tx_last_ms;
        if (elapsed >= 10) {
            s_faro_sent += elapsed / 10;
            s_faro_tx_last_ms += (elapsed / 10) * 10;
        }
    }
#endif
    if (s_faro_tx_active && now_ms - s_faro_tx_rate_ms >= 1000) {
        std::lock_guard<std::mutex> lk(s_faro_tx_mx);
        uint32_t sent = s_faro_sent.load();
        s_faro_tx_rate = sent - s_faro_tx_rate_sent;
        s_faro_tx_rate_sent = sent;
        s_faro_tx_rate_ms = now_ms;
    }
    {
        std::lock_guard<std::mutex> lk(s_csi_mx);
#ifndef ARDUINO
        csi_step_locked(s_csi_sim_ms);
#else
        csi_step_locked(now_ms);
#endif
        if (now_ms - s_csi_rate_ms >= 1000) {
            s_csi_rate = s_csi_samples - s_csi_rate_samples;
            s_faro_rate = s_faro_linked ? s_faro_samples - s_faro_rate_samples : 0;
            s_csi_rate_samples = s_csi_samples;
            s_faro_rate_samples = s_faro_samples;
            s_csi_rate_ms = now_ms;
        }
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
    faro_tx_finish();
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
    uint32_t now = presence_now_ms();
    csi_reset_detector(s_csi, now);
    if (s_faro_discovered) csi_reset_detector(s_faro_zone, now);
    return true;
}
void web_hw_presence_csi_status(web_hw_csi_presence_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_csi_mx);
    memset(out, 0, sizeof(*out));
    uint32_t now = presence_now_ms();
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
    out->faro.discovered = s_faro_discovered;
    out->faro.linked = faro_fresh_locked(now);
    out->faro.calibrating = out->faro.linked && s_faro_zone.calibrating;
    out->faro.present = out->faro.linked && s_faro_zone.present;
    out->faro.motion = out->faro.linked && s_faro_zone.motion;
    out->faro.score = out->faro.linked ? s_faro_zone.score : 0;
    out->faro.samples = s_faro_samples;
    out->faro.rate = s_faro_rate;
    out->faro.rssi = s_faro_zone.rssi;
    out->faro.last_seen_ms = s_faro_seen_ms;
    snprintf(out->faro.mac, sizeof(out->faro.mac), "%s", s_faro_mac);
    out->history_count = s_csi_history_count;
    int first = (s_csi_history_head + CSI_HISTORY_POINTS - s_csi_history_count) % CSI_HISTORY_POINTS;
    for (int i = 0; i < s_csi_history_count; i++) {
        const csi_history_sample_t &h = s_csi_history[(first + i) % CSI_HISTORY_POINTS];
        out->history[i].t = h.t;
        out->history[i].router = h.router;
        out->history[i].faro = h.faro;
    }
}

bool web_hw_presence_faro_start(int channel)
{
    return faro_tx_begin(channel) == 0;
}
bool web_hw_presence_faro_stop(void)
{
    faro_tx_finish();
    return true;
}
void web_hw_presence_faro_status(web_hw_presence_faro_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_faro_tx_mx);
    memset(out, 0, sizeof(*out));
    out->active = s_faro_tx_active;
    out->channel = s_faro_tx_channel;
    out->sent = s_faro_sent.load();
    out->rate = s_faro_tx_rate;
    out->tx_errors = s_faro_tx_errors.load();
    out->started_epoch = s_faro_tx_started;
    out->error = s_faro_tx_error[0] ? s_faro_tx_error : NULL;
    snprintf(out->mac, sizeof(out->mac), "%s", s_faro_tx_mac);
}

bool web_hw_presence_alarm_arm(int exit_s, int duration_s)
{
    alarm_cfg_load();
    return alarm_arm(presence_now_ms(), exit_s, duration_s) == 0;
}
bool web_hw_presence_alarm_disarm(void)
{
    std::lock_guard<std::mutex> lk(s_csi_mx);
    alarm_disarm_locked(presence_now_ms(), "disarmed");
    s_alarm_error[0] = 0;
    return true;
}
void web_hw_presence_alarm_status(web_hw_presence_alarm_t *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(s_csi_mx);
    alarm_cfg_load();
    memset(out, 0, sizeof(*out));
    snprintf(out->state, sizeof(out->state), "%s", alarm_state_name());
    out->remaining_s = alarm_remaining_locked(presence_now_ms());
    out->exit_s = s_alarm_exit_s;
    out->duration_s = s_alarm_duration_s;
    out->triggers = s_alarm_triggers;
    out->error = s_alarm_error[0] ? s_alarm_error : NULL;
    snprintf(out->zone, sizeof(out->zone), "%s", s_alarm_zone);
}

static const char *const CAPS[] = { "presence.peers", "presence.csi", "presence.faro", "presence.alarm", "presence.history", NULL };
const web_tool_t web_tool_presence = { CAPS, handle, presence_tick, presence_second, presence_stop };

#else
const web_tool_t web_tool_presence = { NULL, NULL, NULL, NULL, NULL };
#endif
