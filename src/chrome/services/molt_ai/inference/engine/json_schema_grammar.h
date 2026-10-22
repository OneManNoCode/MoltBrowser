// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// JSON Schema -> GBNF, for constrained decoding in the sandboxed inference
// service (docs/design/SANDBOXED_INFERENCE.md section 9). The schema comes
// from a web page (LanguageModel.prompt(..., {responseConstraint})) or from
// MoltBrowser's own callers; either way it is untrusted input here. The
// grammar it produces drives llama.cpp's grammar sampler, so the model's
// visible answer is JSON matching the schema.
//
// llama.cpp's own converter (common/json-schema-to-grammar.cpp) needs
// nlohmann::json and exceptions and isn't built; this one is std-only,
// linear in the input, and bounded in everything it produces.
//
// Supported (anything else is refused, never approximated):
//  * true / {} (any JSON value, nested at most kAnyValueDepth deep).
//  * "type": one of object, array, string, number, integer, boolean, null,
//    or a list of them. Without "type", the shape keywords decide (only one
//    family may be used); with none, any value.
//  * object: "properties" (at most kMaxProperties; generated in the order
//    given), "required" (names from "properties"), "additionalProperties":
//    false / true / absent (no other keys are generated), or a schema when
//    there are no "properties" (a map with string keys).
//  * array: "items" (one schema), "minItems" / "maxItems" up to
//    kMaxItemsBound.
//  * string: "minLength" / "maxLength" up to kMaxLengthBound (characters as
//    written; an escape such as \n counts as one).
//  * integer: "minimum" / "maximum" / "exclusiveMinimum" /
//    "exclusiveMaximum" (numbers) giving a range of at most kMaxRangeSize
//    values, or a lower bound of 0 or 1 alone.
//  * number: "minimum": 0 alone, or integer "minimum" and "maximum" with
//    0 <= minimum < maximum and a span of at most kMaxRangeSize.
//  * "enum" (at most kMaxEnumValues) and "const", optionally with "type"
//    (values of other types are dropped). The value is written compactly,
//    exactly as JSON.stringify would.
//  * "anyOf" / "oneOf" of the above (oneOf is treated as anyOf; nothing
//    checks that only one branch matches), on their own.
//  * Annotations are ignored: title, description, default, examples,
//    $comment, $schema, $id, deprecated, readOnly, writeOnly, $defs and
//    definitions (unused, since $ref is refused).
// Refused: $ref (so no recursion), pattern, format, allOf, not, if/then/
// else, patternProperties, prefixItems, uniqueItems: true, multipleOf, ...
// and any keyword not listed above.
//
// Bounds: the input (kMaxSchemaBytes, kMaxSchemaValues, JSON nesting), the
// schema nesting (kMaxSchemaDepth), the grammar's size (kMaxGrammarBytes,
// kMaxGrammarRules, kMaxRepetitionBudget) and how many alternatives can be
// alive at once while the sampler reads one value (kMaxLiveAlternatives;
// each costs a pass over the vocabulary per sampled token). llama.cpp's
// grammar parser recurses on nested parentheses; the grammar uses at most
// two levels, and its rules never start with a reference to themselves.
//
// The output is ASCII GBNF (non-ASCII characters are \u / \U escapes) with
// start rule "root", for llama_sampler_init_grammar(). Whitespace between
// tokens is bounded (a space, or a newline and up to 20 spaces or tabs),
// numbers have at most 16 integer and 16 fraction digits and a 2-digit
// exponent, and \u escapes exclude surrogates, so every complete match is
// RFC 8259 JSON that base::JSONReader accepts.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_JSON_SCHEMA_GRAMMAR_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_JSON_SCHEMA_GRAMMAR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace molt_ai {

inline constexpr size_t kMaxSchemaBytes = 32 * 1024;
inline constexpr size_t kMaxSchemaValues = 4096;
inline constexpr size_t kMaxSchemaJsonDepth = 48;
inline constexpr size_t kMaxSchemaDepth = 12;
inline constexpr size_t kMaxProperties = 64;
inline constexpr size_t kMaxEnumValues = 256;
inline constexpr size_t kMaxEnumValueBytes = 1024;
inline constexpr size_t kMaxAnyOf = 16;
inline constexpr uint32_t kMaxItemsBound = 64;
inline constexpr uint32_t kMaxLengthBound = 1024;
inline constexpr int64_t kMaxRangeSize = 1001;
inline constexpr size_t kAnyValueDepth = 3;
inline constexpr size_t kMaxLiveAlternatives = 64;
inline constexpr size_t kMaxGrammarBytes = 128 * 1024;
inline constexpr size_t kMaxGrammarRules = 2048;
// Sum over the grammar of the upper bounds of counted repetitions ({m,n}),
// which llama.cpp's parser expands into one rule each.
inline constexpr size_t kMaxRepetitionBudget = 8192;

enum class SchemaGrammarStatus {
  kOk,
  // Not strict JSON, or a schema that is neither an object nor a boolean.
  kInvalidJson,
  // Over an input or output size cap.
  kTooLarge,
  // Nested deeper than kMaxSchemaDepth (or the JSON nesting cap).
  kTooDeep,
  // More than kMaxLiveAlternatives alternatives could be alive at once.
  kTooAmbiguous,
  // A keyword or form outside the supported subset.
  kUnsupported,
  // No JSON value matches (false, an empty enum, minItems > maxItems, ...).
  kUnsatisfiable,
};

struct SchemaGrammar {
  SchemaGrammarStatus status = SchemaGrammarStatus::kInvalidJson;
  // GBNF, start rule "root". Set when status is kOk.
  std::string gbnf;
  // Why it was refused, for tests and logs. A fixed text that may name a
  // JSON Schema keyword; never contains text from the schema.
  std::string reason;
};

SchemaGrammar JsonSchemaToGrammar(std::string_view schema_json);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_JSON_SCHEMA_GRAMMAR_H_
