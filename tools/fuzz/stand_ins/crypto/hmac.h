// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's crypto/hmac.h (SignSha256 /
// VerifySha256), for tools/fuzz/molt_fuzz.py only, on OpenSSL's libcrypto.

#ifndef MOLT_FUZZ_STAND_INS_CRYPTO_HMAC_H_
#define MOLT_FUZZ_STAND_INS_CRYPTO_HMAC_H_

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>
#include <span>

namespace crypto::hmac {

inline std::array<uint8_t, 32> SignSha256(std::span<const uint8_t> key,
                                          std::span<const uint8_t> data) {
  std::array<uint8_t, 32> out{};
  unsigned int len = 0;
  static const uint8_t kEmpty = 0;
  HMAC(EVP_sha256(), key.empty() ? &kEmpty : key.data(),
       static_cast<int>(key.size()), data.empty() ? &kEmpty : data.data(),
       data.size(), out.data(), &len);
  return out;
}

inline bool VerifySha256(std::span<const uint8_t> key,
                         std::span<const uint8_t> data,
                         std::span<const uint8_t, 32> mac) {
  const std::array<uint8_t, 32> expected = SignSha256(key, data);
  return CRYPTO_memcmp(expected.data(), mac.data(), 32) == 0;
}

}  // namespace crypto::hmac

#endif  // MOLT_FUZZ_STAND_INS_CRYPTO_HMAC_H_
