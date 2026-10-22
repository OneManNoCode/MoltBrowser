// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Std-only rules for constrained decoding (grammar_sampler.h): which tokens
// of a vocabulary may be sampled while a grammar is active, and a strict
// incremental UTF-8 check. Together they keep llama.cpp's grammar accept
// path from ever seeing a token it would reject (it throws, or aborts, on
// one), and keep the visible text exactly the text the grammar matched.
//
// Why each rule:
//  * Control, unknown and unused tokens render as nothing in the visible
//    text but as their markup to the grammar (llama.cpp's piece cache is
//    rendered with special=true), so the two would disagree. They are
//    never sampled; end-of-generation tokens are left to the grammar,
//    which allows them only once the value is complete.
//  * User-defined tokens that look like markup ("<think>", "<|channel>")
//    are what the reasoning filters act on; under a grammar they would land
//    inside JSON strings. Never sampled.
//  * Pieces with a NUL byte: llama.cpp's grammar reads pieces as C strings,
//    so it would stop at the NUL while the text went on. Never sampled.
//  * Pieces longer than kMaxGrammarPieceBytes: llama.cpp's candidate check
//    recurses once per character of a piece. Never sampled.
//  * Invalid UTF-8 across pieces: llama.cpp's grammar decodes overlong
//    forms (and reads the code point 0 they can produce as the end of a
//    piece), so a token it accepted could still empty every grammar stack
//    when accepted, which throws. Utf8Prefix allows only strictly valid
//    UTF-8 (RFC 3629, no NUL), across piece boundaries.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GRAMMAR_TOKENS_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GRAMMAR_TOKENS_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace molt_ai {

inline constexpr size_t kMaxGrammarPieceBytes = 256;

enum class GrammarToken {
  // May be sampled when the grammar allows it.
  kAllowed,
  // Ends the answer; allowed by the grammar once the value is complete.
  kEndOfGeneration,
  // Never sampled under a grammar.
  kBanned,
};

struct GrammarTokenFacts {
  bool end_of_generation = false;
  // LLAMA_TOKEN_ATTR_CONTROL / _UNKNOWN / _UNUSED, or no attribute at all.
  bool control = false;
  bool user_defined = false;
  // The token's vocab text (llama_vocab_get_text).
  std::string_view text;
  // The token as llama.cpp's grammar sees it (llama_token_to_piece with
  // special=true, no lstrip).
  std::string_view piece;
};

GrammarToken ClassifyGrammarToken(const GrammarTokenFacts& token);

// Strict UTF-8 (RFC 3629, no NUL) across a sequence of pieces: the text so
// far is valid except possibly for an incomplete last character, which may
// still become valid.
class Utf8Prefix {
 public:
  // Whether |piece| can follow the text so far.
  bool CanAppend(std::string_view piece) const;
  // Appends |piece|; returns CanAppend(piece). On false the state is
  // unchanged.
  bool Append(std::string_view piece);
  // The text so far ends inside a character.
  bool incomplete() const { return state_.remaining > 0; }

 private:
  struct State {
    uint8_t remaining = 0;  // continuation bytes still expected
    uint8_t low = 0x80;     // range allowed for the next one
    uint8_t high = 0xBF;
  };
  // Feeds one byte to |*state|; false if it can't continue valid UTF-8.
  static bool Feed(State* state, uint8_t byte);

  State state_;
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_GRAMMAR_TOKENS_H_
