/**
 * @file      fido2_device.cpp
 * @brief     Watch-side CTAP2 authenticator: internal-flash store, BOOT-button
 *            user presence and the USB CTAPHID transport.
 *
 * The browser talks CTAPHID over USB; the desktop API talks to the same core
 * directly for credential management. Storage is the internal FFat volume
 * (mounted at /ffat or the board's /fs), never removable SD. Keys sit in fido2.dat on that volume: this is
 * protection at rest (the ESP32 has no secure element), not hardware wrapping.
 *
 * User presence is currently the BOOT button (GPIO0) held or pressed within a
 * 15 s window, with CTAPHID keepalives while waiting. A richer on-screen
 * approve/PIN prompt is still future work.
 */
#include "fido2_device.h"

#if defined(ARDUINO) && defined(LILYGO_USB_FIDO_HID)

#include <Arduino.h>
#include <FFat.h>
#include <LilyGoLog.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "fido2_core.h"
#include "usb/USB_Service.h"

static_assert(FIDO2_HID_REPORT == LILYGO_USB_FIDO_REPORT_SIZE,
              "core and USB transport must agree on the 64-byte CTAPHID report");

#define FIDO2_STORAGE_NAME  "fido2"
#define FIDO2_UP_PIN        0
#define FIDO2_UP_TIMEOUT_MS 15000
#define FIDO2_HID_QUEUE     16

static bool s_started = false;
static bool s_ok = false;
static const char *s_error = NULL;
static QueueHandle_t s_hid_queue = NULL;

static bool hid_sink(const uint8_t report[FIDO2_HID_REPORT], void *ctx)
{
    (void)ctx;
    return lilygo_usb_fido_send(report);
}

static void hid_receive(const uint8_t *report, void *ctx)
{
    (void)ctx;
    if (s_hid_queue) xQueueSend(s_hid_queue, report, 0);
}

static void fido2_task(void *arg)
{
    (void)arg;
    uint8_t report[FIDO2_HID_REPORT];
    for (;;) {
        if (xQueueReceive(s_hid_queue, report, portMAX_DELAY) == pdTRUE) {
            fido2_hid_feed(report);
        }
    }
}

/* BOOT (GPIO0) is active low on the supported boards. Keepalive status 0x02 is
 * CTAPHID_UPNEEDED, so the browser keeps the request open while we wait. */
static int device_user_present(void *ctx, const char *operation, const char *rp_id)
{
    (void)ctx;
    LILYGO_LOG_I("FIDO2 %s for %s: press BOOT to approve", operation ? operation : "operation",
                 rp_id && *rp_id ? rp_id : "relying party");
    pinMode(FIDO2_UP_PIN, INPUT_PULLUP);
    uint32_t start = millis();
    while (millis() - start < FIDO2_UP_TIMEOUT_MS) {
        if (digitalRead(FIDO2_UP_PIN) == LOW) {
            delay(25);
            if (digitalRead(FIDO2_UP_PIN) != LOW) continue;
            LILYGO_LOG_I("FIDO2 user presence approved");
            while (digitalRead(FIDO2_UP_PIN) == LOW &&
                   millis() - start < FIDO2_UP_TIMEOUT_MS + 2000) {
                delay(25);
            }
            return 1;
        }
        fido2_hid_keepalive(0x02);
        delay(100);
    }
    LILYGO_LOG_W("FIDO2 user presence timed out");
    return 0;
}

/* The board mounts FFat during instance.begin() (setupMSC uses "/fs"); older
 * docs and the web layer use "/ffat". Reuse whichever VFS path is live instead
 * of assuming one, then fall back to mounting it ourselves. */
static const char *flash_mount(void)
{
    static const char *mount = NULL;
    if (mount) return mount;
    struct stat st;
    if (stat("/ffat", &st) == 0 && S_ISDIR(st.st_mode)) {
        mount = "/ffat";
    } else if (stat("/fs", &st) == 0 && S_ISDIR(st.st_mode)) {
        mount = "/fs";
    } else if (FFat.begin(false, "/ffat")) {
        mount = "/ffat";
    }
    return mount;
}

bool fido2_device_begin(void)
{
    if (s_started) return s_ok;
    s_started = true;

    const char *mount = flash_mount();
    if (!mount) {
        s_error = "internal flash (FFat) could not be mounted";
        return false;
    }
    char storage[96];
    snprintf(storage, sizeof(storage), "%s/%s", mount, FIDO2_STORAGE_NAME);
    if (!fido2_init(storage, device_user_present, NULL)) {
        s_error = "FIDO2 credential store could not be opened";
        return false;
    }

    s_hid_queue = xQueueCreate(FIDO2_HID_QUEUE, FIDO2_HID_REPORT);
    if (!s_hid_queue) {
        s_error = "FIDO2 transport queue could not be created";
        return false;
    }
    if (xTaskCreate(fido2_task, "fido2", 10240, NULL, 2, NULL) != pdPASS) {
        s_error = "FIDO2 transport task could not be created";
        return false;
    }

    fido2_hid_init(hid_sink, NULL);
    if (!lilygo_usb_fido_begin(hid_receive, NULL)) {
        s_error = "USB CTAPHID transport did not start";
        return false;
    }

    s_ok = true;
    s_error = NULL;
    LILYGO_LOG_I("FIDO2 USB CTAPHID ready, %d credential(s)", fido2_credential_count());
    return true;
}

bool fido2_device_ready(void)
{
    return s_ok;
}

const char *fido2_device_error(void)
{
    return s_error;
}

#else /* not a FIDO HID build */

bool fido2_device_begin(void) { return false; }
bool fido2_device_ready(void) { return false; }
const char *fido2_device_error(void)
{
    return "This firmware was built without the USB FIDO2 transport";
}

#endif