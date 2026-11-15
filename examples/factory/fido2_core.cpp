/**
 * @file fido2_core.cpp
 * @brief Bounded, platform-independent CTAP2/CTAPHID authenticator core (ES256).
 *
 * Implements the CTAP2 command set a security key needs to register and
 * authenticate with a real browser/relying party: authenticatorGetInfo,
 * authenticatorMakeCredential, authenticatorGetAssertion (+GetNextAssertion),
 * authenticatorCredentialManagement, authenticatorClientPIN (pinProtocol 1)
 * and authenticatorReset, plus CTAPHID report framing.
 *
 * Crypto is delegated to fido2_crypto_* (mbedTLS on Arduino, OpenSSL in the
 * host conformance test); CBOR to fido2_cbor_*. No browser, USB or network
 * code lives here: the caller owns storage paths, user-presence prompts and
 * the HID sink.
 *
 * Credentials and the PIN hash live in the caller-supplied internal directory
 * (never removable SD) as a single versioned file written atomically. Private
 * keys are kept in that file; there is no secure element, so this is
 * protection at rest, not hardware key wrapping.
 */
#include "fido2_core.h"
#include "fido2_cbor.h"
#include "fido2_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>

/* ------------------------------------------------------------------ CTAP2 */

/* Command bytes (request[0]). */
enum {
    CMD_MAKE_CREDENTIAL = 0x01,
    CMD_GET_ASSERTION = 0x02,
    CMD_GET_INFO = 0x04,
    CMD_CLIENT_PIN = 0x06,
    CMD_RESET = 0x07,
    CMD_GET_NEXT_ASSERTION = 0x08,
    CMD_CREDENTIAL_MANAGEMENT = 0x09,
};

/* Credential management sub-commands. */
enum {
    CM_METADATA = 0x01,
    CM_ENUM_RPS_BEGIN = 0x02,
    CM_ENUM_RPS_NEXT = 0x03,
    CM_ENUM_CREDS_BEGIN = 0x04,
    CM_ENUM_CREDS_NEXT = 0x05,
    CM_DELETE = 0x06,
};

/* Client PIN sub-commands. */
enum {
    PIN_GET_RETRIES = 0x01,
    PIN_GET_KEY_AGREEMENT = 0x02,
    PIN_SET = 0x03,
    PIN_CHANGE = 0x04,
    PIN_GET_TOKEN = 0x05,
};

/* Auth-data flags. */
enum {
    FLAG_UP = 0x01,
    FLAG_UV = 0x04,
    FLAG_AT = 0x40,
    FLAG_ED = 0x80,
};

enum { ALG_ES256 = -7, ALG_ECDH_ES_HKDF_256 = -25 };

#define FIDO2_AAGUID_SIZE 16
#define FIDO2_RP_ID_MAX 65
#define FIDO2_USER_ID_MAX 64
#define FIDO2_PIN_RETRIES_MAX 8
#define FIDO2_PIN_MIN 4
#define FIDO2_PIN_MAX 63

static const uint8_t FIDO2_AAGUID[FIDO2_AAGUID_SIZE] = {
    0x6c, 0x69, 0x6c, 0x79, 0x67, 0x6f, 0x6f, 0x00,
    0x00, 0x00, 0x00, 0x77, 0x2d, 0x75, 0x6c, 0x74,
};

/* -------------------------------------------------------------- persistence */

#pragma pack(push, 1)
typedef struct {
    uint8_t credential_id[FIDO2_CREDENTIAL_ID_SIZE];
    uint8_t private_key[32];
    uint8_t pub_x[32];
    uint8_t pub_y[32];
    uint8_t rp_id_hash[32];
    char rp_id[FIDO2_RP_ID_MAX];
    char user_name[FIDO2_RP_ID_MAX];
    char user_display[FIDO2_RP_ID_MAX];
    uint8_t user_id[FIDO2_USER_ID_MAX];
    uint8_t user_id_len;
    uint8_t resident;
    uint8_t cred_protect;
    uint8_t reserved;
    uint32_t sign_count;
} fido2_cred_st;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint8_t pin_set;
    uint8_t pin_blocked;
    uint8_t pin_retries;
    uint8_t reserved;
    uint8_t pin_hash[32];
    uint8_t pin_salt[16];
    fido2_cred_st creds[FIDO2_MAX_CREDENTIALS];
} fido2_state_st;
#pragma pack(pop)

#define FIDO2_STATE_MAGIC 0x46324432u /* "F2D2" */

static fido2_state_st s_state;
static fido2_state_st s_load_scratch;
static bool s_ready = false;
static char s_dir[192];
static fido2_user_present_fn s_up = NULL;
static void *s_up_ctx = NULL;
static std::mutex s_mx;

/* Pin-protocol 1 key agreement material (per power cycle, as the spec allows). */
static uint8_t s_ka_priv[32], s_ka_x[32], s_ka_y[32];
static bool s_ka_ready = false;
static uint8_t s_pin_token[32];
static bool s_pin_token_valid = false;

/* GetNextAssertion queue. */
static int s_next_ids[FIDO2_MAX_CREDENTIALS];
static int s_next_count = 0;
static int s_next_cursor = 0;
static uint8_t s_next_client_hash[32];
static bool s_next_valid = false;

/* Credential-management pagination. */
static int s_cm_rp_cursor = 0;
static int s_cm_cred_cursor = 0;
static uint8_t s_cm_rp_hash[32];

static void state_clear(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = FIDO2_STATE_MAGIC;
    s_state.version = 1;
    s_state.pin_retries = FIDO2_PIN_RETRIES_MAX;
}

static void state_path(char *out, size_t cap, const char *suffix)
{
    snprintf(out, cap, "%s/%s", s_dir, suffix);
}

static bool state_load(void)
{
    state_clear();
    char path[256];
    state_path(path, sizeof(path), "fido2.dat");
    FILE *f = fopen(path, "rb");
    if (!f) return true; /* first run: empty store */
    size_t n = fread(&s_load_scratch, 1, sizeof(s_load_scratch), f);
    fclose(f);
    if (n != sizeof(s_load_scratch) || s_load_scratch.magic != FIDO2_STATE_MAGIC ||
        s_load_scratch.version != 1 || s_load_scratch.count > FIDO2_MAX_CREDENTIALS ||
        s_load_scratch.pin_set > 1 || s_load_scratch.pin_blocked > 1) {
        return false; /* corrupt: fail closed, do not trust partial data */
    }
    if (s_load_scratch.pin_retries > FIDO2_PIN_RETRIES_MAX) s_load_scratch.pin_retries = FIDO2_PIN_RETRIES_MAX;
    memcpy(&s_state, &s_load_scratch, sizeof(s_state));
    return true;
}

static bool state_save(void)
{
    char path[256], tmp[264];
    state_path(path, sizeof(path), "fido2.dat");
    state_path(tmp, sizeof(tmp), "fido2.tmp");
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(&s_state, 1, sizeof(s_state), f) == sizeof(s_state) && fflush(f) == 0;
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

/* ------------------------------------------------------------------- helpers */

static fido2_cred_st *cred_by_id(const uint8_t *id, size_t len)
{
    if (len != FIDO2_CREDENTIAL_ID_SIZE) return NULL;
    for (int i = 0; i < s_state.count; i++)
        if (memcmp(s_state.creds[i].credential_id, id, len) == 0) return &s_state.creds[i];
    return NULL;
}

static int cred_index_by_id(const uint8_t *id, size_t len)
{
    if (len != FIDO2_CREDENTIAL_ID_SIZE) return -1;
    for (int i = 0; i < s_state.count; i++)
        if (memcmp(s_state.creds[i].credential_id, id, len) == 0) return i;
    return -1;
}

static bool rp_hash(const char *rp_id, size_t len, uint8_t out[32])
{
    if (!rp_id || len == 0 || len > 64) return false;
    fido2_crypto_sha256((const uint8_t *)rp_id, len, out);
    return true;
}

static void put_cose_es256(fido2_cbor_writer_t *w, const uint8_t x[32], const uint8_t y[32])
{
    fido2_cbor_put_map(w, 5);
    fido2_cbor_put_int(w, 1);  fido2_cbor_put_int(w, 2);   /* kty: EC2 */
    fido2_cbor_put_int(w, 3);  fido2_cbor_put_int(w, ALG_ES256);
    fido2_cbor_put_int(w, -1); fido2_cbor_put_int(w, 1);   /* crv: P-256 */
    fido2_cbor_put_int(w, -2); fido2_cbor_put_bytes(w, x, 32);
    fido2_cbor_put_int(w, -3); fido2_cbor_put_bytes(w, y, 32);
}

static void put_cose_ecdh(fido2_cbor_writer_t *w, const uint8_t x[32], const uint8_t y[32])
{
    fido2_cbor_put_map(w, 5);
    fido2_cbor_put_int(w, 1);  fido2_cbor_put_int(w, 2);
    fido2_cbor_put_int(w, 3);  fido2_cbor_put_int(w, ALG_ECDH_ES_HKDF_256);
    fido2_cbor_put_int(w, -1); fido2_cbor_put_int(w, 1);
    fido2_cbor_put_int(w, -2); fido2_cbor_put_bytes(w, x, 32);
    fido2_cbor_put_int(w, -3); fido2_cbor_put_bytes(w, y, 32);
}

static size_t build_auth_data(uint8_t *out, size_t cap, const uint8_t rp[32], uint8_t flags,
                              uint32_t sign_count, const fido2_cred_st *attested,
                              const uint8_t *ext, size_t ext_len)
{
    if (cap < 37) return 0;
    size_t n = 0;
    memcpy(out, rp, 32); n = 32;
    out[n++] = flags;
    out[n++] = (uint8_t)(sign_count >> 24);
    out[n++] = (uint8_t)(sign_count >> 16);
    out[n++] = (uint8_t)(sign_count >> 8);
    out[n++] = (uint8_t)(sign_count);
    if (flags & FLAG_AT) {
        if (!attested || cap < n + 16 + 2 + FIDO2_CREDENTIAL_ID_SIZE) return 0;
        memcpy(out + n, FIDO2_AAGUID, FIDO2_AAGUID_SIZE); n += FIDO2_AAGUID_SIZE;
        out[n++] = 0; out[n++] = FIDO2_CREDENTIAL_ID_SIZE;
        memcpy(out + n, attested->credential_id, FIDO2_CREDENTIAL_ID_SIZE);
        n += FIDO2_CREDENTIAL_ID_SIZE;
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out + n, cap - n);
        put_cose_es256(&w, attested->pub_x, attested->pub_y);
        if (w.overflow) return 0;
        n += w.len;
    }
    if (ext && ext_len) {
        if (cap < n + ext_len) return 0;
        memcpy(out + n, ext, ext_len); n += ext_len;
    }
    return n;
}

static bool approve(const char *operation, const char *rp_id)
{
    if (!s_up) return false;
    return s_up(s_up_ctx, operation, rp_id) == 1;
}

/* --------------------------------------------------------------- getInfo */

static long cmd_get_info(uint8_t *out, size_t cap)
{
    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, out, cap);
    fido2_cbor_put_map(&w, 7);
    fido2_cbor_put_int(&w, 0x01);
    fido2_cbor_put_array(&w, 1); fido2_cbor_put_text(&w, "FIDO_2_0");
    fido2_cbor_put_int(&w, 0x02);
    fido2_cbor_put_array(&w, 1); fido2_cbor_put_text(&w, "credProtect");
    fido2_cbor_put_int(&w, 0x03);
    fido2_cbor_put_bytes(&w, FIDO2_AAGUID, FIDO2_AAGUID_SIZE);
    fido2_cbor_put_int(&w, 0x04);
    fido2_cbor_put_map(&w, 5);
    fido2_cbor_put_text(&w, "plat"); fido2_cbor_put_bool(&w, false);
    /* Discoverable credentials need PIN-protected access; only advertise rk once a PIN exists. */
    fido2_cbor_put_text(&w, "rk"); fido2_cbor_put_bool(&w, s_state.pin_set != 0);
    fido2_cbor_put_text(&w, "up"); fido2_cbor_put_bool(&w, true);
    fido2_cbor_put_text(&w, "uv"); fido2_cbor_put_bool(&w, false);
    fido2_cbor_put_text(&w, "clientPin"); fido2_cbor_put_bool(&w, s_state.pin_set != 0);
    fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_uint(&w, 1024);
    fido2_cbor_put_int(&w, 0x06);
    fido2_cbor_put_array(&w, 1); fido2_cbor_put_uint(&w, 1);
    fido2_cbor_put_int(&w, 0x0A);
    fido2_cbor_put_array(&w, 1);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_text(&w, "type"); fido2_cbor_put_text(&w, "public-key");
    fido2_cbor_put_text(&w, "alg"); fido2_cbor_put_int(&w, ALG_ES256);
    return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
}

/* ------------------------------------------------------- makeCredential */

static bool params_support_es256(const fido2_cbor_item_t *params)
{
    if (!params || params->major != 4) return false;
    for (size_t i = 0; i < params->value && i < 16; i++) {
        fido2_cbor_item_t entry;
        if (!fido2_cbor_array_at(params, i, &entry) || entry.major != 5) continue;
        fido2_cbor_item_t type_item, alg_item;
        const char *type = NULL; size_t type_len = 0;
        int64_t alg = 0;
        if (!fido2_cbor_map_find_text(&entry, "type", &type_item) ||
            !fido2_cbor_text(&type_item, &type, &type_len)) continue;
        if (!fido2_cbor_map_find_text(&entry, "alg", &alg_item) || !fido2_cbor_int(&alg_item, &alg)) continue;
        if (alg == ALG_ES256 && type_len == 10 && memcmp(type, "public-key", 10) == 0) return true;
    }
    return false;
}

static bool exclude_list_hit(const fido2_cbor_item_t *list)
{
    if (!list || list->major != 4) return false;
    for (size_t i = 0; i < list->value && i < 16; i++) {
        fido2_cbor_item_t entry, id_item;
        const uint8_t *id = NULL; size_t id_len = 0;
        if (!fido2_cbor_array_at(list, i, &entry) || entry.major != 5) continue;
        if (!fido2_cbor_map_find(&entry, 0x01, &id_item) || !fido2_cbor_bytes(&id_item, &id, &id_len)) continue;
        if (cred_by_id(id, id_len)) return true;
    }
    return false;
}

static bool verify_platform_pin_auth(const fido2_cbor_item_t *req, int key, const uint8_t *message)
{
    fido2_cbor_item_t item;
    const uint8_t *auth = NULL; size_t auth_len = 0;
    bool present = fido2_cbor_map_find(req, key, &item) && fido2_cbor_bytes(&item, &auth, &auth_len);
    if (!present) return true; /* no PIN assertion supplied */
    if (auth_len != 16 || !s_pin_token_valid) return false;
    uint8_t mac[32];
    fido2_crypto_hmac(s_pin_token, message, 32, mac);
    return memcmp(mac, auth, 16) == 0;
}

static long cmd_make_credential(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    fido2_cbor_item_t req_map;
    const uint8_t *cursor = req;
    if (!fido2_cbor_read(&cursor, req + len, &req_map) || req_map.major != 5) return -FIDO2_ERR_INVALID_CBOR;

    fido2_cbor_item_t item;
    const uint8_t *client_hash = NULL; size_t client_len = 0;
    if (!fido2_cbor_map_find(&req_map, 0x01, &item) || !fido2_cbor_bytes(&item, &client_hash, &client_len) ||
        client_len != 32) return -FIDO2_ERR_INVALID_PARAMETER;

    fido2_cbor_item_t rp_item, rp_id_item;
    const char *rp_id = NULL; size_t rp_len = 0;
    if (!fido2_cbor_map_find(&req_map, 0x02, &rp_item) || rp_item.major != 5 ||
        !fido2_cbor_map_find(&rp_item, 0x01, &rp_id_item) ||
        !fido2_cbor_text(&rp_id_item, &rp_id, &rp_len) || rp_len == 0 || rp_len > 64)
        return -FIDO2_ERR_INVALID_PARAMETER;

    fido2_cbor_item_t user_item, uid_item, uname_item, udisp_item;
    const uint8_t *user_id = NULL; size_t user_id_len = 0;
    const char *user_name = "", *user_display = "";
    size_t name_len = 0, display_len = 0;
    if (!fido2_cbor_map_find(&req_map, 0x03, &user_item) || user_item.major != 5 ||
        !fido2_cbor_map_find(&user_item, 0x01, &uid_item) ||
        !fido2_cbor_bytes(&uid_item, &user_id, &user_id_len) ||
        user_id_len == 0 || user_id_len > FIDO2_USER_ID_MAX)
        return -FIDO2_ERR_INVALID_PARAMETER;
    if (fido2_cbor_map_find(&user_item, 0x02, &uname_item)) fido2_cbor_text(&uname_item, &user_name, &name_len);
    if (fido2_cbor_map_find(&user_item, 0x03, &udisp_item)) fido2_cbor_text(&udisp_item, &user_display, &display_len);

    fido2_cbor_item_t params;
    if (!fido2_cbor_map_find(&req_map, 0x04, &params) || !params_support_es256(&params))
        return -FIDO2_ERR_UNSUPPORTED_ALGORITHM;

    fido2_cbor_item_t exclude;
    if (fido2_cbor_map_find(&req_map, 0x05, &exclude) && exclude_list_hit(&exclude))
        return -FIDO2_ERR_CREDENTIAL_EXCLUDED;

    bool resident = false;
    fido2_cbor_item_t options;
    if (fido2_cbor_map_find(&req_map, 0x07, &options) && options.major == 5) {
        fido2_cbor_item_t rk;
        if (fido2_cbor_map_find_text(&options, "rk", &rk)) fido2_cbor_bool(&rk, &resident);
    }
    if (resident && !s_state.pin_set) return -FIDO2_ERR_PIN_REQUIRED;
    if (resident && s_state.pin_set) {
        fido2_cbor_item_t pa;
        if (!fido2_cbor_map_find(&req_map, 0x08, &pa)) return -FIDO2_ERR_PIN_REQUIRED;
    }
    if (!verify_platform_pin_auth(&req_map, 0x08, client_hash)) return -FIDO2_ERR_PIN_AUTH_INVALID;

    uint8_t cred_protect = 0;
    uint8_t ext[32];
    fido2_cbor_writer_t extw;
    fido2_cbor_writer_init(&extw, ext, sizeof(ext));
    fido2_cbor_item_t extensions;
    if (fido2_cbor_map_find(&req_map, 0x06, &extensions) && extensions.major == 5) {
        fido2_cbor_item_t cp;
        int64_t cp_value = 0;
        if (fido2_cbor_map_find_text(&extensions, "credProtect", &cp) && fido2_cbor_int(&cp, &cp_value) &&
            cp_value >= 1 && cp_value <= 3) {
            cred_protect = (uint8_t)cp_value;
            fido2_cbor_put_map(&extw, 1);
            fido2_cbor_put_text(&extw, "credProtect");
            fido2_cbor_put_int(&extw, cp_value);
        }
    }

    if (s_state.count >= FIDO2_MAX_CREDENTIALS) return -FIDO2_ERR_KEY_STORE_FULL;
    if (!approve("register", rp_id)) return -FIDO2_ERR_OPERATION_DENIED;

    fido2_cred_st cred;
    memset(&cred, 0, sizeof(cred));
    if (!fido2_crypto_generate_p256(cred.private_key, cred.pub_x, cred.pub_y) ||
        !fido2_crypto_random(cred.credential_id, FIDO2_CREDENTIAL_ID_SIZE))
        return -FIDO2_ERR_PROCESSING;
    if (!rp_hash(rp_id, rp_len, cred.rp_id_hash)) return -FIDO2_ERR_INVALID_PARAMETER;
    snprintf(cred.rp_id, sizeof(cred.rp_id), "%.*s", (int)rp_len, rp_id);
    snprintf(cred.user_name, sizeof(cred.user_name), "%.*s", (int)(name_len > 64 ? 64 : name_len), user_name);
    snprintf(cred.user_display, sizeof(cred.user_display), "%.*s", (int)(display_len > 64 ? 64 : display_len), user_display);
    memcpy(cred.user_id, user_id, user_id_len);
    cred.user_id_len = (uint8_t)user_id_len;
    cred.resident = resident ? 1 : 0;
    cred.cred_protect = cred_protect;

    uint8_t auth_data[320];
    size_t auth_len = build_auth_data(auth_data, sizeof(auth_data), cred.rp_id_hash,
                                      FLAG_UP | FLAG_AT | (cred_protect ? FLAG_ED : 0), 0, &cred,
                                      cred_protect ? ext : NULL, cred_protect ? extw.len : 0);
    if (!auth_len) return -FIDO2_ERR_PROCESSING;

    s_state.creds[s_state.count++] = cred;
    if (!state_save()) { s_state.count--; return -FIDO2_ERR_PROCESSING; }

    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, out, cap);
    fido2_cbor_put_map(&w, 3);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_text(&w, "none");
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, auth_data, auth_len);
    fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_map(&w, 0);
    return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
}

/* -------------------------------------------------------- getAssertion */

static long emit_assertion(int index, const uint8_t *client_hash, uint8_t *out, size_t cap)
{
    fido2_cred_st *cred = &s_state.creds[index];
    uint8_t auth_data[64];
    size_t auth_len = build_auth_data(auth_data, sizeof(auth_data), cred->rp_id_hash, FLAG_UP,
                                      cred->sign_count, NULL, NULL, 0);
    uint8_t message[96];
    memcpy(message, auth_data, auth_len);
    memcpy(message + auth_len, client_hash, 32);
    uint8_t sig[80];
    size_t sig_len = sizeof(sig);
    if (!fido2_crypto_sign_p256(cred->private_key, message, auth_len + 32, sig, &sig_len))
        return -FIDO2_ERR_PROCESSING;

    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, out, cap);
    fido2_cbor_put_map(&w, 5);
    fido2_cbor_put_int(&w, 0x01);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, cred->credential_id, FIDO2_CREDENTIAL_ID_SIZE);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, "public-key");
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, auth_data, auth_len);
    fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_bytes(&w, sig, sig_len);
    fido2_cbor_put_int(&w, 0x04);
    fido2_cbor_put_map(&w, 3);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, cred->user_id, cred->user_id_len);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, cred->user_name);
    fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_text(&w, cred->user_display);
    fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_uint(&w, (uint64_t)s_next_count);
    return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
}

static long cmd_get_assertion(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    fido2_cbor_item_t req_map;
    const uint8_t *cursor = req;
    if (!fido2_cbor_read(&cursor, req + len, &req_map) || req_map.major != 5) return -FIDO2_ERR_INVALID_CBOR;

    fido2_cbor_item_t item;
    const char *rp_id = NULL; size_t rp_len = 0;
    if (!fido2_cbor_map_find(&req_map, 0x01, &item) || !fido2_cbor_text(&item, &rp_id, &rp_len) ||
        rp_len == 0 || rp_len > 64) return -FIDO2_ERR_INVALID_PARAMETER;

    const uint8_t *client_hash = NULL; size_t client_len = 0;
    if (!fido2_cbor_map_find(&req_map, 0x02, &item) || !fido2_cbor_bytes(&item, &client_hash, &client_len) ||
        client_len != 32) return -FIDO2_ERR_INVALID_PARAMETER;

    uint8_t rp_id_hash[32];
    if (!rp_hash(rp_id, rp_len, rp_id_hash)) return -FIDO2_ERR_INVALID_PARAMETER;
    if (!verify_platform_pin_auth(&req_map, 0x06, client_hash)) return -FIDO2_ERR_PIN_AUTH_INVALID;

    s_next_count = 0;
    s_next_cursor = 0;
    s_next_valid = false;

    fido2_cbor_item_t allow;
    bool have_allow = fido2_cbor_map_find(&req_map, 0x03, &allow) && allow.major == 4 && allow.value > 0;
    if (have_allow) {
        for (size_t i = 0; i < allow.value && s_next_count < FIDO2_MAX_CREDENTIALS; i++) {
            fido2_cbor_item_t entry, id_item;
            const uint8_t *id = NULL; size_t id_len = 0;
            if (!fido2_cbor_array_at(&allow, i, &entry) || entry.major != 5) continue;
            if (!fido2_cbor_map_find(&entry, 0x01, &id_item) || !fido2_cbor_bytes(&id_item, &id, &id_len)) continue;
            int idx = cred_index_by_id(id, id_len);
            if (idx >= 0) s_next_ids[s_next_count++] = idx;
        }
    } else {
        for (int i = 0; i < s_state.count && s_next_count < FIDO2_MAX_CREDENTIALS; i++) {
            if (s_state.creds[i].resident && memcmp(s_state.creds[i].rp_id_hash, rp_id_hash, 32) == 0)
                s_next_ids[s_next_count++] = i;
        }
    }
    if (s_next_count == 0) return -FIDO2_ERR_NO_CREDENTIALS;

    if (!approve("authenticate", rp_id)) return -FIDO2_ERR_OPERATION_DENIED;

    memcpy(s_next_client_hash, client_hash, 32);

    int idx = s_next_ids[0];
    s_state.creds[idx].sign_count++;
    long r = emit_assertion(idx, client_hash, out, cap);
    if (r < 0) return r;
    if (!state_save()) return -FIDO2_ERR_PROCESSING;
    if (s_next_count > 1) { s_next_valid = true; s_next_cursor = 1; }
    return r;
}

static long cmd_get_next_assertion(uint8_t *out, size_t cap)
{
    if (!s_next_valid || s_next_cursor >= s_next_count) return -FIDO2_ERR_NOT_ALLOWED;
    int idx = s_next_ids[s_next_cursor++];
    s_state.creds[idx].sign_count++;
    long r = emit_assertion(idx, s_next_client_hash, out, cap);
    if (r < 0) return r;
    if (s_next_cursor >= s_next_count) s_next_valid = false;
    state_save();
    return r;
}

/* ---------------------------------------------------- credential management */

static void put_rp_map(fido2_cbor_writer_t *w, const fido2_cred_st *cred)
{
    fido2_cbor_put_map(w, 2);
    fido2_cbor_put_int(w, 0x01); fido2_cbor_put_text(w, cred->rp_id);
    fido2_cbor_put_int(w, 0x02); fido2_cbor_put_text(w, cred->rp_id);
}

static long cmd_credential_management(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    fido2_cbor_item_t req_map;
    const uint8_t *cursor = req;
    if (!fido2_cbor_read(&cursor, req + len, &req_map) || req_map.major != 5) return -FIDO2_ERR_INVALID_CBOR;

    fido2_cbor_item_t item;
    int64_t sub = 0;
    if (!fido2_cbor_map_find(&req_map, 0x01, &item) || !fido2_cbor_int(&item, &sub))
        return -FIDO2_ERR_MISSING_PARAMETER;

    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, out, cap);

    if (sub == CM_METADATA) {
        int resident = 0;
        for (int i = 0; i < s_state.count; i++) if (s_state.creds[i].resident) resident++;
        fido2_cbor_put_map(&w, 2);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_uint(&w, (uint64_t)resident);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_uint(&w, (uint64_t)(FIDO2_MAX_CREDENTIALS - resident));
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }

    if (sub == CM_ENUM_RPS_BEGIN || sub == CM_ENUM_RPS_NEXT) {
        if (sub == CM_ENUM_RPS_BEGIN) s_cm_rp_cursor = 0;
        int seen = 0, found = -1, total = 0;
        for (int i = 0; i < s_state.count; i++) {
            bool dup = false;
            for (int j = 0; j < i; j++)
                if (memcmp(s_state.creds[j].rp_id_hash, s_state.creds[i].rp_id_hash, 32) == 0) { dup = true; break; }
            if (dup) continue;
            if (seen == s_cm_rp_cursor) found = i;
            seen++; total++;
        }
        if (found < 0) return -FIDO2_ERR_NO_CREDENTIALS;
        s_cm_rp_cursor++;
        fido2_cbor_put_map(&w, 3);
        fido2_cbor_put_int(&w, 0x03); put_rp_map(&w, &s_state.creds[found]);
        fido2_cbor_put_int(&w, 0x04); fido2_cbor_put_bytes(&w, s_state.creds[found].rp_id_hash, 32);
        fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_uint(&w, (uint64_t)total);
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }

    if (sub == CM_ENUM_CREDS_BEGIN || sub == CM_ENUM_CREDS_NEXT) {
        if (sub == CM_ENUM_CREDS_BEGIN) {
            fido2_cbor_item_t params, hash_item;
            const uint8_t *hash = NULL; size_t hash_len = 0;
            if (!fido2_cbor_map_find(&req_map, 0x02, &params) || params.major != 5 ||
                !fido2_cbor_map_find(&params, 0x01, &hash_item) ||
                !fido2_cbor_bytes(&hash_item, &hash, &hash_len) || hash_len != 32)
                return -FIDO2_ERR_MISSING_PARAMETER;
            memcpy(s_cm_rp_hash, hash, 32);
            s_cm_cred_cursor = 0;
        }
        int seen = 0, found = -1, total = 0;
        for (int i = 0; i < s_state.count; i++) {
            if (memcmp(s_state.creds[i].rp_id_hash, s_cm_rp_hash, 32) != 0) continue;
            if (seen == s_cm_cred_cursor) found = i;
            seen++; total++;
        }
        if (found < 0) return -FIDO2_ERR_NO_CREDENTIALS;
        s_cm_cred_cursor++;
        fido2_cred_st *cred = &s_state.creds[found];
        fido2_cbor_put_map(&w, 6);
        fido2_cbor_put_int(&w, 0x06);
        fido2_cbor_put_map(&w, 3);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, cred->user_id, cred->user_id_len);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, cred->user_name);
        fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_text(&w, cred->user_display);
        fido2_cbor_put_int(&w, 0x07); fido2_cbor_put_bytes(&w, cred->credential_id, FIDO2_CREDENTIAL_ID_SIZE);
        fido2_cbor_put_int(&w, 0x08); put_cose_es256(&w, cred->pub_x, cred->pub_y);
        fido2_cbor_put_int(&w, 0x09); fido2_cbor_put_uint(&w, (uint64_t)total);
        fido2_cbor_put_int(&w, 0x0A); fido2_cbor_put_uint(&w, cred->cred_protect);
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }

    if (sub == CM_DELETE) {
        fido2_cbor_item_t params, id_item;
        const uint8_t *id = NULL; size_t id_len = 0;
        if (!fido2_cbor_map_find(&req_map, 0x02, &params) || params.major != 5 ||
            !fido2_cbor_map_find(&params, 0x02, &id_item) ||
            !fido2_cbor_bytes(&id_item, &id, &id_len)) return -FIDO2_ERR_MISSING_PARAMETER;
        int idx = cred_index_by_id(id, id_len);
        if (idx < 0) return -FIDO2_ERR_NO_CREDENTIALS;
        for (int i = idx; i + 1 < s_state.count; i++) s_state.creds[i] = s_state.creds[i + 1];
        s_state.count--;
        if (!state_save()) return -FIDO2_ERR_PROCESSING;
        fido2_cbor_put_map(&w, 0);
        return (long)w.len;
    }

    return -FIDO2_ERR_INVALID_COMMAND;
}

/* ------------------------------------------------------------- client PIN */

static bool pin_key_agreement(void)
{
    if (s_ka_ready) return true;
    if (!fido2_crypto_generate_p256(s_ka_priv, s_ka_x, s_ka_y)) return false;
    s_ka_ready = true;
    return true;
}

static bool pin_shared_token(const uint8_t platform_x[32], const uint8_t platform_y[32], uint8_t token[32])
{
    uint8_t shared[32];
    if (!fido2_crypto_shared_p256(s_ka_priv, platform_x, platform_y, shared)) return false;
    fido2_crypto_sha256(shared, 32, token);
    return true;
}

static bool unpad_pin(const uint8_t *plain, size_t len, char *pin, size_t cap)
{
    size_t n = len;
    while (n > 0 && plain[n - 1] == 0) n--;
    if (n < FIDO2_PIN_MIN || n > FIDO2_PIN_MAX || n + 1 > cap) return false;
    for (size_t i = 0; i < n; i++) {
        if (plain[i] < '0' || plain[i] > '9') return false;
        pin[i] = (char)plain[i];
    }
    pin[n] = 0;
    return true;
}

/* pinProtocol 1 stores SHA-256(pin); the platform proves knowledge with
 * left(16, SHA-256(pin)) encrypted under the pinUvAuthToken. */
static void pin_hash_update(const char *pin)
{
    fido2_crypto_sha256((const uint8_t *)pin, strlen(pin), s_state.pin_hash);
    s_state.pin_set = 1;
    s_state.pin_blocked = 0;
    s_state.pin_retries = FIDO2_PIN_RETRIES_MAX;
}

static bool parse_pin_agreement(const fido2_cbor_item_t *req, uint8_t token[32])
{
    fido2_cbor_item_t ka;
    if (!fido2_cbor_map_find(req, 0x03, &ka) || ka.major != 5) return false;
    fido2_cbor_item_t x_item, y_item;
    const uint8_t *x = NULL, *y = NULL; size_t xl = 0, yl = 0;
    if (!fido2_cbor_map_find(&ka, -2, &x_item) || !fido2_cbor_bytes(&x_item, &x, &xl) || xl != 32) return false;
    if (!fido2_cbor_map_find(&ka, -3, &y_item) || !fido2_cbor_bytes(&y_item, &y, &yl) || yl != 32) return false;
    uint8_t px[32], py[32];
    memcpy(px, x, 32); memcpy(py, y, 32);
    return pin_shared_token(px, py, token);
}

static bool pin_auth_ok(const uint8_t token[32], const fido2_cbor_item_t *req,
                        const uint8_t *msg, size_t msg_len)
{
    fido2_cbor_item_t item;
    const uint8_t *auth = NULL; size_t auth_len = 0;
    if (!fido2_cbor_map_find(req, 0x04, &item) || !fido2_cbor_bytes(&item, &auth, &auth_len) || auth_len != 16)
        return false;
    uint8_t mac[32];
    fido2_crypto_hmac(token, msg, msg_len, mac);
    return memcmp(mac, auth, 16) == 0;
}

static long cmd_client_pin(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    fido2_cbor_item_t req_map;
    const uint8_t *cursor = req;
    if (!fido2_cbor_read(&cursor, req + len, &req_map) || req_map.major != 5) return -FIDO2_ERR_INVALID_CBOR;

    fido2_cbor_item_t item;
    int64_t sub = 0;
    if (!fido2_cbor_map_find(&req_map, 0x02, &item) || !fido2_cbor_int(&item, &sub))
        return -FIDO2_ERR_MISSING_PARAMETER;

    if (sub == PIN_GET_RETRIES) {
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out, cap);
        fido2_cbor_put_map(&w, 1);
        fido2_cbor_put_int(&w, 0x03);
        fido2_cbor_put_uint(&w, s_state.pin_blocked ? 0 : s_state.pin_retries);
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }
    if (sub == PIN_GET_KEY_AGREEMENT) {
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out, cap);
        fido2_cbor_put_map(&w, 1);
        fido2_cbor_put_int(&w, 0x01);
        if (!pin_key_agreement()) return -FIDO2_ERR_PROCESSING;
        put_cose_ecdh(&w, s_ka_x, s_ka_y);
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }

    if (s_state.pin_blocked) return -FIDO2_ERR_PIN_BLOCKED;

    if (sub == PIN_SET) {
        if (s_state.pin_set) return -FIDO2_ERR_NOT_ALLOWED;
        uint8_t token[32];
        if (!parse_pin_agreement(&req_map, token)) return -FIDO2_ERR_INVALID_PARAMETER;
        const uint8_t *enc = NULL; size_t enc_len = 0;
        if (!fido2_cbor_map_find(&req_map, 0x05, &item) || !fido2_cbor_bytes(&item, &enc, &enc_len) ||
            enc_len == 0 || enc_len % 16 || enc_len > 64) return -FIDO2_ERR_INVALID_PARAMETER;
        if (!pin_auth_ok(token, &req_map, enc, enc_len)) return -FIDO2_ERR_PIN_AUTH_INVALID;
        uint8_t zero[16] = {}, plain[64];
        if (!fido2_crypto_aes_cbc(token, zero, false, enc, enc_len, plain)) return -FIDO2_ERR_PROCESSING;
        char pin[64];
        if (!unpad_pin(plain, enc_len, pin, sizeof(pin))) return -FIDO2_ERR_PIN_POLICY_VIOLATION;
        pin_hash_update(pin);
        if (!state_save()) return -FIDO2_ERR_PROCESSING;
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out, cap);
        fido2_cbor_put_map(&w, 0);
        return (long)w.len;
    }

    if (sub == PIN_CHANGE) {
        if (!s_state.pin_set) return -FIDO2_ERR_PIN_NOT_SET;
        uint8_t token[32];
        if (!parse_pin_agreement(&req_map, token)) return -FIDO2_ERR_INVALID_PARAMETER;
        const uint8_t *enc = NULL, *old = NULL; size_t enc_len = 0, old_len = 0;
        if (!fido2_cbor_map_find(&req_map, 0x05, &item) || !fido2_cbor_bytes(&item, &enc, &enc_len) ||
            enc_len == 0 || enc_len % 16 || enc_len > 64) return -FIDO2_ERR_INVALID_PARAMETER;
        if (!fido2_cbor_map_find(&req_map, 0x06, &item) || !fido2_cbor_bytes(&item, &old, &old_len) ||
            old_len != 16) return -FIDO2_ERR_INVALID_PARAMETER;
        uint8_t msg[80];
        memcpy(msg, enc, enc_len); memcpy(msg + enc_len, old, 16);
        if (!pin_auth_ok(token, &req_map, msg, enc_len + 16)) return -FIDO2_ERR_PIN_AUTH_INVALID;
        uint8_t zero[16] = {}, plain[64], old_plain[16];
        if (!fido2_crypto_aes_cbc(token, zero, false, old, 16, old_plain)) return -FIDO2_ERR_PROCESSING;
        if (memcmp(old_plain, s_state.pin_hash, 16) != 0) {
            if (s_state.pin_retries) s_state.pin_retries--;
            if (!s_state.pin_retries) s_state.pin_blocked = 1;
            state_save();
            return -FIDO2_ERR_PIN_INVALID;
        }
        if (!fido2_crypto_aes_cbc(token, zero, false, enc, enc_len, plain)) return -FIDO2_ERR_PROCESSING;
        char pin[64];
        if (!unpad_pin(plain, enc_len, pin, sizeof(pin))) return -FIDO2_ERR_PIN_POLICY_VIOLATION;
        pin_hash_update(pin);
        if (!state_save()) return -FIDO2_ERR_PROCESSING;
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out, cap);
        fido2_cbor_put_map(&w, 0);
        return (long)w.len;
    }

    if (sub == PIN_GET_TOKEN) {
        if (!s_state.pin_set) return -FIDO2_ERR_PIN_NOT_SET;
        uint8_t token[32];
        if (!parse_pin_agreement(&req_map, token)) return -FIDO2_ERR_INVALID_PARAMETER;
        const uint8_t *hash_enc = NULL; size_t hash_len = 0;
        if (!fido2_cbor_map_find(&req_map, 0x06, &item) || !fido2_cbor_bytes(&item, &hash_enc, &hash_len) ||
            hash_len != 16) return -FIDO2_ERR_INVALID_PARAMETER;
        if (!pin_auth_ok(token, &req_map, hash_enc, 16)) return -FIDO2_ERR_PIN_AUTH_INVALID;
        uint8_t zero[16] = {}, plain[16];
        if (!fido2_crypto_aes_cbc(token, zero, false, hash_enc, 16, plain)) return -FIDO2_ERR_PROCESSING;
        if (memcmp(plain, s_state.pin_hash, 16) != 0) {
            if (s_state.pin_retries) s_state.pin_retries--;
            if (!s_state.pin_retries) s_state.pin_blocked = 1;
            state_save();
            return -FIDO2_ERR_PIN_INVALID;
        }
        s_state.pin_retries = FIDO2_PIN_RETRIES_MAX;
        if (!fido2_crypto_random(s_pin_token, sizeof(s_pin_token))) return -FIDO2_ERR_PROCESSING;
        s_pin_token_valid = true;
        state_save();
        fido2_cbor_writer_t w;
        fido2_cbor_writer_init(&w, out, cap);
        fido2_cbor_put_map(&w, 1);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, s_pin_token, sizeof(s_pin_token));
        return w.overflow ? -FIDO2_ERR_REQUEST_TOO_LARGE : (long)w.len;
    }

    return -FIDO2_ERR_INVALID_COMMAND;
}

/* -------------------------------------------------------------- dispatch */

size_t fido2_handle(const uint8_t *request, size_t request_len, uint8_t *out, size_t out_cap)
{
    if (!s_ready || !request || request_len < 1 || !out || out_cap < 2) return 0;
    std::lock_guard<std::mutex> lk(s_mx);
    const uint8_t *payload = request + 1;
    size_t payload_len = request_len - 1;
    long r;
    switch (request[0]) {
        case CMD_GET_INFO: r = cmd_get_info(out + 1, out_cap - 1); break;
        case CMD_MAKE_CREDENTIAL: r = cmd_make_credential(payload, payload_len, out + 1, out_cap - 1); break;
        case CMD_GET_ASSERTION: r = cmd_get_assertion(payload, payload_len, out + 1, out_cap - 1); break;
        case CMD_GET_NEXT_ASSERTION: r = cmd_get_next_assertion(out + 1, out_cap - 1); break;
        case CMD_CREDENTIAL_MANAGEMENT: r = cmd_credential_management(payload, payload_len, out + 1, out_cap - 1); break;
        case CMD_CLIENT_PIN: r = cmd_client_pin(payload, payload_len, out + 1, out_cap - 1); break;
        case CMD_RESET:
            state_clear();
            s_pin_token_valid = false;
            if (!state_save()) { r = -FIDO2_ERR_PROCESSING; break; }
            out[1] = 0xA0; /* empty CBOR map */
            r = 1;
            break;
        default: r = -FIDO2_ERR_INVALID_COMMAND; break;
    }
    if (r < 0) { out[0] = (uint8_t)(-r); return 1; }
    out[0] = FIDO2_OK;
    return 1 + (size_t)r;
}

/* --------------------------------------------------------------- lifecycle */

bool fido2_init(const char *storage_dir, fido2_user_present_fn on_user_present, void *ui_ctx)
{
    if (!storage_dir || !*storage_dir) return false;
    std::lock_guard<std::mutex> lk(s_mx);
    snprintf(s_dir, sizeof(s_dir), "%s", storage_dir);
    mkdir(s_dir, 0700);
    if (!state_load()) return false;
    s_up = on_user_present;
    s_up_ctx = ui_ctx;
    s_ka_ready = s_pin_token_valid = s_next_valid = false;
    s_ready = true;
    return true;
}

void fido2_deinit(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_ready) state_save();
    s_ready = false;
    memset(s_pin_token, 0, sizeof(s_pin_token));
    s_pin_token_valid = false;
}

bool fido2_initialized(void) { return s_ready; }

int fido2_credential_count(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    return s_ready ? s_state.count : 0;
}

int fido2_credential_at(int index, fido2_credential_info_t *out)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_ready || !out || index < 0 || index >= s_state.count) return -1;
    const fido2_cred_st *c = &s_state.creds[index];
    memset(out, 0, sizeof(*out));
    snprintf(out->rp_id, sizeof(out->rp_id), "%s", c->rp_id);
    snprintf(out->user_name, sizeof(out->user_name), "%s", c->user_name);
    memcpy(out->credential_id, c->credential_id, FIDO2_CREDENTIAL_ID_SIZE);
    out->credential_id_len = FIDO2_CREDENTIAL_ID_SIZE;
    out->cred_protect = c->cred_protect;
    out->sign_count = c->sign_count;
    return 0;
}

bool fido2_remove_credential_at(int index)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_ready || index < 0 || index >= s_state.count) return false;
    for (int i = index; i + 1 < s_state.count; i++) s_state.creds[i] = s_state.creds[i + 1];
    s_state.count--;
    return state_save();
}

bool fido2_reset_authenticator(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_ready) return false;
    state_clear();
    s_pin_token_valid = false;
    return state_save();
}

bool fido2_pin_is_set(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    return s_ready && s_state.pin_set;
}

int fido2_pin_retries(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_ready || s_state.pin_blocked) return 0;
    return s_state.pin_retries;
}

bool fido2_pin_is_blocked(void)
{
    std::lock_guard<std::mutex> lk(s_mx);
    return s_ready && s_state.pin_blocked;
}

/* --------------------------------------------------------- management helper */

size_t fido2_make_management_request(uint8_t command, const char *rp_id, const char *user_name,
                                     uint8_t *out, size_t out_cap)
{
    if (!out || out_cap < 2 || !rp_id || !*rp_id) return 0;
    uint8_t client_hash[32];
    if (!fido2_crypto_random(client_hash, sizeof(client_hash))) return 0;
    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, out + 1, out_cap - 1);
    out[0] = command;

    if (command == CMD_MAKE_CREDENTIAL) {
        uint8_t user_id[32];
        fido2_crypto_sha256((const uint8_t *)(user_name ? user_name : ""), strlen(user_name ? user_name : ""), user_id);
        fido2_cbor_put_map(&w, 6);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, client_hash, 32);
        fido2_cbor_put_int(&w, 0x02);
        fido2_cbor_put_map(&w, 2);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_text(&w, rp_id);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, rp_id);
        fido2_cbor_put_int(&w, 0x03);
        fido2_cbor_put_map(&w, 3);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, user_id, 32);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, user_name ? user_name : "");
        fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_text(&w, user_name ? user_name : "");
        fido2_cbor_put_int(&w, 0x04);
        fido2_cbor_put_array(&w, 1);
        fido2_cbor_put_map(&w, 2);
        fido2_cbor_put_text(&w, "type"); fido2_cbor_put_text(&w, "public-key");
        fido2_cbor_put_text(&w, "alg"); fido2_cbor_put_int(&w, ALG_ES256);
        fido2_cbor_put_int(&w, 0x06);
        fido2_cbor_put_map(&w, 1);
        fido2_cbor_put_text(&w, "credProtect"); fido2_cbor_put_int(&w, 2);
        fido2_cbor_put_int(&w, 0x07);
        fido2_cbor_put_map(&w, 2);
        fido2_cbor_put_text(&w, "rk"); fido2_cbor_put_bool(&w, false);
        fido2_cbor_put_text(&w, "up"); fido2_cbor_put_bool(&w, true);
    } else if (command == CMD_GET_ASSERTION) {
        /* Offer every credential stored for this RP so the request can succeed
         * without the caller knowing the credential id. */
        uint8_t rh[32];
        int matches[FIDO2_MAX_CREDENTIALS], count = 0;
        if (rp_hash(rp_id, strlen(rp_id), rh)) {
            for (int i = 0; i < s_state.count && count < FIDO2_MAX_CREDENTIALS; i++)
                if (memcmp(s_state.creds[i].rp_id_hash, rh, 32) == 0) matches[count++] = i;
        }
        fido2_cbor_put_map(&w, 4);
        fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_text(&w, rp_id);
        fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, client_hash, 32);
        fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_array(&w, (size_t)count);
        for (int i = 0; i < count; i++) {
            fido2_cbor_put_map(&w, 2);
            fido2_cbor_put_int(&w, 0x01);
            fido2_cbor_put_bytes(&w, s_state.creds[matches[i]].credential_id, FIDO2_CREDENTIAL_ID_SIZE);
            fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, "public-key");
        }
        fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_map(&w, 0);
    } else {
        return 0;
    }
    if (w.overflow) return 0;
    return 1 + w.len;
}

/* ------------------------------------------------------------- CTAPHID */

enum {
    HID_PING = 0x01, HID_LOCK = 0x04, HID_INIT = 0x06, HID_WINK = 0x08,
    HID_CBOR = 0x10, HID_CANCEL = 0x11, HID_KEEPALIVE = 0x3B, HID_ERROR = 0x3F,
};
#define HID_BROADCAST 0xFFFFFFFFu

static fido2_hid_sink_fn s_hid_sink = NULL;
static void *s_hid_ctx = NULL;
static uint32_t s_hid_cid = 0;
static bool s_hid_cid_valid = false;
static bool s_hid_busy = false;
static uint8_t s_hid_req[FIDO2_MAX_MESSAGE];
static size_t s_hid_expected = 0, s_hid_filled = 0;
static uint8_t s_hid_seq = 0;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static bool hid_send(uint32_t cid, uint8_t type, const uint8_t *data, size_t len)
{
    if (!s_hid_sink) return false;
    uint8_t report[FIDO2_HID_REPORT];
    memset(report, 0, sizeof(report));
    wr32(report, cid);
    report[4] = type;
    report[5] = (uint8_t)(len >> 8);
    report[6] = (uint8_t)len;
    size_t chunk = len > FIDO2_HID_REPORT - 7 ? FIDO2_HID_REPORT - 7 : len;
    memcpy(report + 7, data, chunk);
    if (!s_hid_sink(report, s_hid_ctx)) return false;
    size_t sent = chunk;
    uint8_t seq = 0;
    while (sent < len) {
        memset(report, 0, sizeof(report));
        wr32(report, cid);
        report[4] = 0x00;
        report[5] = seq++;
        chunk = len - sent > FIDO2_HID_REPORT - 5 ? FIDO2_HID_REPORT - 5 : len - sent;
        memcpy(report + 5, data + sent, chunk);
        if (!s_hid_sink(report, s_hid_ctx)) return false;
        sent += chunk;
    }
    return true;
}

static void hid_error(uint32_t cid, uint8_t code)
{
    uint8_t b = code;
    hid_send(cid, HID_ERROR, &b, 1);
}

static void hid_process_cbor(uint32_t cid)
{
    uint8_t response[FIDO2_MAX_MESSAGE];
    memset(response, 0, sizeof(response));
    size_t n = fido2_handle(s_hid_req, s_hid_filled, response, sizeof(response));
    if (n == 0) { hid_error(cid, 0x01); return; }
    hid_send(cid, HID_CBOR, response, n);
}

void fido2_hid_init(fido2_hid_sink_fn sink, void *sink_ctx)
{
    s_hid_sink = sink;
    s_hid_ctx = sink_ctx;
    s_hid_busy = false;
    s_hid_cid_valid = false;
    s_hid_expected = s_hid_filled = 0;
}

void fido2_hid_reset(void)
{
    s_hid_busy = false;
    s_hid_cid_valid = false;
    s_hid_expected = s_hid_filled = 0;
}

bool fido2_hid_busy(void) { return s_hid_busy; }

void fido2_hid_keepalive(uint8_t status)
{
    if (!s_hid_sink || !s_hid_cid_valid) return;
    hid_send(s_hid_cid, HID_KEEPALIVE, &status, 1);
}

void fido2_hid_feed(const uint8_t report[FIDO2_HID_REPORT])
{
    if (!report || !s_hid_sink) return;
    uint32_t cid = rd32(report);
    uint8_t type = report[4];

    if (type == HID_INIT) {
        uint8_t reply[17];
        memcpy(reply, report + 7, 8);
        uint32_t reply_cid = s_hid_cid_valid ? cid : 0;
        if (!s_hid_cid_valid) {
            uint8_t rnd[4];
            if (!fido2_crypto_random(rnd, sizeof(rnd))) wr32(rnd, 1);
            reply_cid = (rd32(rnd) | 0x10000000u) & 0x7FFFFFFFu;
            if (reply_cid == HID_BROADCAST || reply_cid == 0) reply_cid = 0x10000001u;
            s_hid_cid = reply_cid;
            s_hid_cid_valid = true;
        }
        wr32(reply + 8, reply_cid);
        reply[12] = 2;    /* CTAPHID protocol version */
        reply[13] = 1;    /* device version major */
        reply[14] = 0;    /* device version minor */
        reply[15] = 0;    /* device version build */
        reply[16] = 0x0D; /* WINK | CBOR | NMSG */
        hid_send(reply_cid, HID_INIT, reply, sizeof(reply));
        return;
    }

    if (!s_hid_cid_valid || cid != s_hid_cid) return; /* unknown channel */

    if (type == HID_PING) { hid_send(cid, HID_PING, report + 7, FIDO2_HID_REPORT - 7); return; }
    if (type == HID_WINK || type == HID_LOCK) return;
    if (type == HID_CANCEL) { s_hid_busy = false; s_hid_expected = s_hid_filled = 0; return; }
    if (type == HID_CBOR) {
        size_t bcnt = ((size_t)report[5] << 8) | report[6];
        if (bcnt == 0 || bcnt > sizeof(s_hid_req)) { hid_error(cid, 0x03); return; }
        s_hid_expected = bcnt;
        s_hid_filled = 0;
        s_hid_seq = 0;
        size_t chunk = bcnt > FIDO2_HID_REPORT - 7 ? FIDO2_HID_REPORT - 7 : bcnt;
        memcpy(s_hid_req, report + 7, chunk);
        s_hid_filled = chunk;
        if (s_hid_filled == s_hid_expected) { hid_process_cbor(cid); return; }
        s_hid_busy = true;
        return;
    }
    if (type == 0x00) { /* continuation */
        if (!s_hid_busy || report[5] != s_hid_seq) { hid_error(cid, 0x04); s_hid_busy = false; return; }
        s_hid_seq++;
        size_t want = s_hid_expected - s_hid_filled;
        size_t chunk = want > FIDO2_HID_REPORT - 5 ? FIDO2_HID_REPORT - 5 : want;
        memcpy(s_hid_req + s_hid_filled, report + 5, chunk);
        s_hid_filled += chunk;
        if (s_hid_filled == s_hid_expected) { s_hid_busy = false; hid_process_cbor(cid); }
        return;
    }
}
