// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The converter's grammars (engine/json_schema_grammar.h) in llama.cpp's
// own grammar engine (src/llama-grammar.cpp, the one the service's grammar
// sampler drives), in process.
//
// Input: a schema (FuzzedDataProvider::ConsumeRandomLengthString), then
// bytes that steer a walk through the grammar.
//
// Checks, for every schema the converter accepts:
//  * llama.cpp's GBNF parser accepts the grammar (no parse error, no left
//    recursion): the service treats a refused grammar as a converter bug;
//  * a walk that only ever feeds a code point some grammar stack allows
//    (as the sampler does: valid Unicode scalar values, no NUL) never
//    empties the stacks, and llama.cpp's accept path doesn't crash or blow
//    up: the number of live stacks stays under kMaxStacks;
//  * wherever the grammar could end (a stack is empty: the point where the
//    sampler allows an end-of-generation token), the text so far is strict
//    RFC 8259 JSON (bounded_json.h), as json_schema_grammar.h promises.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/bounded_json.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"
#include "third_party/llama_cpp/src/llama-grammar.h"

namespace molt_ai {
namespace {

// Most code points one walk feeds.
constexpr size_t kMaxWalk = 2048;
// llama.cpp keeps one stack per distinct parse position, and the sampler
// checks candidate tokens against every one. The converter bounds the
// alternatives alive at once (kMaxLiveAlternatives); llama.cpp needs a few
// stacks per alternative (the optional whitespace and repetitions after a
// character): 64 optional properties keep about 150 stacks alive while a
// key is read. Many more would mean a schema can make every sampled token
// cost hundreds of passes over the vocabulary.
constexpr size_t kMaxStacks = 8 * kMaxLiveAlternatives;

struct Range {
  uint32_t lo;
  uint32_t hi;
};

// The code point set at a stack's top (a CHAR / CHAR_NOT / CHAR_ANY run), as
// llama_grammar_match_char() reads it.
bool ReadCharSet(const llama_grammar_element* pos,
                 std::vector<Range>* ranges,
                 bool* negated) {
  if (pos->type != LLAMA_GRETYPE_CHAR && pos->type != LLAMA_GRETYPE_CHAR_NOT &&
      pos->type != LLAMA_GRETYPE_CHAR_ANY) {
    return false;
  }
  *negated = pos->type == LLAMA_GRETYPE_CHAR_NOT;
  ranges->clear();
  do {
    if (pos[1].type == LLAMA_GRETYPE_CHAR_RNG_UPPER) {
      ranges->push_back({pos->value, pos[1].value});
      pos += 2;
    } else if (pos->type == LLAMA_GRETYPE_CHAR_ANY) {
      ranges->push_back({0, 0x10FFFF});
      pos += 1;
    } else {
      ranges->push_back({pos->value, pos->value});
      pos += 1;
    }
  } while (pos->type == LLAMA_GRETYPE_CHAR_ALT);
  return true;
}

bool InRanges(const std::vector<Range>& ranges, uint32_t cp) {
  for (const Range& r : ranges) {
    if (r.lo <= cp && cp <= r.hi) return true;
  }
  return false;
}

// What the sampler can ever feed: Unicode scalar values but NUL
// (Utf8Prefix, engine/grammar_tokens.h).
bool Feedable(uint32_t cp) {
  return cp != 0 && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

// A code point the set allows, steered by |provider|; 0 if none was found.
uint32_t PickCodePoint(const std::vector<Range>& ranges,
                       bool negated,
                       FuzzedDataProvider* provider) {
  if (!negated) {
    const Range& r =
        ranges[provider->ConsumeIntegralInRange<size_t>(0, ranges.size() - 1)];
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t cp = provider->ConsumeIntegralInRange<uint32_t>(r.lo, r.hi);
      if (Feedable(cp)) return cp;
    }
    for (uint32_t cp = r.lo; cp <= r.hi && cp <= 0x10FFFF; ++cp) {
      if (Feedable(cp)) return cp;
    }
    return 0;
  }
  // Mostly printable ASCII (what JSON strings are made of), sometimes any
  // code point, and the edges of the excluded ranges.
  for (int attempt = 0; attempt < 8; ++attempt) {
    uint32_t cp;
    switch (provider->ConsumeIntegralInRange<int>(0, 3)) {
      case 0:
        cp = provider->ConsumeIntegralInRange<uint32_t>(0x20, 0x7E);
        break;
      case 1:
        cp = provider->ConsumeIntegralInRange<uint32_t>(1, 0x10FFFF);
        break;
      default: {
        const Range& r = ranges[provider->ConsumeIntegralInRange<size_t>(
            0, ranges.size() - 1)];
        cp = provider->ConsumeBool() ? r.lo - 1 : r.hi + 1;
        break;
      }
    }
    if (Feedable(cp) && !InRanges(ranges, cp)) return cp;
  }
  return 0;
}

void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// RFC 8259 JSON text, checked strictly except for duplicate object keys:
// the grammar of a map (a schema with "additionalProperties" and no
// "properties") can't rule them out, and RFC 8259 and base::JSONReader
// both accept them. Everything else goes through bounded_json.h's parser.
bool IsRfc8259Json(const std::string& text) {
  JsonLimits limits;
  limits.max_bytes = 1 << 20;
  limits.max_depth = 64;
  limits.max_values = 1 << 20;
  JsonError error = JsonError::kNone;
  if (ParseJson(text, limits, &error)) return true;
  if (error != JsonError::kDuplicateKey) return false;
  // Make every object key unique without changing anything else: append a
  // counter to each key ("k" -> "k\u0001...") and parse again.
  std::string renamed;
  size_t counter = 0;
  bool in_string = false;
  // A string followed (after whitespace) by ':' is a key.
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!in_string) {
      renamed.push_back(c);
      if (c == '"') in_string = true;
      continue;
    }
    if (c == '\\') {
      renamed.push_back(c);
      if (i + 1 < text.size()) renamed.push_back(text[++i]);
      continue;
    }
    if (c != '"') {
      renamed.push_back(c);
      continue;
    }
    in_string = false;
    size_t j = i + 1;
    while (j < text.size() && (text[j] == ' ' || text[j] == '\t' ||
                               text[j] == '\n' || text[j] == '\r')) {
      ++j;
    }
    if (j < text.size() && text[j] == ':') {
      renamed += "\\u0001" + std::to_string(counter++);
    }
    renamed.push_back(c);
  }
  return ParseJson(renamed, limits, &error).has_value();
}

void CheckCompleteValue(const std::string& text) {
  CHECK(IsRfc8259Json(text))
      << "the grammar can end on text that isn't JSON: " << text;
}

void Walk(llama_grammar* grammar, FuzzedDataProvider* provider) {
  std::string text;
  std::vector<Range> ranges;
  std::vector<size_t> open;  // stacks positioned at a character set
  for (size_t step = 0; step <= kMaxWalk; ++step) {
    const llama_grammar_stacks& stacks = grammar->stacks;
    CHECK(!stacks.empty()) << "no grammar stack left after: " << text;
    CHECK(stacks.size() <= kMaxStacks)
        << stacks.size() << " grammar stacks after: " << text;
    open.clear();
    bool can_end = false;
    for (size_t i = 0; i < stacks.size(); ++i) {
      if (stacks[i].empty()) {
        can_end = true;
      } else {
        open.push_back(i);
      }
    }
    if (can_end) CheckCompleteValue(text);
    if (open.empty() || step == kMaxWalk || provider->remaining_bytes() == 0 ||
        (can_end && provider->ConsumeIntegralInRange<int>(0, 7) == 0)) {
      return;
    }
    const size_t pick =
        open[provider->ConsumeIntegralInRange<size_t>(0, open.size() - 1)];
    bool negated = false;
    CHECK(ReadCharSet(stacks[pick].back(), &ranges, &negated))
        << "a stack top that isn't a character set";
    const uint32_t cp = PickCodePoint(ranges, negated, provider);
    if (cp == 0) return;
    llama_grammar_accept(grammar, cp);
    AppendUtf8(cp, &text);
  }
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  const std::string schema =
      provider.ConsumeRandomLengthString(molt_ai::kMaxSchemaBytes);
  const molt_ai::SchemaGrammar result = molt_ai::JsonSchemaToGrammar(schema);
  if (result.status != molt_ai::SchemaGrammarStatus::kOk) {
    return 0;
  }
  // As llama_sampler_init_grammar() parses it (vocab only matters for
  // token references, which the converter never writes).
  llama_grammar* grammar = llama_grammar_init_impl(
      /*vocab=*/nullptr, result.gbnf.c_str(), "root", /*lazy=*/false,
      /*trigger_patterns=*/nullptr, 0, /*trigger_tokens=*/nullptr, 0);
  CHECK(grammar) << "llama.cpp refused the grammar for: " << schema << "\n"
                 << result.gbnf;
  molt_ai::Walk(grammar, &provider);
  llama_grammar_free_impl(grammar);
  return 0;
}
