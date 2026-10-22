// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/grammar_tokens.h"

#include <string>

#include "chrome/services/molt_ai/inference/engine/prompt_utils.h"

namespace molt_ai {

GrammarToken ClassifyGrammarToken(const GrammarTokenFacts& token) {
  if (token.end_of_generation) return GrammarToken::kEndOfGeneration;
  if (token.control) return GrammarToken::kBanned;
  if (token.user_defined &&
      LooksLikeSpecialTokenText(std::string(token.text))) {
    return GrammarToken::kBanned;
  }
  if (token.piece.empty() || token.piece.size() > kMaxGrammarPieceBytes ||
      token.piece.find('\0') != std::string_view::npos) {
    return GrammarToken::kBanned;
  }
  return GrammarToken::kAllowed;
}

// static
bool Utf8Prefix::Feed(State* s, uint8_t b) {
  if (s->remaining > 0) {
    if (b < s->low || b > s->high) return false;
    --s->remaining;
    s->low = 0x80;
    s->high = 0xBF;
    return true;
  }
  if (b == 0) return false;
  if (b < 0x80) return true;
  s->low = 0x80;
  s->high = 0xBF;
  if (b >= 0xC2 && b <= 0xDF) {
    s->remaining = 1;
  } else if (b == 0xE0) {
    s->remaining = 2;
    s->low = 0xA0;  // no overlong forms
  } else if (b == 0xED) {
    s->remaining = 2;
    s->high = 0x9F;  // no surrogates
  } else if (b >= 0xE1 && b <= 0xEF) {
    s->remaining = 2;
  } else if (b == 0xF0) {
    s->remaining = 3;
    s->low = 0x90;  // no overlong forms
  } else if (b >= 0xF1 && b <= 0xF3) {
    s->remaining = 3;
  } else if (b == 0xF4) {
    s->remaining = 3;
    s->high = 0x8F;  // nothing above U+10FFFF
  } else {
    return false;  // 0x80-0xC1 (continuation, overlong lead), 0xF5-0xFF
  }
  return true;
}

bool Utf8Prefix::CanAppend(std::string_view piece) const {
  State s = state_;
  for (char c : piece) {
    if (!Feed(&s, static_cast<uint8_t>(c))) return false;
  }
  return true;
}

bool Utf8Prefix::Append(std::string_view piece) {
  State s = state_;
  for (char c : piece) {
    if (!Feed(&s, static_cast<uint8_t>(c))) return false;
  }
  state_ = s;
  return true;
}

}  // namespace molt_ai
