// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A small, strict JSON reader and writer for untrusted text inside the
// sandboxed inference service: the JSON Schemas web pages pass as
// LanguageModel responseConstraint (json_schema_grammar.h). Std-only, so it
// is unit-tested standalone.
//
// Strict RFC 8259: UTF-8 only (no BOM, no overlong forms, no surrogates),
// no comments or trailing commas, no control characters in strings, lone
// surrogate escapes refused, duplicate object keys refused. Linear time;
// the recursion depth, the number of values and the input size are capped
// before anything is allocated for them.
//
// The browser never uses this on what the service sends back: it parses
// output with base::JSONReader, which is memory-safe (Rust).

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_BOUNDED_JSON_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_BOUNDED_JSON_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molt_ai {

struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  JsonValue();
  ~JsonValue();
  JsonValue(const JsonValue&);
  JsonValue& operator=(const JsonValue&);
  JsonValue(JsonValue&&);
  JsonValue& operator=(JsonValue&&);

  bool is_null() const { return type == Type::kNull; }
  bool is_bool() const { return type == Type::kBool; }
  bool is_number() const { return type == Type::kNumber; }
  bool is_string() const { return type == Type::kString; }
  bool is_array() const { return type == Type::kArray; }
  bool is_object() const { return type == Type::kObject; }

  // The member called |key| of an object, or nullptr.
  const JsonValue* Find(std::string_view key) const;

  Type type = Type::kNull;
  bool boolean = false;
  // kString: the decoded value (valid UTF-8, may contain NUL).
  // kNumber: the literal as written (RFC 8259 number syntax).
  std::string text;
  // kArray.
  std::vector<JsonValue> items;
  // kObject, in source order; keys are unique.
  std::vector<std::pair<std::string, JsonValue>> members;
};

struct JsonLimits {
  size_t max_bytes = 32 * 1024;
  // Arrays and objects nested deeper than this are refused.
  size_t max_depth = 32;
  // Values of any kind, containers included.
  size_t max_values = 4096;
};

enum class JsonError {
  kNone,
  kTooLarge,
  kTooDeep,
  kTooManyValues,
  kSyntax,
  kDuplicateKey,
};

// Parses |text| as one JSON value (surrounding whitespace allowed).
// nullopt on any error; |error| (optional) says which.
std::optional<JsonValue> ParseJson(std::string_view text,
                                   const JsonLimits& limits,
                                   JsonError* error = nullptr);

// Compact JSON, as JavaScript's JSON.stringify writes it: no whitespace;
// strings escape only '"', '\' and control characters (\b \f \n \r \t, else
// \u00xx); numbers as written. ParseJson(WriteJson(v)) == v.
std::string WriteJson(const JsonValue& value);

// |s| as a JSON string literal (quotes included), escaped as above.
std::string JsonQuote(std::string_view s);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_BOUNDED_JSON_H_
