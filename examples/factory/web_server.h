/**
 * @file      web_server.h
 * @brief     Web desktop server: web API v1 (docs/web-api.md) for the T-Watch Ultra.
 *
 * The same code runs on the watch (esp_http_server) and in the desktop simulator
 * (a small POSIX server on localhost:8080). Everything is behind FACTORY_WEB.
 */
#pragma once

#include "hal_interface.h"

#if FACTORY_WEB

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before setupGui(): creates the input device the browser drives. Pages hook swipe-back on
 * the pointer devices that exist when they are built, so it must exist before the first page. */
void web_server_init(void);

/* Start the server (idempotent). Call from the UI thread after the GUI is set up. */
bool web_server_start(void);
void web_server_stop(void);
bool web_server_running(void);

/* The 6-digit pairing PIN, regenerated at every start. */
const char *web_server_pin(void);

/* Publish one event to the connected desktop: d_json is a JSON object ("{...}") or NULL for {}.
 * Thread-safe. */
void web_events_publish(const char *topic, const char *d_json);

/* Whether the server starts at boot (saved on the flash). Off by default on the watch,
 * on in the simulator. Setting it also starts or stops the server. UI thread. */
bool web_server_enabled(void);
void web_server_set_enabled(bool on);

/* Pairing works only while the watch shows the PIN (Settings > Web desktop). Opening the
 * window makes a new PIN. UI thread. */
void web_server_set_pairing(bool open);

/* True while a desktop is connected or a request came in the last minute: the watch must
 * not light-sleep then, or Wi-Fi drops (the screen may still go dark). */
bool web_server_keep_awake(void);

typedef struct {
    bool running;
    bool desktop_connected;
    bool pairing_open;
    char pin[7];
    char ip[16];
    char host[24];              /* "watch.local" */
    bool ap_active;             /* the watch's own Wi-Fi network is on */
    char ap_ssid[33];
    char ap_pass[24];
    unsigned sessions;          /* paired browsers */
    char desktop[40];           /* "0.1.0", "Downloading 40%", "Not installed" */
} web_status_t;

/* A snapshot for the watch's Web page. UI thread. */
void web_server_status(web_status_t *out);

/* Forget every paired browser. */
void web_server_forget_browsers(void);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_WEB */
