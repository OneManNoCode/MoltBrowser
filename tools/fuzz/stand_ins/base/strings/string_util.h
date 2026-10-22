// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/strings/string_util.h
// (the subset MoltBrowser uses), for tools/fuzz/molt_fuzz.py only.
// IsStringUTF8() is strict (no surrogates, overlong forms or code points
// past U+10FFFF) and, like upstream, refuses noncharacters (U+FDD0..FDEF,
// U+xxFFFE/F); IsStringUTF8AllowingNoncharacters() doesn't.

#ifndef MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_UTIL_H_
#define MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_UTIL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace base {

enum class CompareCase { SENSITIVE, INSENSITIVE_ASCII };

enum TrimPositions {
  TRIM_NONE = 0,
  TRIM_LEADING = 1 << 0,
  TRIM_TRAILING = 1 << 1,
  TRIM_ALL = TRIM_LEADING | TRIM_TRAILING,
};

inline constexpr char kWhitespaceASCII[] = "\x09\x0A\x0B\x0C\x0D\x20";

template <typename Char>
constexpr bool IsAsciiWhitespace(Char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' ||
         c == '\v';
}
template <typename Char>
constexpr bool IsAsciiAlpha(Char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
template <typename Char>
constexpr bool IsAsciiDigit(Char c) {
  return c >= '0' && c <= '9';
}
template <typename Char>
constexpr bool IsAsciiAlphaNumeric(Char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c);
}
template <typename Char>
constexpr bool IsAsciiUpper(Char c) {
  return c >= 'A' && c <= 'Z';
}
template <typename Char>
constexpr bool IsAsciiLower(Char c) {
  return c >= 'a' && c <= 'z';
}
template <typename Char>
constexpr bool IsAsciiPrintable(Char c) {
  return c >= ' ' && c <= '~';
}

constexpr char ToLowerASCII(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}
constexpr char ToUpperASCII(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}
inline std::string ToLowerASCII(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = ToLowerASCII(c);
  return out;
}
inline std::string ToUpperASCII(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = ToUpperASCII(c);
  return out;
}

inline bool EqualsCaseInsensitiveASCII(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) return false;
  }
  return true;
}

inline bool StartsWith(std::string_view s,
                       std::string_view prefix,
                       CompareCase cc = CompareCase::SENSITIVE) {
  if (s.size() < prefix.size()) return false;
  return cc == CompareCase::SENSITIVE
             ? s.substr(0, prefix.size()) == prefix
             : EqualsCaseInsensitiveASCII(s.substr(0, prefix.size()), prefix);
}
inline bool EndsWith(std::string_view s,
                     std::string_view suffix,
                     CompareCase cc = CompareCase::SENSITIVE) {
  if (s.size() < suffix.size()) return false;
  const std::string_view tail = s.substr(s.size() - suffix.size());
  return cc == CompareCase::SENSITIVE ? tail == suffix
                                      : EqualsCaseInsensitiveASCII(tail, suffix);
}

inline std::string_view TrimWhitespaceASCII(std::string_view s,
                                            TrimPositions positions) {
  size_t begin = 0;
  size_t end = s.size();
  if (positions & TRIM_LEADING) {
    while (begin < end && IsAsciiWhitespace(s[begin])) ++begin;
  }
  if (positions & TRIM_TRAILING) {
    while (end > begin && IsAsciiWhitespace(s[end - 1])) --end;
  }
  return s.substr(begin, end - begin);
}

inline std::string CollapseWhitespaceASCII(std::string_view text,
                                           bool trim_sequences_with_line_breaks) {
  std::string out;
  bool in_ws = true;
  bool saw_newline = false;
  for (char c : text) {
    if (IsAsciiWhitespace(c)) {
      if (!in_ws) {
        in_ws = true;
        saw_newline = false;
      }
      if (c == '\n' || c == '\r') saw_newline = true;
      continue;
    }
    if (in_ws && !out.empty() &&
        !(trim_sequences_with_line_breaks && saw_newline)) {
      out.push_back(' ');
    }
    in_ws = false;
    out.push_back(c);
  }
  return out;
}

inline bool IsStringASCII(std::string_view s) {
  for (char c : s) {
    if (static_cast<unsigned char>(c) >= 0x80) return false;
  }
  return true;
}

namespace internal {

// Length of the valid UTF-8 character at s[i] and its code point, or 0.
inline size_t DecodeUtf8(std::string_view s, size_t i, uint32_t* cp) {
  const auto c = static_cast<unsigned char>(s[i]);
  size_t n;
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if (c >= 0xC2 && c <= 0xDF) {
    n = 2;
    *cp = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    n = 3;
    *cp = c & 0x0F;
  } else if (c >= 0xF0 && c <= 0xF4) {
    n = 4;
    *cp = c & 0x07;
  } else {
    return 0;
  }
  if (i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k) {
    const auto cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return 0;
    *cp = (*cp << 6) | (cc & 0x3F);
  }
  if ((n == 3 && *cp < 0x800) || (n == 4 && *cp < 0x10000) ||
      *cp > 0x10FFFF || (*cp >= 0xD800 && *cp <= 0xDFFF)) {
    return 0;
  }
  return n;
}

inline bool IsNoncharacter(uint32_t cp) {
  return (cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE;
}

}  // namespace internal

inline bool IsStringUTF8AllowingNoncharacters(std::string_view s) {
  uint32_t cp;
  for (size_t i = 0; i < s.size();) {
    const size_t n = internal::DecodeUtf8(s, i, &cp);
    if (n == 0) return false;
    i += n;
  }
  return true;
}

inline bool IsStringUTF8(std::string_view s) {
  uint32_t cp;
  for (size_t i = 0; i < s.size();) {
    const size_t n = internal::DecodeUtf8(s, i, &cp);
    if (n == 0 || internal::IsNoncharacter(cp)) return false;
    i += n;
  }
  return true;
}

// The longest prefix of |input| of at most |byte_size| bytes that doesn't
// end inside a UTF-8 character (invalid bytes are dropped at the cut).
inline std::string_view TruncateUTF8ToByteSize(std::string_view input,
                                               size_t byte_size) {
  if (byte_size >= input.size()) return input;
  size_t end = 0;
  uint32_t cp;
  for (size_t i = 0; i < byte_size;) {
    const size_t n = internal::DecodeUtf8(input, i, &cp);
    if (n == 0 || i + n > byte_size) break;
    i += n;
    end = i;
  }
  return input.substr(0, end);
}

inline void ReplaceSubstringsAfterOffset(std::string* str,
                                         size_t start_offset,
                                         std::string_view find_this,
                                         std::string_view replace_with) {
  if (find_this.empty()) return;
  std::string out;
  out.reserve(str->size());
  out.append(*str, 0, std::min(start_offset, str->size()));
  size_t pos = std::min(start_offset, str->size());
  while (true) {
    const size_t at = str->find(find_this, pos);
    if (at == std::string::npos) break;
    out.append(*str, pos, at - pos);
    out.append(replace_with);
    pos = at + find_this.size();
  }
  out.append(*str, pos, std::string::npos);
  str->swap(out);
}

inline std::string JoinString(const std::vector<std::string>& parts,
                              std::string_view separator) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out.append(separator);
    out.append(parts[i]);
  }
  return out;
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRING_UTIL_H_
