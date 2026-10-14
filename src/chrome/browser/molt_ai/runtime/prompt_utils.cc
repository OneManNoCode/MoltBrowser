// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/prompt_utils.h"

#include <algorithm>
#include <cstddef>

namespace molt_ai {

namespace {

constexpr char kZeroWidthSpace[] = "\xE2\x80\x8B";

}  // namespace

size_t CommonPrefixLength(const std::vector<int32_t>& a,
                          const std::vector<int32_t>& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  return i;
}

std::vector<int32_t> TruncateTokensMiddle(const std::vector<int32_t>& tokens,
                                          size_t limit,
                                          const std::vector<int32_t>& marker) {
  if (tokens.size() <= limit) return tokens;
  if (limit <= marker.size()) {
    // No room for a marker: keep the tail (it carries the instructions).
    return std::vector<int32_t>(
        tokens.end() - static_cast<std::ptrdiff_t>(limit), tokens.end());
  }
  const size_t keep = limit - marker.size();
  const auto head = static_cast<std::ptrdiff_t>(keep / 3);
  const auto tail = static_cast<std::ptrdiff_t>(keep) - head;
  std::vector<int32_t> out;
  out.reserve(limit);
  out.insert(out.end(), tokens.begin(), tokens.begin() + head);
  out.insert(out.end(), marker.begin(), marker.end());
  out.insert(out.end(), tokens.end() - tail, tokens.end());
  return out;
}

bool LooksLikeSpecialTokenText(const std::string& text) {
  if (text.size() < 3) return false;
  const char first = text.front();
  const char last = text.back();
  return (first == '<' && last == '>') || (first == '[' && last == ']');
}

std::string NeutralizeSpecialTokens(
    const std::string& text,
    const std::vector<std::string>& special_texts) {
  if (text.empty() || special_texts.empty()) return text;
  // Cheap reject: every neutralized string starts with '<' or '['.
  if (text.find_first_of("<[") == std::string::npos) return text;

  // Mark byte offsets that start a special-token occurrence, then rebuild.
  std::vector<bool> starts(text.size(), false);
  bool any = false;
  for (const std::string& s : special_texts) {
    if (s.size() < 2) continue;
    size_t pos = text.find(s);
    while (pos != std::string::npos) {
      starts[pos] = true;
      any = true;
      pos = text.find(s, pos + 1);
    }
  }
  if (!any) return text;
  std::string out;
  out.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    out.push_back(text[i]);
    if (starts[i]) out += kZeroWidthSpace;
  }
  return out;
}

}  // namespace molt_ai
