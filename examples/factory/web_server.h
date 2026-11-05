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

/* Start the server (idempotent). Call from the UI thread after the GUI is set up. */
bool web_server_start(void);
void web_server_stop(void);
bool web_server_running(void);

/* The 6-digit pairing PIN, regenerated at every start. */
const char *web_server_pin(void);

/* Publish one event to the connected desktop: d_json is a JSON object ("{...}") or NULL for {}.
 * Thread-safe. */
void web_events_publish(const char *topic, const char *d_json);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_WEB */
