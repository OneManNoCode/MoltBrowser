// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The inference service's strict JSON reader (engine/bounded_json.h) on
// untrusted text: the JSON Schemas web pages pass as responseConstraint.
//
// Checks, on every input: no memory or UB error; the limits hold (bytes,
// depth, values); a parsed value writes back to compact JSON that parses to
// the same value, and writing that again gives the same text; JsonQuote()
// of any decoded string reads back as that string.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/bounded_json.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

namespace molt_ai {
namespace {

// Large enough for anything WriteJson() makes of an accepted input.
JsonLimits Unlimited() {
  JsonLimits limits;
  limits.max_bytes = 16 * 1024 * 1024;
  limits.max_depth = 1024;
  limits.max_values = 1u << 24;
  return limits;
}

bool Equal(const JsonValue& a, const JsonValue& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case JsonValue::Type::kNull:
      return true;
    case JsonValue::Type::kBool:
      return a.boolean == b.boolean;
    case JsonValue::Type::kNumber:
    case JsonValue::Type::kString:
      return a.text == b.text;
    case JsonValue::Type::kArray:
      if (a.items.size() != b.items.size()) return false;
      for (size_t i = 0; i < a.items.size(); ++i) {
        if (!Equal(a.items[i], b.items[i])) return false;
      }
      return true;
    case JsonValue::Type::kObject:
      if (a.members.size() != b.members.size()) return false;
      for (size_t i = 0; i < a.members.size(); ++i) {
        if (a.members[i].first != b.members[i].first ||
            !Equal(a.members[i].second, b.members[i].second)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

// Values (containers included) and the deepest container nesting.
void Measure(const JsonValue& v, size_t depth, size_t* values,
             size_t* max_depth) {
  ++*values;
  if (v.is_array() || v.is_object()) {
    ++depth;
    if (depth > *max_depth) *max_depth = depth;
  }
  for (const JsonValue& item : v.items) {
    Measure(item, depth, values, max_depth);
  }
  for (const auto& member : v.members) {
    Measure(member.second, depth, values, max_depth);
  }
}

void CheckStringsRoundTrip(const JsonValue& v) {
  if (v.is_string()) {
    std::optional<JsonValue> back = ParseJson(JsonQuote(v.text), Unlimited());
    CHECK(back && back->is_string() && back->text == v.text);
  }
  for (const JsonValue& item : v.items) CheckStringsRoundTrip(item);
  for (const auto& member : v.members) {
    std::optional<JsonValue> key =
        ParseJson(JsonQuote(member.first), Unlimited());
    CHECK(key && key->is_string() && key->text == member.first);
    CheckStringsRoundTrip(member.second);
  }
}

}  // namespace
}  // namespace molt_ai

namespace molt_ai {
namespace {

void CheckOne(const std::string& text, const JsonLimits& limits) {
  JsonError error = JsonError::kSyntax;
  std::optional<JsonValue> value = ParseJson(text, limits, &error);
  if (!value) {
    CHECK(error != JsonError::kNone);
    return;
  }
  CHECK(error == JsonError::kNone);
  CHECK(text.size() <= limits.max_bytes);
  size_t values = 0;
  size_t depth = 0;
  Measure(*value, 0, &values, &depth);
  CHECK(values <= limits.max_values);
  CHECK(depth <= limits.max_depth);

  const std::string written = WriteJson(*value);
  std::optional<JsonValue> reread = ParseJson(written, Unlimited(), &error);
  CHECK(reread) << "WriteJson output does not parse";
  CHECK(Equal(*value, *reread)) << "round trip changed the value";
  CHECK(WriteJson(*reread) == written);
  CheckStringsRoundTrip(*value);
}

}  // namespace
}  // namespace molt_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // The whole input is the text, so plain JSON files work as seeds.
  const std::string text(reinterpret_cast<const char*>(data), size);
  // The service's limits for schemas (json_schema_grammar.cc) ...
  molt_ai::JsonLimits limits;
  limits.max_bytes = molt_ai::kMaxSchemaBytes;
  limits.max_depth = molt_ai::kMaxSchemaJsonDepth;
  limits.max_values = molt_ai::kMaxSchemaValues;
  molt_ai::CheckOne(text, limits);
  // ... and small ones derived from the size, so every cap is reached.
  molt_ai::JsonLimits small;
  small.max_bytes = size - size % 7;
  small.max_depth = size % 5;
  small.max_values = size % 13;
  molt_ai::CheckOne(text, small);
  return 0;
}
