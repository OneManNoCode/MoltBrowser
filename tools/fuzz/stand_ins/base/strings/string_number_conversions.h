// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's
// base/strings/string_number_conversions.h (the subset MoltBrowser uses),
// for tools/fuzz/molt_fuzz.py only. StringTo*() accept exactly what
// upstream accepts: an optional '-' (or '+'), digits, nothing else, and
// fail on overflow.

#ifndef MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace base {

inline std::string NumberToString(int v) { return std::to_string(v); }
inline std::string NumberToString(unsigned v) { return std::to_string(v); }
inline std::string NumberToString(long v) { return std::to_string(v); }
inline std::string NumberToString(unsigned long v) { return std::to_string(v); }
inline std::string NumberToString(long long v) { return std::to_string(v); }
inline std::string NumberToString(unsigned long long v) {
  return std::to_string(v);
}
inline std::string NumberToString(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

namespace internal {

template <typename T>
bool StringToSigned(std::string_view s, T* out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (s[0] == '-' || s[0] == '+') {
    negative = s[0] == '-';
    i = 1;
  }
  if (i == s.size()) return false;
  // Accumulate negatively so the minimum fits.
  T value = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const T digit = static_cast<T>(s[i] - '0');
    if (__builtin_mul_overflow(value, T{10}, &value) ||
        __builtin_sub_overflow(value, digit, &value)) {
      return false;
    }
  }
  if (!negative) {
    if (value == std::numeric_limits<T>::min()) return false;
    value = static_cast<T>(-value);
  }
  *out = value;
  return true;
}

template <typename T>
bool StringToUnsigned(std::string_view s, T* out) {
  if (s.empty()) return false;
  size_t i = s[0] == '+' ? 1 : 0;
  if (i == s.size()) return false;
  T value = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    const T digit = static_cast<T>(s[i] - '0');
    if (__builtin_mul_overflow(value, T{10}, &value) ||
        __builtin_add_overflow(value, digit, &value)) {
      return false;
    }
  }
  *out = value;
  return true;
}

inline int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace internal

inline bool StringToInt(std::string_view s, int* out) {
  return internal::StringToSigned(s, out);
}
inline bool StringToInt64(std::string_view s, int64_t* out) {
  return internal::StringToSigned(s, out);
}
inline bool StringToUint(std::string_view s, unsigned* out) {
  return internal::StringToUnsigned(s, out);
}
inline bool StringToUint64(std::string_view s, uint64_t* out) {
  return internal::StringToUnsigned(s, out);
}
inline bool StringToSizeT(std::string_view s, size_t* out) {
  return internal::StringToUnsigned(s, out);
}

inline std::string HexEncode(std::span<const uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15]);
  }
  return out;
}
inline std::string HexEncodeLower(std::span<const uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 15]);
  }
  return out;
}
inline std::string HexEncode(std::string_view bytes) {
  return HexEncode(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}

// Decodes |input| into exactly |output|.size() bytes; false otherwise.
inline bool HexStringToSpan(std::string_view input, std::span<uint8_t> output) {
  if (input.size() != output.size() * 2) return false;
  for (size_t i = 0; i < output.size(); ++i) {
    const int hi = internal::HexDigit(input[2 * i]);
    const int lo = internal::HexDigit(input[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    output[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return true;
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
