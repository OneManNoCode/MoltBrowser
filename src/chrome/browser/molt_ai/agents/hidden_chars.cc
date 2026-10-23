// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/hidden_chars.h"

namespace molt_ai {
namespace hidden_chars {

namespace {

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD

}  // namespace

size_t HiddenCharAt(std::string_view s, size_t i) {
  auto byte = [&](size_t k) {
    return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0;
  };
  const unsigned char b0 = byte(0);
  const unsigned char b1 = byte(1);
  const unsigned char b2 = byte(2);
  const unsigned char b3 = byte(3);
  // U+0080-U+009F, U+00AD.
  if (b0 == 0xC2 && ((b1 >= 0x80 && b1 <= 0x9F) || b1 == 0xAD))
    return 2;
  // U+061C.
  if (b0 == 0xD8 && b1 == 0x9C)
    return 2;
  // U+115F, U+1160.
  if (b0 == 0xE1 && b1 == 0x85 && (b2 == 0x9F || b2 == 0xA0))
    return 3;
  // U+180E.
  if (b0 == 0xE1 && b1 == 0xA0 && b2 == 0x8E)
    return 3;
  // U+200B-U+200F, U+2028-U+202E.
  if (b0 == 0xE2 && b1 == 0x80 &&
      ((b2 >= 0x8B && b2 <= 0x8F) || (b2 >= 0xA8 && b2 <= 0xAE))) {
    return 3;
  }
  // U+2060-U+2064, U+2066-U+2069.
  if (b0 == 0xE2 && b1 == 0x81 &&
      ((b2 >= 0xA0 && b2 <= 0xA4) || (b2 >= 0xA6 && b2 <= 0xA9))) {
    return 3;
  }
  // U+3164.
  if (b0 == 0xE3 && b1 == 0x85 && b2 == 0xA4)
    return 3;
  // U+FE00-U+FE0F.
  if (b0 == 0xEF && b1 == 0xB8 && b2 >= 0x80 && b2 <= 0x8F)
    return 3;
  // U+FEFF.
  if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF)
    return 3;
  // U+FFA0.
  if (b0 == 0xEF && b1 == 0xBE && b2 == 0xA0)
    return 3;
  // U+E0000-U+E007F (F3 A0 80 80 - F3 A0 81 BF) and U+E0100-U+E01EF
  // (F3 A0 84 80 - F3 A0 87 AF). The fourth byte must be there: a
  // continuation byte (valid UTF-8, as base::Value holds, has one).
  if (b0 == 0xF3 && b1 == 0xA0 && (b3 & 0xC0) == 0x80 &&
      (b2 == 0x80 || b2 == 0x81 || (b2 >= 0x84 && b2 <= 0x86) ||
       (b2 == 0x87 && b3 <= 0xAF))) {
    return 4;
  }
  return 0;
}

std::string ShowHidden(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (const size_t n = HiddenCharAt(s, i)) {
      out += kReplacement;
      i += n;
    } else {
      out += s[i];
      ++i;
    }
  }
  return out;
}

}  // namespace hidden_chars
}  // namespace molt_ai
