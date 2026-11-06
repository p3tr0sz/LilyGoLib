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
       web_tool_nfc, web_tool_recorder, web_tool_audio, web_tool_wifi_scan, web_tool_ble_scan,
       web_tool_link, web_tool_hid, web_tool_net, web_tool_ota,
       web_tool_sniffer, web_tool_airspace, web_tool_sentinel, web_tool_presence, web_tool_fido2,
       web_tool_evilportal, web_tool_deauthflood, web_tool_beaconspam;

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

/* P9 Wi-Fi tools share one radio and one promiscuous callback. Claim it before changing
 * Wi-Fi state; on failure error names the tool to stop. Works for API and native watch UI. */
bool tools_wifi_claim(const char *owner, std::string &error);
void tools_wifi_release(const char *owner);

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
bool web_hw_ble_scan(int seconds, std::vector<web_hw_ble_t> &out, bool active = true);

/* ---------------------------------------------------------------- P5 hardware (web_tools_hw2.cpp) */

/* Link: ESP-NOW file transfer between watches. The whole transfer lives in the hardware layer
 * (a real ESP-NOW protocol on the watch, a fake in the simulator); the tool shows it and starts it. */
typedef struct {
    char mac[18];                       /* "aa:bb:cc:dd:ee:ff" */
    char name[32];
    int rssi;
    long long last_seen;                /* epoch seconds */
} web_hw_peer_t;
typedef struct {
    bool active;
    bool tx;                            /* true sending, false receiving */
    char peer[18];
    char name[64];
    uint32_t size, done;
    const char *state;                  /* "sending", "receiving", "done", "error" */
    const char *error;                  /* NULL or a code */
} web_hw_link_transfer_t;
bool web_hw_link_set_enabled(bool on);
bool web_hw_link_enabled(void);
void web_hw_link_self(char *mac, size_t mac_len, char *name, size_t name_len);
int  web_hw_link_peers(web_hw_peer_t *out, int max);
/* Start sending host_path (named name) to to_mac. 0 ok, 404 no peer, 409 busy, 503 SD gone, 500 other. */
int  web_hw_link_send(const char *host_path, const char *name, const char *to_mac);
void web_hw_link_cancel(void);
bool web_hw_link_transfer(web_hw_link_transfer_t *out);     /* false when none; fills the current one */
void web_hw_link_poll(void);                                /* UI thread tick: move the transfer along */

/* USB HID keyboard (DuckyScript runs in the tool; these type the keys). Modifier bits below. */
#define WEB_HID_CTRL    1
#define WEB_HID_SHIFT   2
#define WEB_HID_ALT     4
#define WEB_HID_GUI     8
bool web_hw_hid_available(void);
bool web_hw_hid_text(const char *s);                        /* type a line of text */
bool web_hw_hid_key(const char *name, int modifiers);       /* tap a named key ("ENTER","r","F5") with modifiers */
void web_hw_hid_release(void);

/* Wi-Fi tools: the watch's own connection, DNS, TCP reach. */
typedef struct {
    bool connected;
    char ssid[33], ip[16], gateway[16], netmask[16], dns[16], mac[18];
    int rssi, channel;
} web_hw_net_t;
void web_hw_net_info(web_hw_net_t *out);
/* DNS resolve: 1 ok (ip, ms), 0 failed (ms), -1 offline. */
int web_hw_net_resolve(const char *host, char *ip, size_t ip_len, int *ms);
/* TCP connect test: 1 open, 0 refused/timeout (ms), -1 offline. */
int web_hw_net_tcp(const char *host, int port, int *ms);

/* Firmware update into the spare OTA slot. */
typedef struct {
    const char *slot, *other_slot;      /* "app0" / "app1" */
    const char *state;                  /* "idle", "writing", "ready", "error" */
    const char *error;
    uint32_t done, total;
    bool can_rollback;
} web_hw_ota_t;
void web_hw_ota_status(web_hw_ota_t *out);
bool web_hw_ota_begin(long long total);                     /* open the spare slot; false too large / no room */
bool web_hw_ota_write(const uint8_t *data, size_t len);     /* stream */
int  web_hw_ota_end(void);                                  /* finish + verify: 0 ok, else set state error */
bool web_hw_ota_commit(void);                               /* boot the new slot, reboot after ~1 s */
bool web_hw_ota_rollback(void);                             /* boot the other slot, reboot */

/* ---------------------------------------------------------------- P9 — NucleoOS parity (web_tools_hw3.cpp)
 *
 * Wi-Fi sniffer — passive promiscuous capture → .pcap on the SD
 * Airspace      — Wi-Fi attack detection from passive capture (deauth floods, evil-twin, beacon floods)
 * Sentinel      — BLE anti-stalking: passive scan flags devices that follow you
 * Presence      — Wi-Fi CSI motion/presence + ESP-NOW nearby devices
 * FIDO2         — USB/BLE FIDO2/U2F authenticator
 * BadUSB lib    — DuckyScript payload library (extends usb.hid)
 * BLE tools     — extended BLE scan (services, characteristics, advertise)
 * Evil portal   — rogue AP + captive portal for credential harvesting (security audit)
 * Deauth flood  — 802.11 deauth injection to disconnect devices (security audit)
 * Beacon spam   — flood fake SSIDs into the airspace (security audit)
 */

/* Evil portal — rogue AP + captive portal (three modes: standalone, evil-twin, KARMA lure) */

/* Portal operating modes */
#define EP_MODE_STANDALONE  0   /* Own AP with chosen SSID */
#define EP_MODE_TWIN        1   /* Clone real AP + deauth its clients onto us */
#define EP_MODE_KARMA       2   /* Probe-response: become the SSID devices are looking for */

/* Template: built-in portal page or .html file from /sd/evilportal/portals/ */
int  web_hw_evilportal_template_count(void);
const char *web_hw_evilportal_template_name(int idx);
bool web_hw_evilportal_template_is_builtin(int idx);

/* SSID presets: credible names like "TIM-7AC3F1" (MAC-derived suffix) */
int  web_hw_evilportal_ssid_count(void);
const char *web_hw_evilportal_ssid_preset(int idx);

/* Live page cloning: join open_ssid, fetch its captive-portal page, save as cloned.html */
int  web_hw_evilportal_clone_page(const char *open_ssid);  /* >0 bytes saved, -1 portal running, -2 join failed, -3 no captive */
bool web_hw_evilportal_cloning(void);
const char *web_hw_evilportal_clone_name(void);

/* Status */
typedef struct {
    bool active;
    int  mode;                      /* EP_MODE_* */
    char ssid[33];
    char bssid[18];                 /* twin: the real AP being cloned */
    int  channel;                   /* twin: channel of the real AP */
    bool twin_coherent;             /* twin: matches real WPA2 security (no downgrade) */
    unsigned long deauth_frames;    /* twin: deauth frames sent at real AP */
    int  clients;                   /* stations associated */
    uint32_t attempts;              /* credential submissions */
    uint32_t confirmed;             /* credentials whose password was re-entered */
    const char *error;
    long long started_epoch;
    bool dhcps_ok;                  /* false when DHCP server failed → clients stall */
    int  probes_total;              /* captive-detection HTTP hits */
    int  vendor_counts[6];          /* client OS: 0=iOS,1=Android,2=Windows,3=macOS,4=Linux,5=Other */
} web_hw_evilportal_t;

/* Start standalone: open AP with ssid, captive DNS, serve template_idx on :80 */
bool web_hw_evilportal_start(int template_idx, const char *ssid, const char *gateway_ip);
/* Start evil-twin: clone real AP on its channel, deauth real bssid, serve template */
bool web_hw_evilportal_start_twin(int template_idx, const char *ssid, const uint8_t bssid[6],
                                   int channel, int authmode, bool coherent);
/* Start KARMA: probe-response lure — beacon the SSID devices are probing for */
bool web_hw_evilportal_start_karma(int template_idx);
bool web_hw_evilportal_stop(void);
void web_hw_evilportal_status(web_hw_evilportal_t *out);

/* Credential management */
typedef struct {
    char mac[18];
    char hostname[64];
    char url[256];
    char username[64];
    char password[64];
    long long ts;
} web_hw_evilportal_capture_t;
bool web_hw_evilportal_captures(std::vector<web_hw_evilportal_capture_t> &out);
void web_hw_evilportal_clear_captures(void);
const char *web_hw_evilportal_logpath(void);   /* SD path of this session's loot file */

/* Password redaction modes */
#define EP_PWD_FULL     0   /* show full password */
#define EP_PWD_MASKED   1   /* show first+last char, mask middle */
#define EP_PWD_HIDDEN   2   /* show only "******" */
#define EP_PWD_LENGTH   3   /* show only length e.g. "8 chars" */
void web_hw_evilportal_set_pwd_mode(int mode);
int  web_hw_evilportal_pwd_mode(void);

/* PSK verification (twin mode): verify captured password against real AP */
void web_hw_evilportal_set_verify(bool on);
bool web_hw_evilportal_verify_enabled(void);
int  web_hw_evilportal_verified_ok(void);
int  web_hw_evilportal_verified_bad(void);

/* Gateway IP selection (before arming) */
void web_hw_evilportal_set_gateway(const char *ip);
const char *web_hw_evilportal_gateway(void);

/* Auto-stop: terminate after idle if no clients */
void web_hw_evilportal_set_auto_stop(uint16_t base_s, uint16_t extended_s);

/* Debug logging to SD (opt-in, no credentials logged) */
void web_hw_evilportal_set_debug(bool on);
bool web_hw_evilportal_debug(void);

/* KARMA: probe sniffing for nearby device SSID wishes */
int  web_hw_evilportal_karma_start(int secs);
bool web_hw_evilportal_karma_busy(void);
int  web_hw_evilportal_karma_count(void);
const char *web_hw_evilportal_karma_ssid(int i);
int  web_hw_evilportal_karma_hits(int i);

#ifndef ARDUINO
void web_hw_sim_evilportal_client(const char *mac, const char *hostname);
void web_hw_sim_evilportal_creds(const char *url, const char *user, const char *pass);
void web_hw_sim_evilportal_clone(const char *ssid, int bytes);
void web_hw_sim_evilportal_probe(const char *ssid, int hits);
#endif

/* Deauth flood — 802.11 deauth injection */
typedef struct {
    bool active;
    char target_bssid[18];          /* "ff:ff:ff:ff:ff:ff" = broadcast */
    char target_station[18];        /* "ff:ff:ff:ff:ff:ff" = all stations */
    char target_ssid[33];           /* human-readable name of target */
    int channel;
    uint32_t sent_count;
    uint32_t rate_kbps;
    long long started_epoch;
    const char *error;
} web_hw_deauthflood_t;
bool web_hw_deauthflood_start(const char *bssid, const char *station, int channel, int rate);
bool web_hw_deauthflood_stop(void);
void web_hw_deauthflood_status(web_hw_deauthflood_t *out);
/* Quick list of nearby APs to pick from (from last Wi-Fi scan) */
int  web_hw_deauthflood_scan_targets(void);
bool web_hw_deauthflood_scanning(void);
int  web_hw_deauthflood_target_count(void);
const char *web_hw_deauthflood_target_ssid(int i);
const char *web_hw_deauthflood_target_bssid(int i);
int  web_hw_deauthflood_target_channel(int i);
int  web_hw_deauthflood_target_rssi(int i);
const char *web_hw_deauthflood_target_auth(int i);

/* Beacon spam — flood fake SSIDs */
typedef struct {
    bool active;
    int  mode;                      /* 0=random, 1=from-list, 2=custom */
    int  channel;                   /* 0=hop all */
    uint32_t beacon_count;
    uint32_t ssid_count;
    long long started_epoch;
    const char *error;
} web_hw_beaconspam_t;
bool web_hw_beaconspam_start(int channel, int count, const char *prefix);
bool web_hw_beaconspam_stop(void);
void web_hw_beaconspam_status(web_hw_beaconspam_t *out);
void web_hw_beaconspam_set_custom_ssids(const char **ssids, int count);
int  web_hw_beaconspam_custom_count(void);

/* Wi-Fi sniffer — passive promiscuous capture → .pcap on the SD */

/* Wi-Fi sniffer */
typedef struct {
    bool active;
    int channel;                /* -1 = hop all channels */
    const char *filter;          /* "all", "management", "data", "beacons", "probes", "eapol" */
    uint32_t packet_count;
    uint32_t dropped;
    char file[128];             /* .pcap path on the SD */
    uint32_t bytes_written;
    uint32_t seen;              /* frames observed, including filtered-out ones */
    uint32_t beacons, probes, eapol, handshakes;
    const char *error;
} web_hw_sniffer_t;
bool web_hw_sniffer_start(int channel, uint32_t max_bytes, const char *path);
bool web_hw_sniffer_start_filter(int channel, uint32_t max_bytes, const char *path, const char *filter);
bool web_hw_sniffer_stop(void);
void web_hw_sniffer_status(web_hw_sniffer_t *out);
#ifndef ARDUINO
void web_hw_sim_sniffer_inject(int count);  /* inject fake packets into capture */
#endif

/* Airspace — Wi-Fi attack detection */
typedef struct {
    bool active;
    uint32_t deauth_count;      /* total deauth packets seen */
    uint32_t beacon_count;      /* total beacon frames seen */
    uint32_t deauth_floods;     /* deauth flood events (rate > threshold) */
    uint32_t beacon_floods;     /* beacon flood events */
    uint32_t evil_twins;        /* evil-twin AP candidates */
    uint32_t broadcast_deauth;  /* broadcast deauth/disassociation alerts */
    uint32_t drops;             /* frames dropped when the analysis queue was full */
    long long started_epoch;
    const char *error;
} web_hw_airspace_t;
bool web_hw_airspace_start(void);
bool web_hw_airspace_stop(void);
void web_hw_airspace_status(web_hw_airspace_t *out);

/* Sentinel — BLE anti-stalking */
typedef struct {
    char addr[18];
    char name[64];
    char kind[24];              /* Find My, Samsung SmartTag, Tile, Chipolo */
    int rssi;
    long long first_seen;
    long long last_seen;
    int sightings;
    bool stalking;              /* persistent-tracker heuristic, not proof of intent */
    bool separated;             /* Find My separated/lost-mode advertisement */
} web_hw_sentinel_device_t;
typedef struct {
    bool active;
    bool scanning;
    int device_count;
    web_hw_sentinel_device_t devices[24];
    long long started_epoch;
    const char *error;
} web_hw_sentinel_t;
bool web_hw_sentinel_start(void);
bool web_hw_sentinel_stop(void);
void web_hw_sentinel_status(web_hw_sentinel_t *out);

/* Nearby peers over ESP-NOW, plus Wi-Fi CSI motion/presence sensing. */
typedef struct {
    char mac[18];
    char name[64];
    int rssi;
    long long first_seen, last_seen;
    bool present;
} web_hw_presence_peer_t;
typedef struct {
    long long ts;
    char mac[18], name[64], event[16];
} web_hw_presence_history_t;
typedef struct {
    bool active;
    long long started_epoch;
    int peers_ever, peer_count, history_count;
    web_hw_presence_peer_t peers[32];
    web_hw_presence_history_t history[50];
    const char *error;
} web_hw_presence_t;
bool web_hw_presence_start(void);
bool web_hw_presence_stop(void);
void web_hw_presence_status(web_hw_presence_t *out);

typedef struct {
    bool active;
    bool calibrating;
    bool present;               /* latched for 10 s after the latest motion */
    bool motion;
    float score;                /* normalized 0..1 */
    float threshold;            /* motion threshold, 0.05..0.95 */
    uint32_t samples;
    uint32_t rate;              /* accepted CSI frames in the last second */
    uint32_t drops;             /* raw samples dropped by the driver/UI queue */
    int rssi;
    int channel;
    long long started_epoch;
    char ssid[33];
    const char *error;
} web_hw_csi_presence_t;
bool web_hw_presence_csi_start(float threshold);
bool web_hw_presence_csi_stop(void);
bool web_hw_presence_csi_calibrate(void);
void web_hw_presence_csi_status(web_hw_csi_presence_t *out);

/* FIDO2 — security key */
typedef struct {
    bool available;             /* USB HID enumerated */
    const char *state;          /* "idle", "registering", "authenticating" */
    const char *error;
    int credentials;            /* number of stored credentials */
} web_hw_fido2_t;
bool web_hw_fido2_begin_register(const char *rp, const char *user);
bool web_hw_fido2_begin_authenticate(const char *rp);
int  web_hw_fido2_credentials(void);
bool web_hw_fido2_remove_credential(int index);
void web_hw_fido2_status(web_hw_fido2_t *out);

#ifndef ARDUINO
/* Simulator test hooks. */
void web_hw_sim_lora_inject(const std::string &data, int rssi, int snr);    /* /api/_dev/lora */
void web_hw_sim_link_peer(const char *name);                                /* /api/_dev/link {peer} */
void web_hw_sim_link_rx(const char *name, uint32_t size);                   /* /api/_dev/link {rx} */
void web_hw_sim_hid_available(bool on);                                     /* /api/_dev/hid */
void web_hw_sim_sniffer_inject(int count);                                  /* /api/_dev/sniffer */
void web_hw_sim_airspace_alert(const char *type, const char *detail);       /* /api/_dev/airspace */
void web_hw_sim_sentinel_device(const char *addr, const char *name, int rssi); /* /api/_dev/sentinel */
void web_hw_sim_fido2_available(bool on);                                   /* /api/_dev/fido2 */
#endif

#endif /* FACTORY_WEB */
