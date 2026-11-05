/**
 * @file      web_tools.h
 * @brief     Tools of the web API (docs/web-api.md, "Tools (P4)"): the contract every tool
 *            module implements, helpers they share, and the hardware they use.
 *
 *  Layout
 *    web_tools.cpp         registry: routing, capabilities, the tick timer, start/stop
 *    web_tool_system.cpp   monitor, logs, notifications
 *    web_tool_sensors.cpp  IMU, GNSS
 *    web_tool_lora.cpp     LoRa
 *    web_tool_nfc.cpp      NFC
 *    web_tool_audio.cpp    recorder, player
 *    web_tool_scan.cpp     Wi-Fi and BLE scans
 *    web_tools_hw.cpp      web_hw_*(): the watch's hardware, or fakes in the simulator
 *
 *  Tool modules are platform-independent: they keep the tool's state, check requests, answer
 *  and publish events, and reach the hardware only through web_hw_*(). Request handlers run
 *  on the server's thread; web_hw_*() calls go through web_ui_run() unless marked otherwise.
 */
#pragma once

#include "web_internal.h"

#if FACTORY_WEB

#include <string>
#include <vector>

/* ---------------------------------------------------------------- the tool contract */

typedef struct {
    const char *const *caps;            /* capabilities it adds to /api/caps, NULL-terminated */
    bool (*handle)(web_req_t *r);       /* answer the request if the path is the tool's; true if it was */
    void (*tick)(uint32_t now_ms);      /* UI thread, every 50 ms (may be NULL) */
    void (*second)(uint32_t now_ms);    /* UI thread, once a second (may be NULL) */
    void (*stop)(void);                 /* UI thread, the server stops: let go of the hardware (may be NULL) */
} web_tool_t;

/* Each module defines its tools; web_tools.cpp lists those this board has. */
extern const web_tool_t web_tool_monitor, web_tool_notify, web_tool_sensors, web_tool_gnss, web_tool_lora,
       web_tool_nfc, web_tool_recorder, web_tool_audio, web_tool_wifi_scan, web_tool_ble_scan;

/* ---------------------------------------------------------------- shared helpers (web_tools.cpp) */

uint32_t tools_now_ms(void);
long long tools_epoch(void);                    /* UTC seconds from the watch clock */
std::string tools_int(long long v);
std::string tools_num(double v, int decimals);  /* "null" for NaN */
bool tools_is(const web_req_t *r, const char *method, const char *path);
/* Read a JSON object body ("{}" when empty); answers 400 and returns false otherwise. */
bool tools_read_json(web_req_t *r, std::string &body, size_t max = 2048);
std::string tools_b64_encode(const uint8_t *data, size_t len);
bool tools_b64_decode(const std::string &in, std::string &out);

/* The app open on the watch ("" for none): the real one, or the one a test set (/api/_dev/app). */
const char *tools_current_app(void);
/* Which of apps (NULL-terminated names) is open on the watch now, or NULL. */
const char *tools_holder(const char *const *apps);
/* If one of apps holds the tool, answer 503 busy naming it and return true. */
bool tools_busy(web_req_t *r, const char *const *apps);

/* ---------------------------------------------------------------- hardware (web_tools_hw.cpp)
 * The watch's hardware on the watch, fakes in the simulator. UI thread unless noted. */

/* The app open on the watch ("LoRa", "NFC", ...), "" on the face or the app list. Any thread. */
const char *web_hw_current_app(void);

typedef struct {
    int cpu_mhz;
    bool has_ma; int ma;                /* battery current, negative = discharging */
    bool has_temp; float temp_c;
    bool has_vbus; int vbus_mv;
    uint32_t heap_min, heap_total, psram_total;
} web_hw_monitor_t;
void web_hw_monitor(web_hw_monitor_t *out);

/* A pop-up on the watch that wakes its screen; level 0 info, 1 warn, 2 alert. */
void web_hw_notify(const char *title, const char *body, int level, bool vibrate);

typedef struct {
    float roll, pitch, heading;
    bool accel_valid; float ax, ay, az;
    uint32_t seq;
    bool has_steps; uint32_t steps;
} web_hw_imu_t;
void web_hw_imu_power(bool on);
void web_hw_imu_read(web_hw_imu_t *out);

typedef struct {
    const char *sys;
    int prn, el, az, cn0;
    bool has_cn0, used;
} web_hw_sat_t;
typedef struct {
    char model[24];
    bool fix;
    double lat, lon;
    bool has_alt; double alt;
    bool has_speed; double speed_kmh;
    int sats;
    long long time;                     /* UTC epoch, 0 = unknown */
    bool has_ttff; uint32_t ttff_ms;
    const char *antenna;
    int nsat;
    web_hw_sat_t sat[40];
} web_hw_gnss_t;
/* Read the receiver (about once a second). false when there is nothing new. */
bool web_hw_gnss_read(web_hw_gnss_t *out);

typedef struct {
    float freq, bw;
    int sf, cr, power, sync_word;
    bool listen;
} web_hw_lora_cfg_t;
void web_hw_lora_defaults(web_hw_lora_cfg_t *out);
bool web_hw_lora_apply(const web_hw_lora_cfg_t &c);       /* listen: receive, else standby */
bool web_hw_lora_send(const uint8_t *data, size_t len);   /* starts sending */
bool web_hw_lora_tx_done(void);
bool web_hw_lora_rx(uint8_t *buf, size_t *len, int *rssi, int *snr);   /* a packet, up to 255 bytes */
void web_hw_lora_release(void);                           /* radio to standby, for the watch's apps */

struct web_nfc_tag_t {
    char uid[40];                       /* hex with colons */
    char type[32];
    int nrec;
    struct { const char *kind; char value[128]; } rec[4];
};
bool web_hw_nfc_start(void);            /* tags come back through web_tool_nfc_tag() */
void web_hw_nfc_stop(void);
void web_hw_nfc_loop(void);
void web_tool_nfc_tag(const web_nfc_tag_t &tag);          /* web_tool_nfc.cpp; any thread */

/* Microphone, 16 kHz mono 16-bit. read runs on the recorder's task. */
bool web_hw_mic_open(void);
int web_hw_mic_read(int16_t *buf, size_t samples);        /* samples read, 0 = none yet, -1 = error */
void web_hw_mic_close(void);

/* Speaker. root "sd" or "flash", fs_path the path inside it ("/data/x.wav"), host the host path. */
bool web_hw_audio_play(const char *root, const char *fs_path, const char *host);
void web_hw_audio_stop(void);
bool web_hw_audio_playing(void);
int web_hw_audio_volume(void);
void web_hw_audio_set_volume(int pct);

/* Scans: block the calling task for the scan's length; run them on their own task. */
typedef struct {
    char ssid[33];
    char bssid[18];
    int rssi, channel;
    const char *auth;
} web_hw_wifi_t;
bool web_hw_wifi_scan(std::vector<web_hw_wifi_t> &out);
typedef struct {
    std::string addr, name, mfr;
    int rssi;
    bool connectable;
    std::vector<std::string> services;
} web_hw_ble_t;
bool web_hw_ble_scan(int seconds, std::vector<web_hw_ble_t> &out);

#ifndef ARDUINO
/* Simulator test hook: a LoRa packet arrives (/api/_dev/lora). */
void web_hw_sim_lora_inject(const std::string &data, int rssi, int snr);
#endif

#endif /* FACTORY_WEB */
