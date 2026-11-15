/**
 * @file      fido2_device.h
 * @brief     On-device CTAP2 authenticator wiring (USB CTAPHID + internal flash).
 *
 * Only compiled into builds that select LILYGO_USB_FIDO_HID (TinyUSB FIDO HID
 * personality, ARDUINO_USB_MODE=0). Other builds keep the capability hidden.
 */
#pragma once

#include <stdbool.h>

/** Start storage, the protocol core and the USB CTAPHID transport. Idempotent. */
bool fido2_device_begin(void);

/** True once the core and transport are up (USB may still be enumerating). */
bool fido2_device_ready(void);

/** Human-readable reason when fido2_device_begin() failed, else NULL. */
const char *fido2_device_error(void);