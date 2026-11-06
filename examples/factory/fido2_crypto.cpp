#include "fido2_crypto.h"
#include <string.h>

#if defined(ARDUINO)

#include <mutex>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/bignum.h>

static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_drbg_ready = false;
static std::mutex s_crypto_mx;

static bool random_locked(uint8_t *out, size_t len)
{
    if (!s_drbg_ready) {
        mbedtls_entropy_init(&s_entropy);
        mbedtls_ctr_drbg_init(&s_drbg);
        const char *personal = "twatch-fido2-es256";
        if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                  (const uint8_t *)personal, strlen(personal)) != 0) return false;
        s_drbg_ready = true;
    }
    return mbedtls_ctr_drbg_random(&s_drbg, out, len) == 0;
}

static int random_adapter(void *, unsigned char *out, size_t len)
{
    return random_locked(out, len) ? 0 : -1;
}

bool fido2_crypto_random(uint8_t *out, size_t len)
{
    std::lock_guard<std::mutex> lk(s_crypto_mx);
    return random_locked(out, len);
}

void fido2_crypto_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    mbedtls_sha256_ret(data, len, out, 0);
}

void fido2_crypto_hmac(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t out[32])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, data, len, out);
}

bool fido2_crypto_pbkdf2(const uint8_t *password, size_t password_len,
                         const uint8_t salt[16], uint32_t iterations, uint8_t out[32])
{
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    bool ok = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
              mbedtls_pkcs5_pbkdf2_hmac(&md, password, password_len, salt, 16, iterations, 32, out) == 0;
    mbedtls_md_free(&md);
    return ok;
}

bool fido2_crypto_hkdf(const uint8_t key[32], const char *info, uint8_t out[32])
{
    static const uint8_t salt[32] = {};
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_hkdf(md, salt, sizeof(salt), key, 32,
                             (const uint8_t *)info, strlen(info), out, 32) == 0;
}

bool fido2_crypto_aes_cbc(const uint8_t key[32], const uint8_t iv[16], bool encrypt,
                          const uint8_t *in, size_t len, uint8_t *out)
{
    if (len % 16) return false;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = encrypt ? mbedtls_aes_setkey_enc(&aes, key, 256) : mbedtls_aes_setkey_dec(&aes, key, 256);
    uint8_t chain[16];
    memcpy(chain, iv, 16);
    if (rc == 0) rc = mbedtls_aes_crypt_cbc(&aes, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
                                           len, chain, in, out);
    mbedtls_aes_free(&aes);
    return rc == 0;
}

bool fido2_crypto_aes_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t plain[32], uint8_t encrypted[32], uint8_t tag[16])
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, 32, nonce, 12,
                                               aad, aad_len, plain, encrypted, 16, tag);
    mbedtls_gcm_free(&gcm);
    return rc == 0;
}

bool fido2_crypto_aes_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t encrypted[32], const uint8_t tag[16], uint8_t plain[32])
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) rc = mbedtls_gcm_auth_decrypt(&gcm, 32, nonce, 12, aad, aad_len,
                                              tag, 16, encrypted, plain);
    mbedtls_gcm_free(&gcm);
    return rc == 0;
}

bool fido2_crypto_generate_p256(uint8_t private_key[32], uint8_t x[32], uint8_t y[32])
{
    std::lock_guard<std::mutex> lk(s_crypto_mx);
    mbedtls_ecp_group group;
    mbedtls_ecp_point public_key;
    mbedtls_mpi d;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&public_key);
    mbedtls_mpi_init(&d);
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_gen_keypair(&group, &d, &public_key, random_adapter, NULL) == 0 &&
              mbedtls_mpi_write_binary(&d, private_key, 32) == 0 &&
              mbedtls_mpi_write_binary(&public_key.X, x, 32) == 0 &&
              mbedtls_mpi_write_binary(&public_key.Y, y, 32) == 0;
    mbedtls_ecp_group_free(&group);
    mbedtls_ecp_point_free(&public_key);
    mbedtls_mpi_free(&d);
    return ok;
}

bool fido2_crypto_shared_p256(const uint8_t private_key[32], const uint8_t x[32], const uint8_t y[32], uint8_t shared[32])
{
    std::lock_guard<std::mutex> lk(s_crypto_mx);
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    mbedtls_mpi d, z;
    mbedtls_ecp_group_init(&group); mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&z);
    bool ok = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_mpi_read_binary(&d, private_key, 32) == 0 &&
              mbedtls_mpi_read_binary(&point.X, x, 32) == 0 && mbedtls_mpi_read_binary(&point.Y, y, 32) == 0 &&
              mbedtls_mpi_lset(&point.Z, 1) == 0 &&
              mbedtls_ecdh_compute_shared(&group, &z, &point, &d, random_adapter, NULL) == 0 &&
              mbedtls_mpi_write_binary(&z, shared, 32) == 0;
    mbedtls_ecp_group_free(&group); mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&d); mbedtls_mpi_free(&z);
    return ok;
}

bool fido2_crypto_sign_p256(const uint8_t private_key[32], const uint8_t *data, size_t len,
                            uint8_t *der_signature, size_t *signature_len)
{
    uint8_t hash[32];
    fido2_crypto_sha256(data, len, hash);
    std::lock_guard<std::mutex> lk(s_crypto_mx);
    mbedtls_ecdsa_context keypair;
    mbedtls_ecdsa_init(&keypair);
    bool ok = mbedtls_ecp_group_load(&keypair.grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_mpi_read_binary(&keypair.d, private_key, 32) == 0 &&
              mbedtls_ecdsa_write_signature(&keypair, MBEDTLS_MD_SHA256, hash, sizeof(hash), der_signature,
                                            signature_len, random_adapter, NULL) == 0;
    mbedtls_ecdsa_free(&keypair);
    return ok;
}

#elif defined(FACTORY_FIDO_HOST_CRYPTO)

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/obj_mac.h>
#include <vector>

bool fido2_crypto_random(uint8_t *out, size_t len) { return RAND_bytes(out, (int)len) == 1; }
void fido2_crypto_sha256(const uint8_t *data, size_t len, uint8_t out[32]) { SHA256(data, len, out); }
void fido2_crypto_hmac(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t out[32])
{
    unsigned int n = 32;
    HMAC(EVP_sha256(), key, 32, data, len, out, &n);
}
bool fido2_crypto_pbkdf2(const uint8_t *password, size_t password_len,
                         const uint8_t salt[16], uint32_t iterations, uint8_t out[32])
{
    return PKCS5_PBKDF2_HMAC((const char *)password, (int)password_len, salt, 16, (int)iterations, EVP_sha256(), 32, out) == 1;
}
bool fido2_crypto_hkdf(const uint8_t key[32], const char *info, uint8_t out[32])
{
    uint8_t salt[32] = {}, prk[32], input[128];
    fido2_crypto_hmac(salt, key, 32, prk);
    size_t info_len = strlen(info);
    if (info_len + 1 > sizeof(input)) return false;
    memcpy(input, info, info_len); input[info_len] = 1;
    fido2_crypto_hmac(prk, input, info_len + 1, out);
    return true;
}

static bool aes_evp(const EVP_CIPHER *cipher, const uint8_t key[32], const uint8_t iv[16], bool encrypt,
                    const uint8_t *in, size_t len, uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int a = 0, b = 0;
    bool ok = EVP_CipherInit_ex(ctx, cipher, NULL, key, iv, encrypt ? 1 : 0) == 1 &&
              EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 && len <= INT32_MAX &&
              EVP_CipherUpdate(ctx, out, &a, in, (int)len) == 1 && EVP_CipherFinal_ex(ctx, out + a, &b) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok && (size_t)(a + b) == len;
}
bool fido2_crypto_aes_cbc(const uint8_t key[32], const uint8_t iv[16], bool encrypt,
                          const uint8_t *in, size_t len, uint8_t *out)
{
    return len % 16 == 0 && aes_evp(EVP_aes_256_cbc(), key, iv, encrypt, in, len, out);
}

bool fido2_crypto_aes_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t plain[32], uint8_t encrypted[32], uint8_t tag[16])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int n = 0, total = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
              EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
              (aad_len == 0 || EVP_EncryptUpdate(ctx, NULL, &n, aad, (int)aad_len) == 1) &&
              EVP_EncryptUpdate(ctx, encrypted, &n, plain, 32) == 1;
    total = n;
    ok = ok && EVP_EncryptFinal_ex(ctx, encrypted + total, &n) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool fido2_crypto_aes_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t encrypted[32], const uint8_t tag[16], uint8_t plain[32])
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    int n = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
              EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
              (aad_len == 0 || EVP_DecryptUpdate(ctx, NULL, &n, aad, (int)aad_len) == 1) &&
              EVP_DecryptUpdate(ctx, plain, &n, encrypted, 32) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain + n, &n) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static void bn32(const BIGNUM *bn, uint8_t out[32]) { BN_bn2binpad(bn, out, 32); }
bool fido2_crypto_generate_p256(uint8_t private_key[32], uint8_t x[32], uint8_t y[32])
{
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (!key) return false;
    const EC_GROUP *group = EC_KEY_get0_group(key);
    const BIGNUM *d = NULL;
    const EC_POINT *pub = NULL;
    BIGNUM *px = BN_new(), *py = BN_new();
    bool ok = px && py && EC_KEY_generate_key(key) == 1;
    if (ok) {
        d = EC_KEY_get0_private_key(key); pub = EC_KEY_get0_public_key(key);
        bn32(d, private_key);
        ok = EC_POINT_get_affine_coordinates(group, pub, px, py, NULL) == 1;
        if (ok) { bn32(px, x); bn32(py, y); }
    }
    BN_free(px); BN_free(py); EC_KEY_free(key);
    return ok;
}

bool fido2_crypto_shared_p256(const uint8_t private_key[32], const uint8_t x[32], const uint8_t y[32], uint8_t shared[32])
{
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *point = key ? EC_POINT_new(EC_KEY_get0_group(key)) : NULL;
    BIGNUM *d = BN_bin2bn(private_key, 32, NULL), *px = BN_bin2bn(x, 32, NULL), *py = BN_bin2bn(y, 32, NULL);
    bool ok = key && point && d && px && py && EC_KEY_set_private_key(key, d) == 1 &&
              EC_POINT_set_affine_coordinates(EC_KEY_get0_group(key), point, px, py, NULL) == 1 &&
              ECDH_compute_key(shared, 32, point, key, NULL) == 32;
    BN_free(d); BN_free(px); BN_free(py); EC_POINT_free(point); EC_KEY_free(key);
    return ok;
}

bool fido2_crypto_sign_p256(const uint8_t private_key[32], const uint8_t *data, size_t len,
                            uint8_t *der_signature, size_t *signature_len)
{
    uint8_t hash[32];
    fido2_crypto_sha256(data, len, hash);
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *d = BN_bin2bn(private_key, 32, NULL);
    bool ok = key && d && EC_KEY_set_private_key(key, d) == 1;
    if (ok) {
        unsigned int n = (unsigned int)*signature_len;
        ok = ECDSA_sign(0, hash, sizeof(hash), der_signature, &n, key) == 1;
        if (ok) *signature_len = n;
    }
    BN_free(d); EC_KEY_free(key);
    return ok;
}

#else

bool fido2_crypto_random(uint8_t *, size_t) { return false; }
void fido2_crypto_sha256(const uint8_t *, size_t, uint8_t out[32]) { memset(out, 0, 32); }
void fido2_crypto_hmac(const uint8_t *, const uint8_t *, size_t, uint8_t out[32]) { memset(out, 0, 32); }
bool fido2_crypto_pbkdf2(const uint8_t *, size_t, const uint8_t *, uint32_t, uint8_t *) { return false; }
bool fido2_crypto_hkdf(const uint8_t *, const char *, uint8_t *) { return false; }
bool fido2_crypto_aes_cbc(const uint8_t *, const uint8_t *, bool, const uint8_t *, size_t, uint8_t *) { return false; }
bool fido2_crypto_aes_gcm_seal(const uint8_t *, const uint8_t *, const uint8_t *, size_t, const uint8_t *, uint8_t *, uint8_t *) { return false; }
bool fido2_crypto_aes_gcm_open(const uint8_t *, const uint8_t *, const uint8_t *, size_t, const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool fido2_crypto_generate_p256(uint8_t *, uint8_t *, uint8_t *) { return false; }
bool fido2_crypto_shared_p256(const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *) { return false; }
bool fido2_crypto_sign_p256(const uint8_t *, const uint8_t *, size_t, uint8_t *, size_t *) { return false; }

#endif
