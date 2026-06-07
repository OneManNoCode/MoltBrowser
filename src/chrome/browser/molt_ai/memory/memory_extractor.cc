// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_extractor.h"

#include <algorithm>  // for std::min — required on Linux under -fmodules
#include <cctype>

#include "chrome/browser/molt_ai/memory/memory_types.h"

namespace molt_ai {
namespace memory {

namespace {

inline bool IsSpaceByte(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v' || c == 0xA0 /*nbsp first byte; harmless overmatch*/;
}

}  // namespace

std::string NormalizePageText(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool last_was_space = true;  // start trimmed
  for (char c : raw) {
    if (IsSpaceByte(static_cast<unsigned char>(c))) {
      if (!last_was_space) {
        out.push_back(' ');
        last_was_space = true;
      }
    } else {
      out.push_back(c);
      last_was_space = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  // Skip near-empty pages — too little signal to be worth embedding.
  if (CountWords(out) < 25) return std::string();
  return out;
}

std::vector<std::string> ChunkText(const std::string& text) {
  std::vector<std::string> out;
  if (text.empty()) return out;

  const size_t target = kChunkTargetChars;
  const size_t overlap = kChunkOverlapChars;
  size_t i = 0;
  while (i < text.size()) {
    size_t end = std::min(i + target, text.size());

    // Try to extend (up to +80 chars) until a sentence-ish break for
    // a nicer read on snippet preview. Skip if we'd run off the end.
    if (end < text.size()) {
      size_t scan_end = std::min(end + 80, text.size());
      for (size_t j = end; j < scan_end; ++j) {
        char c = text[j];
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
          end = j + 1;
          break;
        }
      }
    }

    out.push_back(text.substr(i, end - i));

    if (end >= text.size()) break;
    // Overlap window: step forward by target-overlap so neighboring
    // chunks share ~overlap chars (helps query terms that fall on a
    // boundary still match one of the two).
    i = (end > overlap) ? (end - overlap) : end;
  }
  return out;
}

int CountWords(const std::string& text) {
  int n = 0;
  bool in_word = false;
  for (char c : text) {
    bool space = IsSpaceByte(static_cast<unsigned char>(c));
    if (!space && !in_word) {
      ++n;
      in_word = true;
    } else if (space) {
      in_word = false;
    }
  }
  return n;
}

}  // namespace memory
}  // namespace molt_ai
