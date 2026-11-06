/**
 * @file      web_tools_hw.cpp
 * @brief     The hardware behind the web tools (web_tools.h, web_hw_*): the watch's on the
 *            watch (ARDUINO), fakes in the simulator, side by side per tool so they stay alike.
 *
 *            Fakes (docs/web-api.md, Test hooks): a GNSS fix walking around one spot, a slowly
 *            turning IMU, a LoRa station that answers each packet with "ack: <text>", a tag one
 *            second after the NFC reader starts, a 440 Hz tone as the microphone, playback that
 *            lasts as long as the WAV, and a few Wi-Fi networks and BLE devices.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include "ui_define.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <mutex>
#include <deque>
#include <chrono>
#include <thread>

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#endif
#if FACTORY_WATCH_UI
#include "ui_watch_shell.h"
#endif

static void sleep_ms(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/* ---------------------------------------------------------------- app, monitor, notification */

const char *web_hw_current_app(void)
{
    return hw_current_app();
}

void web_hw_monitor(web_hw_monitor_t *out)
{
    memset(out, 0, sizeof(*out));
#ifdef ARDUINO
    out->cpu_mhz = (int)getCpuFrequencyMhz();
    out->heap_min = ESP.getMinFreeHeap();
    out->heap_total = ESP.getHeapSize();
    out->psram_total = ESP.getPsramSize();
    power_monitor_snapshot_t p;
    memset(&p, 0, sizeof(p));
    hw_get_power_monitor_snapshot(p);
    out->has_ma = p.battery_ma.valid;
    out->ma = (int)p.battery_ma.value;
    out->has_temp = p.battery_temperature_c.valid || p.temperature_c.valid;
    out->temp_c = p.battery_temperature_c.valid ? p.battery_temperature_c.value : p.temperature_c.value;
    out->has_vbus = p.vbus_mv.valid;
    out->vbus_mv = (int)p.vbus_mv.value;
#else
    /* A watch on the desk: a little current, a warm battery, numbers that move. */
    uint32_t t = lv_tick_get() / 1000;
    out->cpu_mhz = 240;
    out->heap_min = 150000;
    out->heap_total = 320000;
    out->psram_total = 8388608;
    out->has_ma = true;
    out->ma = -80 - (int)(t % 7) * 3;
    out->has_temp = true;
    out->temp_c = 27.5f + (float)(t % 10) / 10.0f;
    out->has_vbus = true;
    out->vbus_mv = 0;
#endif
}

void web_hw_notify(const char *title, const char *body, int level, bool vibrate)
{
#if FACTORY_WATCH_UI
    watch_shell_wake();
#endif
    ui_msg_pop_up(title, body && *body ? body : " ");
    if (vibrate) hw_haptic(level >= 2 ? HW_HAPTIC_ALERT : level == 1 ? HW_HAPTIC_FAILURE : HW_HAPTIC_TAP);
}

/* ---------------------------------------------------------------- IMU */

void web_hw_imu_power(bool on)
{
#ifdef ARDUINO
    if (on) hw_register_imu_process();
    else hw_unregister_imu_process();
#else
    (void)on;
#endif
}

void web_hw_imu_read(web_hw_imu_t *out)
{
    memset(out, 0, sizeof(*out));
#ifdef ARDUINO
    imu_params_t p;
    hw_get_imu_params(p);
    out->roll = p.roll;
    out->pitch = p.pitch;
    out->heading = p.heading;
    out->accel_valid = p.accel_valid;
    out->ax = p.accel_x;
    out->ay = p.accel_y;
    out->az = p.accel_z;
    out->seq = p.accel_sequence;
    uint32_t steps = 0;
    out->has_steps = hw_get_step_count(steps);
    out->steps = steps;
#else
    float t = lv_tick_get() / 1000.0f;
    out->roll = 12.0f * sinf(t * 0.7f);
    out->pitch = 8.0f * cosf(t * 0.5f);
    out->heading = fmodf(t * 6.0f, 360.0f);
    out->accel_valid = true;
    out->ax = 9.81f * sinf(out->pitch * (float)M_PI / 180.0f);
    out->ay = 9.81f * sinf(out->roll * (float)M_PI / 180.0f);
    out->az = sqrtf(fmaxf(0.0f, 96.2f - out->ax * out->ax - out->ay * out->ay));
    out->seq = lv_tick_get() / 20;                  /* 50 samples a second */
    out->has_steps = true;
    out->steps = 1200 + lv_tick_get() / 1500;
#endif
}

/* ---------------------------------------------------------------- GNSS */

#ifdef ARDUINO
static const char *sat_system(gps_satellite_system_t s)
{
    switch (s) {
    case GPS_SAT_SYSTEM_GPS: return "GPS";
    case GPS_SAT_SYSTEM_GLONASS: return "GLONASS";
    case GPS_SAT_SYSTEM_BEIDOU: return "BeiDou";
    case GPS_SAT_SYSTEM_GALILEO: return "Galileo";
    case GPS_SAT_SYSTEM_QZSS: return "QZSS";
    default: return "GPS";
    }
}

static long long utc_epoch(const struct tm &t)
{
    /* timegm without the libc extension: days since 1970 by Howard Hinnant's algorithm. */
    long long y = t.tm_year + 1900;
    unsigned m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}
#endif

bool web_hw_gnss_read(web_hw_gnss_t *out)
{
#ifdef ARDUINO
    static gps_params_t p;              /* big: not on the stack */
    p.nmea_to_serial = false;
    if (!hw_get_gps_info(p)) return false;
    snprintf(out->model, sizeof(out->model), "%s", p.model);
    out->fix = p.location_valid;
    out->lat = p.lat;
    out->lon = p.lng;
    out->has_alt = p.altitude_valid;
    out->alt = p.altitude;
    out->has_speed = p.speed_valid;
    out->speed_kmh = p.speed;
    out->sats = p.satellite;
    out->time = p.datetime_valid ? utc_epoch(p.datetime) : 0;
    out->has_ttff = p.ttff_valid;
    out->ttff_ms = p.ttff_ms;
    out->antenna = p.antenna_state == GPS_ANTENNA_OK ? "ok" : p.antenna_state == GPS_ANTENNA_OPEN ? "open"
                   : p.antenna_state == GPS_ANTENNA_SHORT ? "short" : "unknown";
    out->nsat = 0;
    for (int i = 0; i < p.signal_satellite_count && out->nsat < (int)(sizeof(out->sat) / sizeof(out->sat[0])); i++) {
        const gps_signal_satellite_t &s = p.signal_satellites[i];
        out->sat[out->nsat++] = { sat_system(s.system), s.prn, s.elevation, s.azimuth, s.cn0, s.has_cn0, s.used };
    }
    return true;
#else
    /* A fix walking slowly around a spot in Warsaw, with ten satellites. */
    static const struct { const char *sys; int prn, el, az, cn0; } SATS[] = {
        { "GPS", 3, 64, 40, 42 }, { "GPS", 8, 31, 120, 37 }, { "GPS", 14, 12, 200, 24 }, { "GPS", 22, 48, 300, 40 },
        { "Galileo", 5, 55, 80, 39 }, { "Galileo", 11, 22, 250, 30 }, { "GLONASS", 7, 40, 160, 33 },
        { "BeiDou", 19, 70, 10, 41 }, { "BeiDou", 27, 8, 330, 18 }, { "QZSS", 194, 5, 140, 0 },
    };
    double t = lv_tick_get() / 1000.0;
    snprintf(out->model, sizeof(out->model), "Simulator");
    out->fix = true;
    out->lat = 52.2297 + 0.0006 * sin(t / 60.0);
    out->lon = 21.0122 + 0.0009 * cos(t / 60.0);
    out->has_alt = true;
    out->alt = 112.0 + 2.0 * sin(t / 17.0);
    out->has_speed = true;
    out->speed_kmh = 4.2;
    out->time = (long long)time(NULL);
    out->has_ttff = true;
    out->ttff_ms = 31000;
    out->antenna = "ok";
    out->nsat = 0;
    for (auto &s : SATS) out->sat[out->nsat++] = { s.sys, s.prn, s.el, s.az, s.cn0, s.cn0 > 0, s.cn0 >= 30 };
    out->sats = 0;
    for (int i = 0; i < out->nsat; i++) out->sats += out->sat[i].used;
    return true;
#endif
}

/* ---------------------------------------------------------------- LoRa */

void web_hw_lora_defaults(web_hw_lora_cfg_t *out)
{
    radio_params_t p;
    hw_get_radio_params(p);
    *out = { p.freq, p.bandwidth, p.sf, p.cr, p.power, p.syncWord, true };
}

#ifdef ARDUINO

static uint8_t s_tx_buf[256];

static int16_t lora_set(const web_hw_lora_cfg_t &c, uint8_t mode)
{
    radio_params_t p;
    hw_get_radio_params(p);
    p.freq = c.freq;
    p.bandwidth = c.bw;
    p.sf = (uint8_t)c.sf;
    p.cr = (uint16_t)c.cr;
    p.power = (uint8_t)(c.power < 0 ? 0 : c.power);    /* the HAL's power is unsigned */
    p.syncWord = (uint8_t)c.sync_word;
    p.mode = mode;
    p.isRunning = mode != RADIO_DISABLE;
    return hw_set_radio_params(p);
}

bool web_hw_lora_apply(const web_hw_lora_cfg_t &c)
{
    return lora_set(c, c.listen ? RADIO_RX : RADIO_DISABLE) == 0;
}

bool web_hw_lora_send(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_tx_buf)) return false;
    memcpy(s_tx_buf, data, len);
    radio_tx_params_t tx = { s_tx_buf, len, 0 };
    hw_set_radio_tx(tx, false);
    return tx.state == 0;
}

bool web_hw_lora_tx_done(void)
{
    int16_t state;
    return hw_get_radio_tx_done(state);
}

bool web_hw_lora_rx(uint8_t *buf, size_t *len, int *rssi, int *snr)
{
    radio_rx_params_t rx = { buf, 0, 0, 0, -1 };
    hw_get_radio_rx(rx);
    if (rx.state != 0 || rx.length == 0 || rx.length > 255) return false;
    *len = rx.length;
    *rssi = rx.rssi;
    *snr = rx.snr;
    return true;
}

void web_hw_lora_release(void)
{
    web_hw_lora_cfg_t c;
    web_hw_lora_defaults(&c);
    lora_set(c, RADIO_DISABLE);
}

#else   /* simulator: a station in range that answers each packet */

struct sim_lora_packet_t { uint32_t at; std::string data; int rssi, snr; };
static std::mutex s_sim_lora_mx;
static std::deque<sim_lora_packet_t> s_sim_air;
static bool s_sim_listen = false;
static uint32_t s_sim_tx_until = 0;

bool web_hw_lora_apply(const web_hw_lora_cfg_t &c)
{
    std::lock_guard<std::mutex> lk(s_sim_lora_mx);
    s_sim_listen = c.listen;
    return true;
}

bool web_hw_lora_send(const uint8_t *data, size_t len)
{
    std::lock_guard<std::mutex> lk(s_sim_lora_mx);
    uint32_t now = lv_tick_get();
    s_sim_tx_until = now + 100;
    s_sim_air.push_back({ now + 1000, "ack: " + std::string((const char *)data, len), -61, 9 });
    return true;
}

bool web_hw_lora_tx_done(void)
{
    return (int32_t)(lv_tick_get() - s_sim_tx_until) >= 0;
}

bool web_hw_lora_rx(uint8_t *buf, size_t *len, int *rssi, int *snr)
{
    std::lock_guard<std::mutex> lk(s_sim_lora_mx);
    if (s_sim_air.empty() || (int32_t)(lv_tick_get() - s_sim_air.front().at) < 0) return false;
    sim_lora_packet_t p = s_sim_air.front();
    s_sim_air.pop_front();
    if (!s_sim_listen) return false;
    *len = p.data.size() > 255 ? 255 : p.data.size();
    memcpy(buf, p.data.data(), *len);
    *rssi = p.rssi;
    *snr = p.snr;
    return true;
}

void web_hw_lora_release(void)
{
    std::lock_guard<std::mutex> lk(s_sim_lora_mx);
    s_sim_listen = false;
    s_sim_air.clear();
}

void web_hw_sim_lora_inject(const std::string &data, int rssi, int snr)
{
    std::lock_guard<std::mutex> lk(s_sim_lora_mx);
    s_sim_air.push_back({ lv_tick_get(), data, rssi, snr });
}

#endif

/* ---------------------------------------------------------------- NFC */

#ifdef ARDUINO

bool web_hw_nfc_start(void)
{
    return hw_start_nfc_discovery();
}

void web_hw_nfc_stop(void)
{
    hw_stop_nfc_discovery();
}

void web_hw_nfc_loop(void)
{
    hw_loop_nfc();
}

static const char *record_kind(LilyGoNfcRecordKind k)
{
    switch (k) {
    case LILYGO_NFC_RECORD_TEXT: return "text";
    case LILYGO_NFC_RECORD_URI: return "uri";
    case LILYGO_NFC_RECORD_WIFI: return "wifi";
    default: return "other";
    }
}

/* From the reader's callback (hal_interface.cpp): a card read, with its NDEF records if any. */
void web_server_nfc_reader_event(LilyGoNfcEvent event, const LilyGoNfcReaderResult &result)
{
    if (event != LILYGO_NFC_EVENT_NDEF_READ && event != LILYGO_NFC_EVENT_NDEF_UNSUPPORTED &&
            event != LILYGO_NFC_EVENT_NDEF_ERROR) {
        return;                         /* one report per card, once its NDEF is known */
    }
    web_nfc_tag_t tag;
    memset(&tag, 0, sizeof(tag));
    size_t o = 0;
    for (uint8_t i = 0; i < result.uidLen && o + 3 < sizeof(tag.uid); i++) {
        o += snprintf(tag.uid + o, sizeof(tag.uid) - o, i ? ":%02x" : "%02x", result.uid[i]);
    }
    snprintf(tag.type, sizeof(tag.type), "%s", result.cardType[0] ? result.cardType : result.tagType);
    for (uint8_t i = 0; i < result.recordCount && tag.nrec < 4; i++) {
        const LilyGoNfcRecord &rec = result.records[i];
        tag.rec[tag.nrec].kind = record_kind(rec.kind);
        snprintf(tag.rec[tag.nrec].value, sizeof(tag.rec[0].value), "%s",
                 rec.kind == LILYGO_NFC_RECORD_WIFI ? rec.wifiSsid : rec.value);
        tag.nrec++;
    }
    web_tool_nfc_tag(tag);
}

#else   /* simulator: a tag is held to the reader a second after it starts */

static uint32_t s_sim_nfc_at = 0;

bool web_hw_nfc_start(void)
{
    s_sim_nfc_at = lv_tick_get() + 1000;
    return true;
}

void web_hw_nfc_stop(void)
{
    s_sim_nfc_at = 0;
}

void web_hw_nfc_loop(void)
{
    if (!s_sim_nfc_at || (int32_t)(lv_tick_get() - s_sim_nfc_at) < 0) return;
    s_sim_nfc_at = 0;
    web_nfc_tag_t tag;
    memset(&tag, 0, sizeof(tag));
    snprintf(tag.uid, sizeof(tag.uid), "04:5a:3c:12:8e:61:80");
    snprintf(tag.type, sizeof(tag.type), "NTAG215");
    tag.nrec = 2;
    tag.rec[0].kind = "uri";
    snprintf(tag.rec[0].value, sizeof(tag.rec[0].value), "https://www.lilygo.cc");
    tag.rec[1].kind = "text";
    snprintf(tag.rec[1].value, sizeof(tag.rec[1].value), "Hello from the simulator");
    web_tool_nfc_tag(tag);
}

#endif

/* ---------------------------------------------------------------- microphone */

#ifdef ARDUINO

static AudioInputIf *s_mic = NULL;
static uint8_t s_mic_ch = 1;
static int32_t s_dc = 0;                /* running DC offset, Q8 */

bool web_hw_mic_open(void)
{
    s_mic = instance.getAudioInput();
    if (!s_mic) return false;
    s_mic_ch = instance.getCodecInputChannels();
    if (s_mic_ch < 1) s_mic_ch = 1;
    s_dc = 0;
    if (!s_mic->open(16, s_mic_ch, 16000)) {
        s_mic = NULL;
        return false;
    }
    if (hw_has_mic_input_source_setting()) hw_apply_mic_input_source();
    return true;
}

int web_hw_mic_read(int16_t *buf, size_t samples)
{
    if (!s_mic) return -1;
    static int16_t raw[320 * 2];
    size_t frames = samples;
    if (frames * s_mic_ch > sizeof(raw) / sizeof(raw[0])) frames = sizeof(raw) / sizeof(raw[0]) / s_mic_ch;
    int got = s_mic->read((uint8_t *)raw, frames * s_mic_ch * sizeof(int16_t));
    if (got < 0) return -1;
    int n = got / (int)(s_mic_ch * sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        int32_t v = raw[i * s_mic_ch];                  /* the first channel: the watch's microphone */
        s_dc += ((v << 8) - s_dc) >> 10;                /* remove the PDM path's DC bias */
        v -= s_dc >> 8;
        buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return n;
}

void web_hw_mic_close(void)
{
    if (s_mic) s_mic->close();
    s_mic = NULL;
}

#else   /* simulator: a 440 Hz tone, at the real pace */

static uint32_t s_sim_mic_t0 = 0, s_sim_mic_sent = 0;

bool web_hw_mic_open(void)
{
    s_sim_mic_t0 = lv_tick_get();
    s_sim_mic_sent = 0;
    return true;
}

int web_hw_mic_read(int16_t *buf, size_t samples)
{
    uint32_t due = (lv_tick_get() - s_sim_mic_t0) * 16;
    if (due <= s_sim_mic_sent) return 0;
    size_t n = due - s_sim_mic_sent;
    if (n > samples) n = samples;
    for (size_t i = 0; i < n; i++) {
        double t = (double)(s_sim_mic_sent + i) / 16000.0;
        buf[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * t) * (0.6 + 0.4 * sin(2 * M_PI * 0.5 * t)));
    }
    s_sim_mic_sent += (uint32_t)n;
    return (int)n;
}

void web_hw_mic_close(void) {}

#endif

/* ---------------------------------------------------------------- speaker */

static uint32_t s_play_started = 0;     /* the player starts asynchronously: count as playing for a moment */

#ifdef ARDUINO

static char s_play_path[256];           /* the player keeps the pointer */

bool web_hw_audio_play(const char *root, const char *fs_path, const char *host)
{
    (void)host;
    snprintf(s_play_path, sizeof(s_play_path), "%s", fs_path);
    hw_set_sd_music_play(!strcmp(root, "sd") ? AUDIO_SOURCE_SDCARD : AUDIO_SOURCE_FATFS, s_play_path);
    s_play_started = lv_tick_get();
    return true;
}

void web_hw_audio_stop(void)
{
    hw_set_play_stop();
    s_play_started = 0;
}

bool web_hw_audio_playing(void)
{
    if (hw_player_running()) return true;
    return s_play_started && lv_tick_get() - s_play_started < 1500;
}

int web_hw_audio_volume(void)
{
    return hw_get_volume();
}

void web_hw_audio_set_volume(int pct)
{
    hw_set_volume((uint8_t)pct);
}

#else   /* simulator: "plays" for as long as the WAV lasts (3 s for anything else) */

static uint32_t s_sim_play_until = 0;
static int s_sim_volume = 70;

static uint32_t wav_ms(const char *host)
{
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    uint8_t h[44];
    size_t n = fread(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (n < 44 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 3000;
    uint32_t rate = h[28] | h[29] << 8 | h[30] << 16 | (uint32_t)h[31] << 24;   /* bytes per second */
    return rate ? (uint32_t)((size - 44) * 1000LL / rate) : 3000;
}

bool web_hw_audio_play(const char *root, const char *fs_path, const char *host)
{
    (void)root;
    (void)fs_path;
    uint32_t ms = wav_ms(host);
    s_play_started = lv_tick_get();
    s_sim_play_until = s_play_started + (ms < 500 ? 500 : ms);
    return true;
}

void web_hw_audio_stop(void)
{
    s_sim_play_until = 0;
    s_play_started = 0;
}

bool web_hw_audio_playing(void)
{
    return s_sim_play_until && (int32_t)(lv_tick_get() - s_sim_play_until) < 0;
}

int web_hw_audio_volume(void)
{
    return s_sim_volume;
}

void web_hw_audio_set_volume(int pct)
{
    s_sim_volume = pct;
}

#endif

/* ---------------------------------------------------------------- Wi-Fi scan */

#ifdef ARDUINO

static const char *auth_name(uint8_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "other";
    }
}

bool web_hw_wifi_scan(std::vector<web_hw_wifi_t> &out)
{
    out.clear();
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        /* The watch's own Wi-Fi page is scanning: use its result. */
    } else if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
        return false;
    }
    for (int waited = 0; WiFi.scanComplete() == WIFI_SCAN_RUNNING && waited < 15000; waited += 100) sleep_ms(100);
    int n = WiFi.scanComplete();
    for (int i = 0; i < n; i++) {
        web_hw_wifi_t w;
        memset(&w, 0, sizeof(w));
        snprintf(w.ssid, sizeof(w.ssid), "%s", WiFi.SSID(i).c_str());
        snprintf(w.bssid, sizeof(w.bssid), "%s", WiFi.BSSIDstr(i).c_str());
        w.rssi = WiFi.RSSI(i);
        w.channel = WiFi.channel(i);
        w.auth = auth_name(WiFi.encryptionType(i));
        out.push_back(w);
    }
    WiFi.scanDelete();
    return n >= 0;
}

#else

bool web_hw_wifi_scan(std::vector<web_hw_wifi_t> &out)
{
    static const struct { const char *ssid, *bssid; int rssi, channel; const char *auth; } NETS[] = {
        { "LilyGo-AABB0", "a4:cf:12:aa:bb:c0", -42, 6, "wpa2" },
        { "Home Network 5G", "3c:84:6a:10:22:31", -58, 36, "wpa2/wpa3" },
        { "Cafe Guest", "f0:9f:c2:7a:01:9e", -71, 11, "open" },
        { "Office", "00:1d:7e:55:42:10", -77, 1, "wpa2-ent" },
        { "Neighbour's very long network name", "b0:be:76:43:19:aa", -84, 1, "wpa" },
    };
    sleep_ms(1000);
    out.clear();
    for (auto &n : NETS) {
        web_hw_wifi_t w;
        memset(&w, 0, sizeof(w));
        snprintf(w.ssid, sizeof(w.ssid), "%s", n.ssid);
        snprintf(w.bssid, sizeof(w.bssid), "%s", n.bssid);
        w.rssi = n.rssi;
        w.channel = n.channel;
        w.auth = n.auth;
        out.push_back(w);
    }
    return true;
}

#endif

/* ---------------------------------------------------------------- BLE scan */

#ifdef ARDUINO

static std::string hex(const std::string &s, size_t max)
{
    static const char D[] = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < s.size() && i < max; i++) {
        o += D[(uint8_t)s[i] >> 4];
        o += D[(uint8_t)s[i] & 15];
    }
    return o;
}

bool web_hw_ble_scan(int seconds, std::vector<web_hw_ble_t> &out, bool active)
{
    out.clear();
    bool started_stack = false;
    if (!NimBLEDevice::isInitialized()) {
        if (!NimBLEDevice::init("T-Watch")) return false;
        started_stack = true;
    }
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan) return false;
    scan->setScanCallbacks(NULL, false);
    scan->setActiveScan(active);
    scan->setInterval(100);
    scan->setWindow(100);
    scan->setMaxResults(64);
    NimBLEScanResults res = scan->getResults((uint32_t)seconds * 1000, false);
    for (int i = 0; i < res.getCount(); i++) {
        const NimBLEAdvertisedDevice *d = res.getDevice(i);
        if (!d) continue;
        web_hw_ble_t b;
        b.addr = d->getAddress().toString();
        b.name = d->haveName() ? d->getName() : "";
        b.rssi = d->getRSSI();
        b.connectable = d->isConnectable();
        b.mfr = d->haveManufacturerData() ? hex(d->getManufacturerData(), 32) : "";
        for (int k = 0; k < d->getServiceUUIDCount() && k < 8; k++) b.services.push_back(d->getServiceUUID(k).toString());
        out.push_back(b);
    }
    scan->clearResults();
    if (started_stack) NimBLEDevice::deinit(true);      /* give the memory back */
    return true;
}

#else

bool web_hw_ble_scan(int seconds, std::vector<web_hw_ble_t> &out, bool active)
{
    sleep_ms((uint32_t)seconds * 1000);
    out = {
        { "c8:2b:96:11:04:7e", "Mi Band 7", "5701", -63, true, { "0xfee0" } },
        { "e4:5f:01:a2:33:90", "", "4c0012190010", -72, false, {} },
        { "d0:3e:4a:77:12:05", "JBL Flip 5", "", -58, true, { "0x110b", "0x110e" } },
        { "f1:22:93:0c:aa:41", "T-Watch-4F2A", "", -49, true, { "6e400001-b5a3-f393-e0a9-e50e24dcca9e" } },
        { "7a:10:c3:55:9e:02", "", "0600010920", -88, false, {} },
    };
    return true;
}

#endif

#endif /* FACTORY_WEB */
