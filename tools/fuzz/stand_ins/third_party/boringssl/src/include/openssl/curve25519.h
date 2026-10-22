// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for BoringSSL's openssl/curve25519.h (the
// Ed25519 functions MoltBrowser uses), for tools/fuzz/molt_fuzz.py only,
// on OpenSSL's libcrypto (EVP_PKEY_ED25519). Same contract: 1 on success,
// 0 otherwise; ED25519_sign takes the 64-byte BoringSSL private key (seed
// followed by the public key).

#ifndef MOLT_FUZZ_STAND_INS_OPENSSL_CURVE25519_H_
#define MOLT_FUZZ_STAND_INS_OPENSSL_CURVE25519_H_

#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>

#define ED25519_PRIVATE_KEY_LEN 64
#define ED25519_PUBLIC_KEY_LEN 32
#define ED25519_SIGNATURE_LEN 64

inline int ED25519_verify(const uint8_t* message,
                          size_t message_len,
                          const uint8_t signature[ED25519_SIGNATURE_LEN],
                          const uint8_t public_key[ED25519_PUBLIC_KEY_LEN]) {
  EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                              public_key,
                                              ED25519_PUBLIC_KEY_LEN);
  if (!key) return 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  int ok = 0;
  static const uint8_t kEmpty = 0;
  if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
    ok = EVP_DigestVerify(ctx, signature, ED25519_SIGNATURE_LEN,
                          message_len ? message : &kEmpty, message_len) == 1;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

inline int ED25519_sign(uint8_t out_sig[ED25519_SIGNATURE_LEN],
                        const uint8_t* message,
                        size_t message_len,
                        const uint8_t private_key[ED25519_PRIVATE_KEY_LEN]) {
  EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                               private_key, 32);
  if (!key) return 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  int ok = 0;
  size_t sig_len = ED25519_SIGNATURE_LEN;
  static const uint8_t kEmpty = 0;
  if (ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
    ok = EVP_DigestSign(ctx, out_sig, &sig_len,
                        message_len ? message : &kEmpty, message_len) == 1;
  }
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

inline void ED25519_keypair_from_seed(uint8_t out_public_key[32],
                                      uint8_t out_private_key[64],
                                      const uint8_t seed[32]) {
  EVP_PKEY* key =
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed, 32);
  size_t len = 32;
  EVP_PKEY_get_raw_public_key(key, out_public_key, &len);
  EVP_PKEY_free(key);
  for (int i = 0; i < 32; ++i) {
    out_private_key[i] = seed[i];
    out_private_key[32 + i] = out_public_key[i];
  }
}

#endif  // MOLT_FUZZ_STAND_INS_OPENSSL_CURVE25519_H_
