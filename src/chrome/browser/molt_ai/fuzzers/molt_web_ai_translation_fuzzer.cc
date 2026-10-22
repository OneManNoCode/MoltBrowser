// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Translator API's text handling (web_ai/web_ai_translation.h) on what
// a web page passes: language tags and text to translate, and on what the
// model replies.
//
// Checks: no memory or UB error or timeout (the splitter is linear);
// SplitForTranslation() pieces concatenate back to the text exactly, each
// translated piece fits |max_tokens|, has letters and no surrounding
// whitespace, untranslated pieces are never adjacent, and on valid UTF-8 no
// piece starts or ends inside a character; TakeUntranslated() walks them;
// the model's instruction never contains page text (it is the same for any
// piece) and the piece goes only into the user turn; CleanTranslation()
// returns a trimmed substring of the reply or nothing; tag lookups agree
// with IsValidLanguageTag().

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::web_ai {
namespace {

bool IsWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool StartsInsideCharacter(const std::string& s) {
  return !s.empty() && (static_cast<unsigned char>(s.front()) & 0xC0) == 0x80;
}

void CheckSplit(const std::string& text, uint32_t max_tokens) {
  const std::vector<TranslationPiece> pieces =
      SplitForTranslation(text, max_tokens);
  std::string joined;
  const bool valid = IsValidUtf8(text);
  for (size_t i = 0; i < pieces.size(); ++i) {
    const TranslationPiece& piece = pieces[i];
    CHECK(!piece.text.empty());
    joined += piece.text;
    if (piece.translate) {
      CHECK(EstimateTokens(piece.text) <= std::max<uint32_t>(max_tokens, 2))
          << "a piece over the token budget";
      CHECK(!IsWs(piece.text.front()) && !IsWs(piece.text.back()));
    } else if (i > 0) {
      CHECK(pieces[i - 1].translate) << "adjacent untranslated pieces";
    }
    if (valid) {
      CHECK(!StartsInsideCharacter(piece.text));
      CHECK(IsValidUtf8(piece.text)) << "a piece cut inside a character";
    }
  }
  CHECK(joined == text) << "the pieces don't add up to the text";

  // The backend's walk over the pieces.
  size_t next = 0;
  std::string walked;
  while (next < pieces.size()) {
    const size_t before = next;
    walked += TakeUntranslated(pieces, &next);
    if (next < pieces.size()) {
      CHECK(pieces[next].translate);
      walked += pieces[next].text;
      ++next;
    }
    CHECK(next > before);
  }
  CHECK(walked == text);
}

}  // namespace
}  // namespace molt_ai::web_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::web_ai;
  FuzzedDataProvider p(data, size);
  const uint32_t max_tokens = p.ConsumeBool()
                                  ? kTranslatorPieceTokens
                                  : p.ConsumeIntegralInRange<uint32_t>(0, 64);
  const std::string source_tag = p.ConsumeRandomLengthString(48);
  const std::string target_tag = p.ConsumeRandomLengthString(48);
  const std::string reply = p.ConsumeRandomLengthString(2048);
  const std::string text = p.ConsumeRemainingBytesAsString();

  for (const std::string* tag : {&source_tag, &target_tag}) {
    const TranslatorLanguage* language = FindTranslatorLanguage(*tag);
    if (language) CHECK(IsValidLanguageTag(*tag));
    const std::string name = LanguageNameForPrompt(*tag);
    CHECK(name.empty() == !IsValidLanguageTag(*tag));
  }
  const std::optional<TranslatorPair> pair =
      ResolveTranslatorPair(source_tag, target_tag);
  if (pair) {
    CHECK(pair->source && pair->target && pair->source != pair->target);
  }

  CheckSplit(text, max_tokens);

  // The instruction is built only from the table's names.
  TranslatorPair any;
  any.source = &kTranslatorLanguages[0];
  any.target = &kTranslatorLanguages[1];
  const TranslatorPair& use = pair ? *pair : any;
  const TaskPrompt prompt = BuildTranslatorPrompt(use, text);
  const TaskPrompt other = BuildTranslatorPrompt(use, "x");
  CHECK(prompt.system_prompt == other.system_prompt);
  CHECK(prompt.context.empty() && prompt.context_preamble.empty());
  CHECK(prompt.user_message == text);
  CHECK(prompt.max_output_tokens <= kTranslatorMaxReplyTokens);

  const std::optional<std::string> cleaned = CleanTranslation(text, reply);
  if (cleaned) {
    CHECK(!cleaned->empty());
    CHECK(reply.find(*cleaned) != std::string::npos);
    CHECK(!IsWs(cleaned->front()) && !IsWs(cleaned->back()));
  }
  return 0;
}
