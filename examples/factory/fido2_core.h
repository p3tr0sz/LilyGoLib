/**
 * @file fido2_core.h
 * @brief Small, bounded CTAP2/CTAPHID authenticator core (ES256).
 *
 * Platform independent protocol/storage logic. Crypto uses mbedTLS on Arduino
 * and OpenSSL in the host conformance test. No browser/network code lives here.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FIDO2_MAX_CREDENTIALS 16
#define FIDO2_MAX_MESSAGE 2048
#define FIDO2_HID_REPORT 64
#define FIDO2_CREDENTIAL_ID_SIZE 32

/* CTAP2 status bytes (subset, values from the public CTAP specification). */
enum {
    FIDO2_OK = 0x00,
    FIDO2_ERR_INVALID_COMMAND = 0x01,
    FIDO2_ERR_INVALID_PARAMETER = 0x02,
    FIDO2_ERR_INVALID_LENGTH = 0x03,
    FIDO2_ERR_INVALID_SEQ = 0x04,
    FIDO2_ERR_TIMEOUT = 0x05,
    FIDO2_ERR_CHANNEL_BUSY = 0x06,
    FIDO2_ERR_INVALID_CHANNEL = 0x0B,
    FIDO2_ERR_CBOR_UNEXPECTED_TYPE = 0x11,
    FIDO2_ERR_INVALID_CBOR = 0x12,
    FIDO2_ERR_MISSING_PARAMETER = 0x14,
    FIDO2_ERR_LIMIT_EXCEEDED = 0x15,
    FIDO2_ERR_CREDENTIAL_EXCLUDED = 0x19,
    FIDO2_ERR_PROCESSING = 0x21,
    FIDO2_ERR_INVALID_CREDENTIAL = 0x22,
    FIDO2_ERR_OPERATION_PENDING = 0x24,
    FIDO2_ERR_UNSUPPORTED_ALGORITHM = 0x26,
    FIDO2_ERR_OPERATION_DENIED = 0x27,
    FIDO2_ERR_KEY_STORE_FULL = 0x28,
    FIDO2_ERR_INVALID_OPTION = 0x2C,
    FIDO2_ERR_NO_CREDENTIALS = 0x2E,
    FIDO2_ERR_USER_ACTION_TIMEOUT = 0x2F,
    FIDO2_ERR_NOT_ALLOWED = 0x30,
    FIDO2_ERR_PIN_INVALID = 0x31,
    FIDO2_ERR_PIN_BLOCKED = 0x32,
    FIDO2_ERR_PIN_AUTH_INVALID = 0x33,
    FIDO2_ERR_PIN_AUTH_BLOCKED = 0x34,
    FIDO2_ERR_PIN_NOT_SET = 0x35,
    FIDO2_ERR_PIN_REQUIRED = 0x36,
    FIDO2_ERR_PIN_POLICY_VIOLATION = 0x37,
    FIDO2_ERR_REQUEST_TOO_LARGE = 0x39,
    FIDO2_ERR_UP_REQUIRED = 0x3B,
    FIDO2_ERR_UV_BLOCKED = 0x3C,
    FIDO2_ERR_UV_INVALID = 0x3F,
};

/* Physical approval callback. Return 1 for approve, 0 for deny/timeout. */
typedef int (*fido2_user_present_fn)(void *ctx, const char *operation, const char *rp_id);

typedef struct {
    char rp_id[65];
    char user_name[65];
    uint8_t credential_id[FIDO2_CREDENTIAL_ID_SIZE];
    uint8_t credential_id_len;
    uint8_t cred_protect;
    uint32_t sign_count;
} fido2_credential_info_t;

/* Initialize/load authenticator. storage_dir must be internal persistent storage,
 * never removable SD. Returns false when storage/crypto cannot be initialized. */
bool fido2_init(const char *storage_dir, fido2_user_present_fn on_user_present, void *ui_ctx);
void fido2_deinit(void);
bool fido2_initialized(void);

/* One CTAP2 command, request[0] is the command byte. Response always starts with
 * a CTAP status byte; CBOR payload follows on success. Returns bytes in out. */
size_t fido2_handle(const uint8_t *request, size_t request_len, uint8_t *out, size_t out_cap);

int fido2_credential_count(void);
int fido2_credential_at(int index, fido2_credential_info_t *out);
bool fido2_remove_credential_at(int index);
bool fido2_reset_authenticator(void);
bool fido2_pin_is_set(void);
int fido2_pin_retries(void);
bool fido2_pin_is_blocked(void);

/* CTAPHID transport. Feed 64-byte reports; complete requests produce fragmented
 * responses through sink. sink returns false when the USB host is unavailable. */
typedef bool (*fido2_hid_sink_fn)(const uint8_t report[FIDO2_HID_REPORT], void *ctx);
void fido2_hid_init(fido2_hid_sink_fn sink, void *sink_ctx);
void fido2_hid_reset(void);
void fido2_hid_feed(const uint8_t report[FIDO2_HID_REPORT]);
void fido2_hid_keepalive(uint8_t status);
bool fido2_hid_busy(void);

/* Test/management helpers for direct native/web management operations. */
size_t fido2_make_management_request(uint8_t command, const char *rp_id, const char *user_name,
                                     uint8_t *out, size_t out_cap);
