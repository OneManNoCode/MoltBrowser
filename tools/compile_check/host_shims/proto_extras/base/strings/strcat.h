// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
// compile_check STAND-IN for //base in a host tool (see base/check.h).

#ifndef COMPILE_CHECK_SHIM_BASE_STRINGS_STRCAT_H_
#define COMPILE_CHECK_SHIM_BASE_STRINGS_STRCAT_H_

#include <initializer_list>
#include <string>
#include <string_view>

namespace base {

inline std::string StrCat(std::initializer_list<std::string_view> pieces) {
  std::string out;
  for (std::string_view p : pieces) {
    out.append(p);
  }
  return out;
}

}  // namespace base

#endif  // COMPILE_CHECK_SHIM_BASE_STRINGS_STRCAT_H_
