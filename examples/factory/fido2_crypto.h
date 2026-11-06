#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool fido2_crypto_random(uint8_t *out, size_t len);
void fido2_crypto_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void fido2_crypto_hmac(const uint8_t key[32], const uint8_t *data, size_t len, uint8_t out[32]);
bool fido2_crypto_pbkdf2(const uint8_t *password, size_t password_len,
                         const uint8_t salt[16], uint32_t iterations, uint8_t out[32]);
bool fido2_crypto_hkdf(const uint8_t key[32], const char *info, uint8_t out[32]);
bool fido2_crypto_aes_cbc(const uint8_t key[32], const uint8_t iv[16], bool encrypt,
                          const uint8_t *in, size_t len, uint8_t *out);
bool fido2_crypto_aes_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t plain[32], uint8_t encrypted[32], uint8_t tag[16]);
bool fido2_crypto_aes_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t encrypted[32], const uint8_t tag[16], uint8_t plain[32]);
bool fido2_crypto_generate_p256(uint8_t private_key[32], uint8_t x[32], uint8_t y[32]);
bool fido2_crypto_shared_p256(const uint8_t private_key[32], const uint8_t x[32], const uint8_t y[32], uint8_t shared[32]);
bool fido2_crypto_sign_p256(const uint8_t private_key[32], const uint8_t *data, size_t len,
                            uint8_t *der_signature, size_t *signature_len);
