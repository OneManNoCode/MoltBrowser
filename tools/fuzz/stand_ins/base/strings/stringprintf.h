// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/strings/stringprintf.h,
// for tools/fuzz/molt_fuzz.py only.

#ifndef MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRINGPRINTF_H_
#define MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRINGPRINTF_H_

#include <cstdarg>
#include <cstdio>
#include <string>

namespace base {

__attribute__((format(printf, 1, 2))) inline std::string StringPrintf(
    const char* format,
    ...) {
  va_list ap;
  va_start(ap, format);
  va_list ap2;
  va_copy(ap2, ap);
  const int n = std::vsnprintf(nullptr, 0, format, ap);
  va_end(ap);
  std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
  if (n > 0) std::vsnprintf(out.data(), out.size() + 1, format, ap2);
  va_end(ap2);
  return out;
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_STRINGS_STRINGPRINTF_H_
