// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/helper_output.h"

#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"

namespace molt_ai {

std::string SanitizeHelperText(std::string_view text, size_t max_bytes) {
  std::string out;
  if (base::IsStringUTF8AllowingNoncharacters(text)) {
    out.assign(text);
  } else {
    // The UTF-8 -> UTF-16 conversion replaces each invalid sequence with
    // U+FFFD.
    out = base::UTF16ToUTF8(base::UTF8ToUTF16(text));
  }
  if (out.size() > max_bytes) {
    out = std::string(base::TruncateUTF8ToByteSize(out, max_bytes));
  }
  return out;
}

}  // namespace molt_ai
