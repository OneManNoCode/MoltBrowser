// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/base64.h (the subset
// MoltBrowser uses), for tools/fuzz/molt_fuzz.py only. kStrict decoding,
// as upstream's: the standard alphabet, padding required, no whitespace,
// and unused bits of the last character must be zero.

#ifndef MOLT_FUZZ_STAND_INS_BASE_BASE64_H_
#define MOLT_FUZZ_STAND_INS_BASE_BASE64_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace base {

enum class Base64DecodePolicy { kStrict, kForgiving };

inline std::string Base64Encode(std::span<const uint8_t> input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 3 <= input.size(); i += 3) {
    const uint32_t v = input[i] << 16 | input[i + 1] << 8 | input[i + 2];
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (input.size() - i == 1) {
    const uint32_t v = input[i] << 16;
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += "==";
  } else if (input.size() - i == 2) {
    const uint32_t v = input[i] << 16 | input[i + 1] << 8;
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

inline std::string Base64Encode(std::string_view input) {
  return Base64Encode(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(input.data()), input.size()));
}

inline bool Base64Decode(std::string_view input,
                         std::string* output,
                         Base64DecodePolicy policy = Base64DecodePolicy::kStrict) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (input.size() % 4 != 0) return false;
  std::string out;
  for (size_t i = 0; i < input.size(); i += 4) {
    const bool last = i + 4 == input.size();
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = input[i + k];
      if (c == '=' && last && k >= 2) {
        v[k] = 0;
        ++pad;
        continue;
      }
      if (pad > 0) return false;  // data after padding
      v[k] = value(c);
      if (v[k] < 0) return false;
    }
    const uint32_t bits = static_cast<uint32_t>(v[0] << 18 | v[1] << 12 |
                                                v[2] << 6 | v[3]);
    out += static_cast<char>(bits >> 16);
    if (pad < 2) out += static_cast<char>((bits >> 8) & 0xFF);
    if (pad < 1) out += static_cast<char>(bits & 0xFF);
    // Strict: the bits the padding hides must be zero.
    if ((pad == 2 && (bits & 0xFFFF)) || (pad == 1 && (bits & 0xFF))) {
      return false;
    }
  }
  output->swap(out);
  return true;
}

inline std::optional<std::vector<uint8_t>> Base64Decode(std::string_view input) {
  std::string out;
  if (!Base64Decode(input, &out)) return std::nullopt;
  return std::vector<uint8_t>(out.begin(), out.end());
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_BASE64_H_
