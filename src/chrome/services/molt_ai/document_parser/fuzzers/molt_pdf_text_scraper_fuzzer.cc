// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The document parser's PDF text scraper (pdf/pdf_text_scraper.h) on any
// PDF a website serves: its own stream walker, zlib inflation with the
// decompression-bomb caps, and PDF string unescaping. The whole input is
// the file. Run with -timeout and -rss_limit_mb: a hang or an inflation
// past the caps is a bug.
//
// Checks: no memory or UB error; the text is at most |max_chars| bytes,
// with no run of two blanks and no blank line; non-PDF input gives no text.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/check.h"
#include "chrome/services/molt_ai/document_parser/pdf/pdf_text_scraper.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::vector<uint8_t> pdf(data, data + size);
  // The document parser's default, and a small cap derived from the input
  // so the cut paths run too.
  for (const size_t max_chars : {size_t{50000}, size % 97}) {
    const std::string text = molt_ai::pdf::ExtractText(pdf, max_chars);
    CHECK(text.size() <= max_chars);
    for (size_t i = 0; i < text.size(); ++i) {
      CHECK(text[i] != '\t' && text[i] != '\r');
      if (i > 0) {
        CHECK(!(text[i] == ' ' && text[i - 1] == ' '));
        CHECK(!(text[i] == '\n' && text[i - 1] == '\n'));
      }
    }
    // Only a file that starts with "%PDF-" (after an optional UTF-8 BOM).
    const bool bom = size >= 3 && data[0] == 0xEF && data[1] == 0xBB &&
                     data[2] == 0xBF;
    const bool pdf_header =
        size >= 8 && std::string(reinterpret_cast<const char*>(data) +
                                     (bom ? 3 : 0),
                                 5) == "%PDF-";
    if (!pdf_header) CHECK(text.empty());
  }
  return 0;
}
