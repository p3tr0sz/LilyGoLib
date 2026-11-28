/**
 * @file      web_provision.h
 * @brief     Wi-Fi setup from a phone: the watch raises a short-lived SoftAP with a
 *            PIN shown on screen; the phone joins it, a captive page lists nearby
 *            networks and takes the home password (typed on the phone, not the watch),
 *            and the watch saves the credentials and joins. No BLE, so it does not
 *            fight the NimBLE stack the rest of the firmware uses.
 *
 *            Server side lives here (captive page + the /prov endpoints, shared by the
 *            watch transport and the simulator transport). The watch UI entry point is
 *            in ui_wireless.cpp. SoftAP work is Arduino-only; the simulator stubs it so
 *            the page and the endpoints can still be exercised.
 */
#pragma once

#include "web_server.h"

#if FACTORY_WEB

#include <stddef.h>

struct web_req_t;

/* Where the provisioning flow is. The watch UI polls web_provision_state() to show it. */
enum web_prov_state_t {
    WEB_PROV_IDLE = 0,      /* not running */
    WEB_PROV_WAITING,       /* AP up, waiting for the phone to send credentials */
    WEB_PROV_CONNECTING,    /* credentials received, joining the home network */
    WEB_PROV_CONNECTED,     /* joined; credentials saved */
    WEB_PROV_FAILED,        /* the join failed (wrong password, out of range, timeout) */
};

/* Raise the setup AP and start serving the captive page. Returns false if the radio
 * could not be claimed or the AP could not start. Safe to call when already active. */
bool web_provision_start(void);

/* Tear the setup AP down and stop serving. Safe to call when not active. */
void web_provision_stop(void);

bool web_provision_active(void);
web_prov_state_t web_provision_state(void);

/* The setup AP's name and PIN (its WPA2 passphrase), and the address the page lives at,
 * for the watch to show on screen. Valid while active. */
const char *web_provision_ap_ssid(void);
const char *web_provision_ap_psk(void);
const char *web_provision_ap_ip(void);

/* The network being joined / just joined (empty until the phone picks one). */
const char *web_provision_target_ssid(void);

/* Captive interception, called from web_api_handle() before the pairing gate. Serves the
 * setup page for any page request and handles the /prov/* endpoints while active.
 * Returns true when it answered the request. */
bool web_provision_captive(web_req_t *r);

#endif /* FACTORY_WEB */
