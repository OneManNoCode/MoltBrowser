// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Text that a helper program (tesseract, whisper-cli) wrote, made safe to
// put in a base::Value and send to an internal page. The helpers run outside
// the browser's sandbox on untrusted input (any website's PDF, an attached
// image, recorded audio), so their output is treated as untrusted bytes: it
// need not be UTF-8, and a byte-count cut can split a character.

#ifndef CHROME_BROWSER_MOLT_AI_COMMON_HELPER_OUTPUT_H_
#define CHROME_BROWSER_MOLT_AI_COMMON_HELPER_OUTPUT_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace molt_ai {

// |text| with every invalid UTF-8 sequence replaced by U+FFFD, then cut to at
// most |max_bytes| bytes at a character boundary.
std::string SanitizeHelperText(std::string_view text, size_t max_bytes);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_COMMON_HELPER_OUTPUT_H_
