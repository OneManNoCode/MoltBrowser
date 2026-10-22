// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/strings/strcat.h, for
// tools/fuzz/molt_fuzz.py only.

#ifndef MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRCAT_H_
#define MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRCAT_H_

#include <initializer_list>
#include <string>
#include <string_view>

namespace base {

inline std::string StrCat(std::initializer_list<std::string_view> pieces) {
  std::string out;
  for (std::string_view p : pieces) out.append(p);
  return out;
}

inline void StrAppend(std::string* dest,
                      std::initializer_list<std::string_view> pieces) {
  for (std::string_view p : pieces) dest->append(p);
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRCAT_H_
