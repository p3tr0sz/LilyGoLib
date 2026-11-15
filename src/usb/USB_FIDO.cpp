/**
 * @file      USB_FIDO.cpp
 * @brief     USB CTAPHID (FIDO2 authenticator) HID transport.
 *
 * Exposes the FIDO Alliance HID usage page (0xF1D0) with the 64-byte
 * interrupt IN/OUT reports CTAPHID expects. The protocol core lives in the
 * application (fido2_core.cpp); this file only moves reports.
 *
 * Build with -DLILYGO_USB_FIDO_HID and ARDUINO_USB_MODE=0 (TinyUSB). The
 * Arduino-ESP32 USBHID driver merges every registered HID device into one
 * interface, so this personality and the USB keyboard (Bad USB) are mutually
 * exclusive: USB_MSC.cpp does not register the keyboard in this build.
 */
#include "USB_Service.h"
#include "sdkconfig.h"

#if defined(LILYGO_USB_FIDO_HID) && !ARDUINO_USB_MODE && CONFIG_TINYUSB_ENABLED && CONFIG_TINYUSB_HID_ENABLED

#include <USBHID.h>
#include <string.h>

/* FIDO Alliance usage page, U2F authenticator, 64-byte input + output reports. */
static const uint8_t FIDO_HID_DESCRIPTOR[] = {
    0x06, 0xD0, 0xF1,       /* Usage Page (FIDO Alliance) */
    0x09, 0x01,             /* Usage (U2F Authenticator Device) */
    0xA1, 0x01,             /* Collection (Application) */
    0x09, 0x20,             /*   Usage (Input Report Data) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x40,             /*   Report Count (64) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0x09, 0x21,             /*   Usage (Output Report Data) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x40,             /*   Report Count (64) */
    0x91, 0x02,             /*   Output (Data, Variable, Absolute) */
    0xC0,                   /* End Collection */
};

class LilyGoFidoHidDevice : public USBHIDDevice
{
public:
    LilyGoFidoHidDevice()
    {
        USBHID::addDevice(this, sizeof(FIDO_HID_DESCRIPTOR));
    }

    uint16_t _onGetDescriptor(uint8_t *buffer) override
    {
        memcpy(buffer, FIDO_HID_DESCRIPTOR, sizeof(FIDO_HID_DESCRIPTOR));
        return sizeof(FIDO_HID_DESCRIPTOR);
    }

    void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) override
    {
        (void)report_id;
        if (!s_recv_cb || len < LILYGO_USB_FIDO_REPORT_SIZE) return;
        s_recv_cb(buffer, s_recv_ctx);
    }

    static lilygo_usb_fido_recv_cb s_recv_cb;
    static void *s_recv_ctx;
};

lilygo_usb_fido_recv_cb LilyGoFidoHidDevice::s_recv_cb = NULL;
void *LilyGoFidoHidDevice::s_recv_ctx = NULL;

/* Constructing USBHID enables the single HID interface; constructing the
 * device registers the report descriptor with it. Both run at static init,
 * before USB.begin(), in either order. */
static LilyGoFidoHidDevice s_fido_device;
static USBHID s_fido_hid;
static bool s_fido_begun = false;

void lilygo_usb_fido_hid_prepare()
{
    s_fido_hid.begin(); /* idempotent: USBHID::begin() only creates the semaphores */
}

bool lilygo_usb_fido_available()
{
    return s_fido_begun;
}

bool lilygo_usb_fido_begin(lilygo_usb_fido_recv_cb cb, void *ctx)
{
    LilyGoFidoHidDevice::s_recv_cb = cb;
    LilyGoFidoHidDevice::s_recv_ctx = ctx;
    lilygo_usb_fido_hid_prepare();
    if (lilygo_usb_service_begin()) s_fido_begun = true;
    return s_fido_begun;
}

bool lilygo_usb_fido_send(const uint8_t *report)
{
    if (!s_fido_begun || !report) return false;
    return s_fido_hid.SendReport(0, report, LILYGO_USB_FIDO_REPORT_SIZE, 50);
}

#else /* FIDO HID personality not selected in this build */

void lilygo_usb_fido_hid_prepare() {}
bool lilygo_usb_fido_available() { return false; }
bool lilygo_usb_fido_begin(lilygo_usb_fido_recv_cb cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return false;
}
bool lilygo_usb_fido_send(const uint8_t *report)
{
    (void)report;
    return false;
}

#endif