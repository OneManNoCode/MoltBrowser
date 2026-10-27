// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h):
// ToUpperASCII() and ReplaceChars() as base/strings/string_util.cc has
// them (ReplaceChars returns whether anything was replaced).

#ifndef COMPILE_CHECK_SHIM_BASE_STRINGS_STRING_UTIL_H_
#define COMPILE_CHECK_SHIM_BASE_STRINGS_STRING_UTIL_H_

#include <string>
#include <string_view>

namespace base {

inline std::string ToUpperASCII(std::string_view str) {
  std::string out(str);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

inline bool ReplaceChars(std::string_view input,
                         std::string_view replace_chars,
                         std::string_view replace_with,
                         std::string* output) {
  std::string result;
  bool removed = false;
  for (char c : input) {
    if (replace_chars.find(c) != std::string_view::npos) {
      result.append(replace_with);
      removed = true;
    } else {
      result.push_back(c);
    }
  }
  *output = std::move(result);
  return removed;
}

}  // namespace base

#endif  // COMPILE_CHECK_SHIM_BASE_STRINGS_STRING_UTIL_H_
