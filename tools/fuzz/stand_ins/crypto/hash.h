// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's crypto/hash.h (SHA-256), for
// tools/fuzz/molt_fuzz.py only, on OpenSSL's libcrypto.

#ifndef MOLT_FUZZ_STAND_INS_CRYPTO_HASH_H_
#define MOLT_FUZZ_STAND_INS_CRYPTO_HASH_H_

#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace crypto::hash {

inline constexpr size_t kSha256Size = 32;

inline std::array<uint8_t, kSha256Size> Sha256(std::span<const uint8_t> data) {
  std::array<uint8_t, kSha256Size> out{};
  SHA256(data.data(), data.size(), out.data());
  return out;
}

inline std::array<uint8_t, kSha256Size> Sha256(std::string_view data) {
  return Sha256(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

}  // namespace crypto::hash

#endif  // MOLT_FUZZ_STAND_INS_CRYPTO_HASH_H_
