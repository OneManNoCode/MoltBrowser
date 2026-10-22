// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/omnibox/browser/molt_local_input.h"

#include "components/omnibox/browser/molt_memory_query.h"

namespace molt_omnibox {

namespace {

// The whitespace molt_memory's normalizer treats as a space (one UTF-16
// unit each).
bool IsSpace(char16_t c) {
  return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

char16_t AsciiLower(char16_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char16_t>(c + ('a' - 'A')) : c;
}

}  // namespace

std::optional<size_t> AiPromptStart(std::u16string_view text) {
  constexpr std::u16string_view kKeyword = kAiPromptKeyword;
  size_t i = 0;
  while (i < text.size() && IsSpace(text[i])) {
    ++i;
  }
  if (text.size() - i <= kKeyword.size()) {
    return std::nullopt;  // also "@ai" alone
  }
  for (size_t k = 0; k < kKeyword.size(); ++k) {
    if (AsciiLower(text[i + k]) != kKeyword[k]) {
      return std::nullopt;
    }
  }
  i += kKeyword.size();
  if (!IsSpace(text[i])) {
    return std::nullopt;  // "@aim ..."
  }
  return i + 1;
}

bool IsAiPromptInput(std::u16string_view text) {
  return AiPromptStart(text).has_value();
}

LocalInput ClassifyLocalInput(std::u16string_view text) {
  if (molt_memory::IsShortcutInput(text)) {
    return LocalInput::kMemoryShortcut;
  }
  if (IsAiPromptInput(text)) {
    return LocalInput::kAiPrompt;
  }
  return LocalInput::kNone;
}

bool IsLocalOnlyInput(std::u16string_view text) {
  return ClassifyLocalInput(text) != LocalInput::kNone;
}

}  // namespace molt_omnibox
