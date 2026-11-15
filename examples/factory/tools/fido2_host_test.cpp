#if defined(FACTORY_FIDO_HOST_CRYPTO) && defined(FIDO2_HOST_TEST)
/**
 * @file fido2_host_test.cpp
 * @brief Host conformance test for the CTAP2 authenticator core.
 *
 * Builds the core with OpenSSL primitives (FACTORY_FIDO_HOST_CRYPTO) and
 * exercises it the way a platform/browser does: CBOR command framing, the
 * makeCredential -> getAssertion lifecycle (with the assertion signature
 * verified independently through OpenSSL), credential management, the
 * pinProtocol 1 client-PIN handshake, and CTAPHID report fragmentation.
 *
 * Build/run: examples/factory/tools/run_fido2_host_test.sh
 */
#include "../fido2_core.h"
#include "../fido2_cbor.h"
#include "../fido2_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>

static int g_pass = 0, g_fail = 0;

static void check(const char *name, bool ok, const char *detail = "")
{
    if (ok) { g_pass++; printf("  ok   %s\n", name); }
    else { g_fail++; printf("  FAIL %s %s\n", name, detail); }
}

static int up_approve(void *, const char *operation, const char *rp_id)
{
    (void)operation; (void)rp_id;
    return 1;
}

/* ---- independent signature verification (OpenSSL, no fido2_crypto) ---- */

static bool verify_es256(const uint8_t x[32], const uint8_t y[32],
                         const uint8_t *message, size_t message_len,
                         const uint8_t *der_sig, size_t der_len)
{
    uint8_t hash[32];
    SHA256(message, message_len, hash);
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key) return false;
    const EC_GROUP *group = EC_KEY_get0_group(key);
    EC_POINT *point = EC_POINT_new(group);
    BIGNUM *px = BN_bin2bn(x, 32, NULL), *py = BN_bin2bn(y, 32, NULL);
    bool ok = point && px && py && der_len > 0 &&
              EC_POINT_set_affine_coordinates(group, point, px, py, NULL) == 1 &&
              EC_KEY_set_public_key(key, point) == 1 &&
              ECDSA_verify(0, hash, sizeof(hash), der_sig, (int)der_len, key) == 1;
    BN_free(px); BN_free(py); EC_POINT_free(point); EC_KEY_free(key);
    return ok;
}

/* ---- CBOR response readers ---- */

static bool response_ok(const uint8_t *resp, size_t n, fido2_cbor_item_t *map)
{
    if (n < 2 || resp[0] != FIDO2_OK) return false;
    const uint8_t *cursor = resp + 1;
    return fido2_cbor_read(&cursor, resp + n, map) && map->major == 5;
}

static bool get_bytes(const fido2_cbor_item_t *map, int key, const uint8_t **data, size_t *len)
{
    fido2_cbor_item_t item;
    return fido2_cbor_map_find(map, key, &item) && fido2_cbor_bytes(&item, data, len);
}

/* ---- platform-side PIN helper (pinProtocol 1) ---- */

static void derive_token(const uint8_t platform_priv[32], const uint8_t ax[32], const uint8_t ay[32],
                         uint8_t token[32])
{
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *point = EC_POINT_new(EC_KEY_get0_group(key));
    BIGNUM *d = BN_bin2bn(platform_priv, 32, NULL), *px = BN_bin2bn(ax, 32, NULL), *py = BN_bin2bn(ay, 32, NULL);
    uint8_t shared[32];
    EC_KEY_set_private_key(key, d);
    EC_POINT_set_affine_coordinates(EC_KEY_get0_group(key), point, px, py, NULL);
    ECDH_compute_key(shared, sizeof(shared), point, key, NULL);
    SHA256(shared, sizeof(shared), token);
    BN_free(d); BN_free(px); BN_free(py); EC_POINT_free(point); EC_KEY_free(key);
}

/* Builds authenticatorClientPIN (sub) with keyAgreement + optional pinAuth/fields. */
static size_t build_client_pin(uint8_t *buf, size_t cap, int sub,
                               const uint8_t ax[32], const uint8_t ay[32],
                               const uint8_t *new_pin_enc, size_t new_pin_len,
                               const uint8_t *pin_hash_enc,
                               const uint8_t *pin_auth)
{
    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, buf + 1, cap - 1);
    buf[0] = 0x06;
    size_t fields = 3 + (pin_auth ? 1 : 0) + (new_pin_enc ? 1 : 0) + (pin_hash_enc ? 1 : 0);
    fido2_cbor_put_map(&w, fields);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_uint(&w, 1); /* pinUvAuthProtocol */
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_int(&w, sub);
    fido2_cbor_put_int(&w, 0x03);
    fido2_cbor_put_map(&w, 5);
    fido2_cbor_put_int(&w, 1);  fido2_cbor_put_int(&w, 2);
    fido2_cbor_put_int(&w, 3);  fido2_cbor_put_int(&w, -25);
    fido2_cbor_put_int(&w, -1); fido2_cbor_put_int(&w, 1);
    fido2_cbor_put_int(&w, -2); fido2_cbor_put_bytes(&w, ax, 32);
    fido2_cbor_put_int(&w, -3); fido2_cbor_put_bytes(&w, ay, 32);
    if (pin_auth) { fido2_cbor_put_int(&w, 0x04); fido2_cbor_put_bytes(&w, pin_auth, 16); }
    if (new_pin_enc) { fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_bytes(&w, new_pin_enc, new_pin_len); }
    if (pin_hash_enc) { fido2_cbor_put_int(&w, 0x06); fido2_cbor_put_bytes(&w, pin_hash_enc, 16); }
    if (w.overflow) return 0;
    return 1 + w.len;
}

static size_t build_make_credential(uint8_t *buf, size_t cap, const char *rp, const char *user,
                                    const uint8_t client_hash[32], bool resident, int cred_protect,
                                    const uint8_t *pin_auth)
{
    uint8_t user_id[32];
    SHA256((const uint8_t *)user, strlen(user), user_id);
    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, buf + 1, cap - 1);
    buf[0] = 0x01;
    fido2_cbor_put_map(&w, pin_auth ? 7 : 6);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, client_hash, 32);
    fido2_cbor_put_int(&w, 0x02);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_text(&w, rp);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, rp);
    fido2_cbor_put_int(&w, 0x03);
    fido2_cbor_put_map(&w, 3);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, user_id, 32);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, user);
    fido2_cbor_put_int(&w, 0x03); fido2_cbor_put_text(&w, user);
    fido2_cbor_put_int(&w, 0x04);
    fido2_cbor_put_array(&w, 1);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_text(&w, "type"); fido2_cbor_put_text(&w, "public-key");
    fido2_cbor_put_text(&w, "alg"); fido2_cbor_put_int(&w, -7);
    fido2_cbor_put_int(&w, 0x06);
    fido2_cbor_put_map(&w, 1);
    fido2_cbor_put_text(&w, "credProtect"); fido2_cbor_put_int(&w, cred_protect);
    fido2_cbor_put_int(&w, 0x07);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_text(&w, "rk"); fido2_cbor_put_bool(&w, resident);
    fido2_cbor_put_text(&w, "up"); fido2_cbor_put_bool(&w, true);
    if (pin_auth) { fido2_cbor_put_int(&w, 0x08); fido2_cbor_put_bytes(&w, pin_auth, 16); }
    if (w.overflow) return 0;
    return 1 + w.len;
}

/* ---- CTAPHID report capture ---- */

static uint8_t g_reports[64][64];
static int g_report_count = 0;

static bool hid_sink(const uint8_t report[64], void *)
{
    if (g_report_count < 64) memcpy(g_reports[g_report_count], report, 64);
    g_report_count++;
    return true;
}

static uint32_t hid_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------------------------------------------------------------- main */

int main(void)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/fido2_host_test_%d", (int)getpid());
    mkdir(dir, 0700);

    printf("fido2 host conformance test\n");

    /* crypto sanity vectors */
    uint8_t digest[32];
    fido2_crypto_sha256((const uint8_t *)"abc", 3, digest);
    static const uint8_t sha_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    check("sha256 known vector", memcmp(digest, sha_abc, 32) == 0);

    check("init", fido2_init(dir, up_approve, NULL));

    /* getInfo */
    uint8_t resp[2048];
    uint8_t req[2048];
    req[0] = 0x04;
    size_t n = fido2_handle(req, 1, resp, sizeof(resp));
    fido2_cbor_item_t map;
    check("getInfo ok", response_ok(resp, n, &map));
    fido2_cbor_item_t versions, aaguid;
    bool got_versions = fido2_cbor_map_find(&map, 0x01, &versions) && versions.major == 4 && versions.value >= 1;
    bool got_aaguid = fido2_cbor_map_find(&map, 0x03, &aaguid) && aaguid.major == 2 && aaguid.value == 16;
    check("getInfo versions + aaguid", got_versions && got_aaguid);

    /* makeCredential (non-resident, credProtect 2) */
    uint8_t client_hash[32];
    memset(client_hash, 0x11, sizeof(client_hash));
    size_t mreq = build_make_credential(req, sizeof(req), "example.com", "alice", client_hash, false, 2, NULL);
    n = fido2_handle(req, mreq, resp, sizeof(resp));
    fido2_cbor_item_t mc;
    check("makeCredential ok", response_ok(resp, n, &mc));
    const uint8_t *auth_data = NULL; size_t auth_len = 0;
    fido2_cbor_item_t fmt;
    const char *fmt_text = NULL; size_t fmt_len = 0;
    bool fmt_ok = fido2_cbor_map_find(&mc, 0x01, &fmt) && fido2_cbor_text(&fmt, &fmt_text, &fmt_len) &&
                  fmt_len == 4 && memcmp(fmt_text, "none", 4) == 0;
    check("makeCredential fmt none", fmt_ok);
    check("makeCredential authData present", get_bytes(&mc, 0x02, &auth_data, &auth_len) && auth_len > 100);
    if (!auth_data || auth_len < 100) { printf("\ncannot continue: makeCredential failed\n"); fido2_deinit(); return 1; }

    uint8_t want_rp[32];
    SHA256((const uint8_t *)"example.com", 11, want_rp);
    check("authData rpIdHash", auth_len > 37 && memcmp(auth_data, want_rp, 32) == 0);
    check("authData UP|AT|ED", auth_len > 32 && (auth_data[32] & 0xC1) == 0xC1);

    size_t off = 37 + 16;             /* rp hash + flags + signCount + aaguid */
    size_t cred_id_len = ((size_t)auth_data[off] << 8) | auth_data[off + 1];
    uint8_t cred_id[FIDO2_CREDENTIAL_ID_SIZE] = {};
    if (cred_id_len == FIDO2_CREDENTIAL_ID_SIZE) memcpy(cred_id, auth_data + off + 2, cred_id_len);
    check("authData credential id", cred_id_len == FIDO2_CREDENTIAL_ID_SIZE);

    fido2_cbor_item_t cose;
    const uint8_t *cursor = auth_data + off + 2 + cred_id_len;
    bool cose_ok = fido2_cbor_read(&cursor, auth_data + auth_len, &cose) && cose.major == 5;
    fido2_cbor_item_t x_item, y_item, alg_item;
    const uint8_t *pub_x = NULL, *pub_y = NULL; size_t xl = 0, yl = 0;
    int64_t alg = 0;
    bool key_ok = cose_ok && fido2_cbor_map_find(&cose, -2, &x_item) && fido2_cbor_bytes(&x_item, &pub_x, &xl) &&
                  fido2_cbor_map_find(&cose, -3, &y_item) && fido2_cbor_bytes(&y_item, &pub_y, &yl) &&
                  fido2_cbor_map_find(&cose, 3, &alg_item) && fido2_cbor_int(&alg_item, &alg) && alg == -7;
    uint8_t key_x[32] = {}, key_y[32] = {};
    if (key_ok && xl == 32 && yl == 32) { memcpy(key_x, pub_x, 32); memcpy(key_y, pub_y, 32); }
    check("authData COSE ES256 key", key_ok && xl == 32 && yl == 32);

    /* getAssertion and independent signature check */
    memset(req, 0, sizeof(req));
    fido2_cbor_writer_t w;
    fido2_cbor_writer_init(&w, req + 1, sizeof(req) - 1);
    req[0] = 0x02;
    fido2_cbor_put_map(&w, 4);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_text(&w, "example.com");
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, client_hash, 32);
    fido2_cbor_put_int(&w, 0x03);
    fido2_cbor_put_array(&w, 1);
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_bytes(&w, cred_id, cred_id_len);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_text(&w, "public-key");
    fido2_cbor_put_int(&w, 0x05); fido2_cbor_put_map(&w, 0);
    n = fido2_handle(req, 1 + w.len, resp, sizeof(resp));
    fido2_cbor_item_t ga;
    check("getAssertion ok", response_ok(resp, n, &ga));
    const uint8_t *assert_auth = NULL, *signature = NULL;
    size_t assert_len = 0, sig_len = 0;
    const uint8_t *got_id = NULL; size_t got_id_len = 0;
    fido2_cbor_item_t cred_map;
    bool have_cred = fido2_cbor_map_find(&ga, 0x01, &cred_map) && cred_map.major == 5 &&
                     get_bytes(&cred_map, 0x01, &got_id, &got_id_len);
    check("getAssertion credential id", have_cred && got_id_len == cred_id_len && memcmp(got_id, cred_id, cred_id_len) == 0);
    bool have_parts = get_bytes(&ga, 0x02, &assert_auth, &assert_len) && get_bytes(&ga, 0x03, &signature, &sig_len);
    check("getAssertion signCount 1", have_parts && assert_len >= 37 && assert_auth[36] == 1);

    uint8_t message[128];
    memcpy(message, assert_auth, assert_len);
    memcpy(message + assert_len, client_hash, 32);
    check("assertion signature verifies", have_parts && key_ok &&
          verify_es256(key_x, key_y, message, assert_len + 32, signature, sig_len));

    /* credential management */
    memset(req, 0, sizeof(req));
    fido2_cbor_writer_init(&w, req + 1, sizeof(req) - 1);
    req[0] = 0x09;
    fido2_cbor_put_map(&w, 1);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_int(&w, 0x01); /* getCredsMetadata */
    n = fido2_handle(req, 1 + w.len, resp, sizeof(resp));
    fido2_cbor_item_t cm;
    check("credMgmt metadata ok", response_ok(resp, n, &cm));
    fido2_cbor_item_t count_item;
    int64_t existing = -1;
    check("credMgmt metadata count", fido2_cbor_map_find(&cm, 0x01, &count_item) && fido2_cbor_int(&count_item, &existing) && existing == 0);

    memset(req, 0, sizeof(req));
    fido2_cbor_writer_init(&w, req + 1, sizeof(req) - 1);
    req[0] = 0x09;
    fido2_cbor_put_map(&w, 2);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_int(&w, 0x06); /* deleteCredential */
    fido2_cbor_put_int(&w, 0x02);
    fido2_cbor_put_map(&w, 1);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_bytes(&w, cred_id, cred_id_len);
    n = fido2_handle(req, 1 + w.len, resp, sizeof(resp));
    check("credMgmt delete ok", n >= 1 && resp[0] == FIDO2_OK && fido2_credential_count() == 0);

    /* clientPIN: setPIN -> makeCredential(resident) -> getAssertion -> changePIN */
    memset(req, 0, sizeof(req));
    fido2_cbor_writer_init(&w, req + 1, sizeof(req) - 1);
    req[0] = 0x06;
    fido2_cbor_put_map(&w, 3);
    fido2_cbor_put_int(&w, 0x01); fido2_cbor_put_uint(&w, 1);
    fido2_cbor_put_int(&w, 0x02); fido2_cbor_put_int(&w, 2); /* getKeyAgreement */
    fido2_cbor_put_int(&w, 0x03);
    fido2_cbor_put_map(&w, 5);
    fido2_cbor_put_int(&w, 1);  fido2_cbor_put_int(&w, 2);
    fido2_cbor_put_int(&w, 3);  fido2_cbor_put_int(&w, -25);
    fido2_cbor_put_int(&w, -1); fido2_cbor_put_int(&w, 1);
    uint8_t zero_xy[32] = {};
    fido2_cbor_put_int(&w, -2); fido2_cbor_put_bytes(&w, zero_xy, 32);
    fido2_cbor_put_int(&w, -3); fido2_cbor_put_bytes(&w, zero_xy, 32);
    n = fido2_handle(req, 1 + w.len, resp, sizeof(resp));
    fido2_cbor_item_t ka_resp, ka_map;
    const uint8_t *ka_x = NULL, *ka_y = NULL; size_t kaxl = 0, kayl = 0;
    bool ka_ok = response_ok(resp, n, &ka_resp) && fido2_cbor_map_find(&ka_resp, 0x01, &ka_map) &&
                 fido2_cbor_map_find(&ka_map, -2, &x_item) && fido2_cbor_bytes(&x_item, &ka_x, &kaxl) &&
                 fido2_cbor_map_find(&ka_map, -3, &y_item) && fido2_cbor_bytes(&y_item, &ka_y, &kayl);
    check("clientPIN getKeyAgreement", ka_ok && kaxl == 32 && kayl == 32);

    uint8_t plat_priv[32], plat_x[32], plat_y[32], token[32];
    check("platform keypair", fido2_crypto_generate_p256(plat_priv, plat_x, plat_y));
    derive_token(plat_priv, ka_x, ka_y, token);

    uint8_t iv0[16] = {};
    uint8_t pin_plain[16] = {};
    memcpy(pin_plain, "1234", 4);
    uint8_t new_pin_enc[16];
    check("platform encrypts PIN", fido2_crypto_aes_cbc(token, iv0, true, pin_plain, 16, new_pin_enc));
    uint8_t new_pin_mac[32], pin_auth[16];
    fido2_crypto_hmac(token, new_pin_enc, 16, new_pin_mac);
    memcpy(pin_auth, new_pin_mac, 16);
    size_t set_len = build_client_pin(req, sizeof(req), 3, plat_x, plat_y, new_pin_enc, 16, NULL, pin_auth);
    n = fido2_handle(req, set_len, resp, sizeof(resp));
    check("clientPIN setPIN", n >= 1 && resp[0] == FIDO2_OK && fido2_pin_is_set());
    check("clientPIN retries reset", fido2_pin_retries() == 8);

    uint8_t pin_hash_enc[16];
    uint8_t pin_digest[32];
    fido2_crypto_sha256((const uint8_t *)"1234", 4, pin_digest);
    fido2_crypto_aes_cbc(token, iv0, true, pin_digest, 16, pin_hash_enc);
    fido2_crypto_hmac(token, pin_hash_enc, 16, new_pin_mac);
    memcpy(pin_auth, new_pin_mac, 16);
    size_t tok_len = build_client_pin(req, sizeof(req), 5, plat_x, plat_y, NULL, 0, pin_hash_enc, pin_auth);
    n = fido2_handle(req, tok_len, resp, sizeof(resp));
    fido2_cbor_item_t tok_resp;
    const uint8_t *pin_token = NULL; size_t pin_token_len = 0;
    bool token_ok = response_ok(resp, n, &tok_resp) && get_bytes(&tok_resp, 0x02, &pin_token, &pin_token_len);
    check("clientPIN getPINToken", token_ok && pin_token_len == 32);

    /* resident credential requires PIN auth */
    uint8_t res_client[32];
    memset(res_client, 0x22, sizeof(res_client));
    fido2_crypto_hmac(pin_token, res_client, 32, new_pin_mac);
    memcpy(pin_auth, new_pin_mac, 16);
    size_t rreq = build_make_credential(req, sizeof(req), "login.example", "bob", res_client, true, 2, pin_auth);
    n = fido2_handle(req, rreq, resp, sizeof(resp));
    fido2_cbor_item_t rmc;
    check("resident makeCredential with PIN", response_ok(resp, n, &rmc));
    check("credential stored", fido2_credential_count() == 1);
    fido2_credential_info_t info;
    check("credential info", fido2_credential_at(0, &info) == 0 && strcmp(info.rp_id, "login.example") == 0);

    /* wrong PIN is rejected and decrements retries */
    uint8_t bad_digest[16];
    memset(bad_digest, 0x5a, 16);
    fido2_crypto_aes_cbc(token, iv0, true, bad_digest, 16, pin_hash_enc);
    fido2_crypto_hmac(token, pin_hash_enc, 16, new_pin_mac);
    memcpy(pin_auth, new_pin_mac, 16);
    tok_len = build_client_pin(req, sizeof(req), 5, plat_x, plat_y, NULL, 0, pin_hash_enc, pin_auth);
    n = fido2_handle(req, tok_len, resp, sizeof(resp));
    check("wrong PIN rejected", n == 1 && resp[0] == FIDO2_ERR_PIN_INVALID && fido2_pin_retries() == 7);

    /* reset clears everything */
    req[0] = 0x07;
    n = fido2_handle(req, 1, resp, sizeof(resp));
    check("reset", n >= 1 && resp[0] == FIDO2_OK && fido2_credential_count() == 0 && !fido2_pin_is_set());

    /* management-request bridge used by the web/native front-ends */
    size_t mgmt = fido2_make_management_request(0x01, "mgmt.example", "carol", req, sizeof(req));
    check("management register request builds", mgmt > 0);
    n = mgmt ? fido2_handle(req, mgmt, resp, sizeof(resp)) : 0;
    check("management register", n >= 1 && resp[0] == FIDO2_OK && fido2_credential_count() == 1);
    mgmt = fido2_make_management_request(0x02, "mgmt.example", "carol", req, sizeof(req));
    n = mgmt ? fido2_handle(req, mgmt, resp, sizeof(resp)) : 0;
    check("management authenticate", n >= 1 && resp[0] == FIDO2_OK);

    /* CTAPHID: INIT, then a fragmented CBOR request (getInfo) */
    fido2_hid_init(hid_sink, NULL);
    uint8_t report[64] = {};
    report[0] = 0xFF; report[1] = 0xFF; report[2] = 0xFF; report[3] = 0xFF;
    report[4] = 0x06; /* INIT */
    for (int i = 0; i < 8; i++) report[7 + i] = (uint8_t)(i + 1);
    g_report_count = 0;
    fido2_hid_feed(report);
    check("HID init response", g_report_count == 1 && g_reports[0][4] == 0x06 && g_reports[0][7 + 12] == 2);
    uint32_t cid = hid_rd32(g_reports[0] + 7 + 8);
    check("HID allocates channel", cid != 0 && cid != 0xFFFFFFFFu);

    /* getInfo fits in one packet (well under 57 bytes?) - use a PING for framing too */
    uint8_t ping[64] = {};
    ping[0] = cid; ping[1] = cid >> 8; ping[2] = cid >> 16; ping[3] = cid >> 24;
    ping[4] = 0x01;
    for (int i = 0; i < 10; i++) ping[7 + i] = (uint8_t)(0xA0 + i);
    g_report_count = 0;
    fido2_hid_feed(ping);
    check("HID ping echo", g_report_count == 1 && g_reports[0][4] == 0x01 && g_reports[0][7] == 0xA0);

    /* CBOR getInfo over HID (response may fragment) */
    uint8_t cbor[64] = {};
    cbor[0] = cid; cbor[1] = cid >> 8; cbor[2] = cid >> 16; cbor[3] = cid >> 24;
    cbor[4] = 0x10; cbor[5] = 0; cbor[6] = 1; cbor[7] = 0x04;
    g_report_count = 0;
    fido2_hid_feed(cbor);
    check("HID CBOR framed response", g_report_count >= 1 && g_reports[0][4] == 0x10);

    fido2_deinit();
    char path[128];
    snprintf(path, sizeof(path), "%s/fido2.dat", dir);
    remove(path);
    snprintf(path, sizeof(path), "%s/fido2.tmp", dir);
    remove(path);
    rmdir(dir);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
#endif /* FACTORY_FIDO_HOST_CRYPTO && FIDO2_HOST_TEST */
