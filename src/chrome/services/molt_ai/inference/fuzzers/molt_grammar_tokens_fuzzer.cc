// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The token rules of constrained decoding (engine/grammar_tokens.h): which
// vocabulary pieces may be sampled under a grammar, and the strict UTF-8
// check across pieces that keeps llama.cpp's grammar accept path from ever
// seeing a token it would reject. Pieces come from the model file's
// vocabulary (untrusted) and arrive in any order the model samples them.
//
// Checks: Utf8Prefix accepts a piece exactly when the text so far plus the
// piece can still become valid UTF-8 without NUL (checked against
// stream_filters.h's IsValidUtf8()); CanAppend() agrees with Append(), and a
// refused Append() changes nothing; incomplete() is set exactly when the
// text ends inside a character; ClassifyGrammarToken() never lets through
// an end-of-generation, control, empty, NUL-carrying or over-long piece.

#include <cstddef>
#include <cstdint>
#include <string>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/grammar_tokens.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai {
namespace {

// Whether some bytes appended to |s| make it valid UTF-8 without NUL. A
// character is at most 4 bytes, so 3 continuation bytes complete any
// prefix; these values cover every range Table 3-7 allows after a lead.
bool CanBecomeValid(const std::string& s) {
  if (s.find('\0') != std::string::npos) return false;
  if (IsValidUtf8(s)) return true;
  static constexpr unsigned char kContinuations[] = {0x80, 0x8F, 0x90,
                                                     0x9F, 0xA0, 0xBF};
  for (unsigned char a : kContinuations) {
    std::string one = s;
    one.push_back(static_cast<char>(a));
    if (IsValidUtf8(one)) return true;
    for (unsigned char b : kContinuations) {
      std::string two = one;
      two.push_back(static_cast<char>(b));
      if (IsValidUtf8(two)) return true;
      for (unsigned char c : kContinuations) {
        std::string three = two;
        three.push_back(static_cast<char>(c));
        if (IsValidUtf8(three)) return true;
      }
    }
  }
  return false;
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai;
  FuzzedDataProvider provider(data, size);

  // A token's facts, as GrammarVocab::Build() gathers them.
  GrammarTokenFacts facts;
  facts.end_of_generation = provider.ConsumeBool();
  facts.control = provider.ConsumeBool();
  facts.user_defined = provider.ConsumeBool();
  const std::string text = provider.ConsumeRandomLengthString(512);
  const std::string piece = provider.ConsumeRandomLengthString(512);
  facts.text = text;
  facts.piece = piece;
  const GrammarToken cls = ClassifyGrammarToken(facts);
  if (facts.end_of_generation) {
    CHECK(cls == GrammarToken::kEndOfGeneration);
  } else if (cls == GrammarToken::kAllowed) {
    CHECK(!facts.control);
    CHECK(!piece.empty() && piece.size() <= kMaxGrammarPieceBytes);
    CHECK(piece.find('\0') == std::string::npos);
  }

  // Pieces in the order a model samples them; a refused one is skipped, as
  // the sampler never accepts it.
  Utf8Prefix prefix;
  // The bytes after the last complete character of the accepted text.
  std::string open;
  while (provider.remaining_bytes() > 0) {
    const std::string next = provider.ConsumeRandomLengthString(64);
    const bool was_incomplete = prefix.incomplete();
    const bool can = prefix.CanAppend(next);
    const bool appended = prefix.Append(next);
    CHECK(can == appended);
    CHECK(appended == CanBecomeValid(open + next))
        << "Utf8Prefix and IsValidUtf8 disagree";
    if (!appended) {
      CHECK(prefix.incomplete() == was_incomplete);
      continue;
    }
    const std::string text_end = open + next;
    open = text_end.substr(Utf8SafePrefixLength(text_end));
    CHECK(prefix.incomplete() == !open.empty());
    CHECK(IsValidUtf8(text_end.substr(0, text_end.size() - open.size())));
  }
  return 0;
}
