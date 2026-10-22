// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The document parser's .docx fallback (docx_text_extractor.h) on any file
// a user attaches or a site serves: the zip walk with its read cap, and the
// WordprocessingML scanner.
//
// Input: the first byte picks the target (the whole .docx, or the XML
// scanner alone); the rest is the file. Checks: no memory or UB error or
// timeout; the scanner is never longer than the XML and keeps nothing but
// paragraph, tab and break marks from XML without a <w:t> run;
// ExtractDocxText() returns non-blank UTF-8 or an error, never both.

#include <cstddef>
#include <cstdint>
#include <string>

#include "base/check.h"
#include "base/strings/string_util.h"
#include "chrome/services/molt_ai/document_parser/docx_text_extractor.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const std::string input(reinterpret_cast<const char*>(data) + 1, size - 1);
  if (data[0] & 1) {
    const std::string text = molt_ai::docx::StripWordprocessingXml(input);
    CHECK(text.size() <= input.size()) << "the scanner made text up";
    if (input.find("<w:t") == std::string::npos) {
      // No text run: only the paragraph, tab and break marks.
      CHECK(text.find_first_not_of("\n\t") == std::string::npos)
          << "text outside <w:t> runs";
    }
    return 0;
  }
  std::string error;
  const std::string text = molt_ai::docx::ExtractDocxText(input, &error);
  if (text.empty()) {
    CHECK(!error.empty());
  } else {
    CHECK(error.empty());
    CHECK(base::IsStringUTF8(text));
    CHECK(text.find_first_not_of(" \t\r\n") != std::string::npos);
  }
  return 0;
}
