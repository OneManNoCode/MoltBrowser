// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// JSON Schema -> GBNF (engine/json_schema_grammar.h) on untrusted schemas:
// a web page's LanguageModel responseConstraint, or a schema built from an
// imported workflow. The whole input is the schema text.
//
// Checks: no memory or UB error, no timeout (the converter is linear); a
// grammar is ASCII, has a "root" rule and stays within kMaxGrammarBytes and
// kMaxGrammarRules; a refusal has a short fixed-looking reason and no
// grammar; the result is the same on a second call.
// molt_json_schema_grammar_llama_fuzzer feeds the grammars to llama.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view schema(reinterpret_cast<const char*>(data), size);
  const molt_ai::SchemaGrammar result = molt_ai::JsonSchemaToGrammar(schema);

  if (result.status == molt_ai::SchemaGrammarStatus::kOk) {
    const std::string& gbnf = result.gbnf;
    CHECK(!gbnf.empty());
    CHECK(gbnf.size() <= molt_ai::kMaxGrammarBytes);
    CHECK(gbnf.rfind("root ::= ", 0) == 0 ||
          gbnf.find("\nroot ::= ") != std::string::npos);
    size_t rules = 0;
    for (size_t i = 0; i < gbnf.size(); ++i) {
      const auto c = static_cast<unsigned char>(gbnf[i]);
      // ASCII only (non-ASCII characters are written as \u / \U escapes),
      // one rule per line.
      CHECK(c < 0x80 && (c >= 0x20 || c == '\n')) << "byte " << int{c};
      if (gbnf.compare(i, 5, " ::= ") == 0) ++rules;
    }
    CHECK(rules >= 1 && rules <= molt_ai::kMaxGrammarRules);
  } else {
    CHECK(result.gbnf.empty());
    CHECK(!result.reason.empty() && result.reason.size() < 200);
    for (const char ch : result.reason) {
      const auto c = static_cast<unsigned char>(ch);
      CHECK(c >= 0x20 && c < 0x7F);
    }
  }

  const molt_ai::SchemaGrammar again = molt_ai::JsonSchemaToGrammar(schema);
  CHECK(again.status == result.status);
  CHECK(again.gbnf == result.gbnf);
  CHECK(again.reason == result.reason);
  return 0;
}
