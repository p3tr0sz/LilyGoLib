/**
 * @file      web_internal.h
 * @brief     Web server internals shared by the core (web_core.cpp, web_fs.cpp, web_json.cpp)
 *            and the two transports (web_transport_esp.cpp on the watch,
 *            web_transport_posix.cpp in the simulator).
 */
#pragma once

#include "web_server.h"

#if FACTORY_WEB

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <functional>
#include <vector>

/* ---------------------------------------------------------------- request / response */

/* One HTTP request. The transport fills the fields and the function pointers; the core
 * answers through web_resp_*(). Header values the core needs are copied in up front. */
struct web_req_t {
    char method[8];
    char path[256];             /* URL path, percent-decoded */
    char query[640];            /* raw query string, without '?' */
    char cookie[320];
    char range[64];
    char accept_encoding[64];
    char if_none_match[48];
    char user_agent[128];       /* User-Agent header for fingerprinting (security tools) */
    long long content_length;   /* -1 when unknown */

    void *ctx;                  /* transport handle */
    /* Read up to n body bytes; returns bytes read, 0 at the end, -1 on error. */
    int (*read)(web_req_t *r, char *buf, size_t n);
    /* Start the answer. headers: extra "Name: value\r\n" lines or NULL. length: body size, or -1 (chunked). */
    bool (*begin)(web_req_t *r, int status, const char *type, const char *headers, long long length);
    bool (*write)(web_req_t *r, const char *data, size_t n);
    void (*end)(web_req_t *r);
};

/* Answer helpers (core). */
void web_resp_json(web_req_t *r, int status, const std::string &json, const char *headers = NULL);
void web_resp_error(web_req_t *r, int status, const char *code, const char *message = NULL, const char *extra_json = NULL);
void web_resp_ok(web_req_t *r, const char *extra_json = NULL);   /* {"ok":true,<extra>} */
void web_resp_html(web_req_t *r, int status, const char *html);  /* text/html response */

/* Query parameter, percent-decoded. false when absent. */
bool web_query_get(const web_req_t *r, const char *name, char *out, size_t out_len);

/* Read the whole body (small JSON bodies only). false when larger than max or on error. */
bool web_read_body(web_req_t *r, std::string &out, size_t max);

/* ---------------------------------------------------------------- core entry points (transports call these) */

void web_api_handle(web_req_t *r);
/* A WebSocket asks for /api/events. Returns false to refuse (not paired). */
bool web_api_ws_open(int conn, const char *cookie);
void web_api_ws_closed(int conn);

/* ---------------------------------------------------------------- transport (one per platform) */

bool web_transport_start(uint16_t port);
void web_transport_stop(void);
bool web_transport_ws_send(int conn, const char *text, size_t len);
void web_transport_ws_close(int conn);

/* ---------------------------------------------------------------- platform (one per platform) */

/* Host folder for an API root: "sd" or "flash". NULL for an unknown root. */
const char *web_plat_root_dir(const char *root);
void web_plat_random(uint8_t *buf, size_t n);

typedef struct {
    bool present;
    bool mounted;
    uint64_t total;
    uint64_t free;
    const char *card;           /* "SDHC", "SDSC", ... or NULL */
    const char *error;          /* NULL or an error code */
} web_sd_info_t;

/* UI thread only. with_space reads total/free, which can be slow on a large card: only on demand. */
void web_plat_sd_info(web_sd_info_t *out, bool with_space);
bool web_plat_sd_mount(void);
void web_plat_sd_unmount(void);

typedef struct {
    int percent;
    bool charging;
    int mv;
    char mode[8];               /* "sta", "ap", "off" */
    char ssid[33];
    char ip[16];
    int rssi;
    uint32_t heap_free;
    uint32_t psram_free;
    uint64_t flash_total;
    uint64_t flash_free;
} web_dev_info_t;

/* UI thread only. */
void web_plat_dev_info(web_dev_info_t *out);
const char *web_plat_fw_version(void);
void web_plat_reboot(void);
/* One line to the console (serial on the watch, stdout in the simulator). */
void web_plat_log(const char *fmt, ...);

/* Wi-Fi housekeeping, once a second while the server runs (UI thread): on the watch, start the
 * watch's own access point when no known network is joined, and stop it again once one is. */
void web_plat_net_tick(bool server_running);
void web_plat_net_stop(void);
/* The access point, when it is on. */
bool web_plat_ap_info(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/* ---- desktop self-install (web_desktop.cpp) */
/* Run fn(arg) on its own task / thread, with room for file and network work. */
void web_plat_spawn(void (*fn)(void *), void *arg);
/* Raw DEFLATE (ZIP method 8), whole buffer: the ROM's tinfl on the watch, zlib in the simulator. */
bool web_plat_inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
/* Can the watch reach the internet now (joined a network)? */
bool web_plat_online(void);
/* Where releases are: the ZIP is fetched from <base>/<zip name>. */
std::string web_plat_desktop_url(const char *version, const char *zip_name);
/* Download url to path, resuming from the file's current size when the server takes a Range.
 * progress(done, total). Returns 0 when complete, the HTTP status on an HTTP error, -1 on a
 * network error. Runs on the desktop task. */
int web_plat_download(const char *url, const char *path, std::function<void(long long, long long)> progress);

/* Serialise access to the SD/SPI bus around one file I/O step (no-op in the simulator). */
void web_plat_io_lock(void);
void web_plat_io_unlock(void);

/* ---------------------------------------------------------------- core services */

/* Run fn on the LVGL/UI thread and wait for it (up to timeout_ms). false on timeout. */
bool web_ui_run(std::function<void()> fn, uint32_t timeout_ms = 3000);

/* Screen, input and display routes (web_screen.cpp). true when the path was one of them. */
bool web_screen_handle(web_req_t *r);
std::string web_screen_last_input(void);

/* Desktop self-install (web_desktop.cpp). */
void web_desktop_check(const char *source);      /* "auto", "card" or "download"; returns at once */
void web_desktop_card_mounted(void);             /* a card was just mounted: check, without the retry pause */
bool web_desktop_handle(web_req_t *r);
std::string web_desktop_json(void);
void web_desktop_status_line(char *out, size_t n);

/* Tool routes (web_tools.cpp and web_tool_*.cpp, P4; see web_tools.h). true when the path was one of them. */
bool web_tools_handle(web_req_t *r);
/* The tool capabilities this build has ("gnss", "lora", ...), appended to /api/caps. */
void web_tools_caps(std::vector<const char *> &out);
void web_tools_start(void);                     /* UI thread, when the server starts */
void web_tools_stop(void);                      /* UI thread, when it stops: release the hardware */
/* The last log lines (GET /api/logs). web_plat_log() feeds it. Thread-safe. */
void web_log_add(const char *line);

/* Files routes (web_fs.cpp). */
void web_fs_handle(web_req_t *r);
/* Map an API path ("/sd/x") to a host path. Returns 0, 400 (bad path) or 503 (SD missing). */
int web_fs_resolve(const char *api_path, std::string &host, std::string *root_out = NULL);
bool web_fs_sd_mounted(void);

/* Static files (web_core.cpp). */
const char *web_mime_type(const char *path);

/* ---------------------------------------------------------------- JSON (web_json.cpp) */

/* Append a JSON string literal (with quotes, escaped). */
void web_json_str(std::string &out, const char *s);
/* Read one top-level field of a flat JSON object. */
bool web_json_get_str(const std::string &json, const char *key, std::string &out);
bool web_json_get_num(const std::string &json, const char *key, double *out);
bool web_json_get_bool(const std::string &json, const char *key, bool *out);
bool web_json_has(const std::string &json, const char *key);
/* Is the text a JSON object (balanced, top-level {...})? */
bool web_json_is_object(const std::string &json);

#endif /* FACTORY_WEB */
