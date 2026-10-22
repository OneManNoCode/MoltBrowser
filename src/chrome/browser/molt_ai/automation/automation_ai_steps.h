// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The answer formats of AutomationRunner's model decisions
// (docs/design/AGENT_ACTIONS.md): AI_DECIDE, AI_EXTRACT and the selector
// recovery fallback. Each asks the on-device model for one JSON object
// under grammar-constrained decoding (PromptOptions::json_schema) and
// reads the reply back strictly with base::JSONReader. The reply comes
// from the sandboxed inference process, which the browser treats as
// hostile, so nothing here relies on the grammar having held.
//
// AI_EXTRACT's field list comes from the step's "schema" (Step::extra),
// e.g. {"price": "int", "airline": "string"}. It is part of the workflow
// file, which may have been imported, so it is converted here with caps
// and refused, never approximated, when it is outside them.
//
// A reply that can't be used fails the step (and its retries); nothing
// is stored. In particular raw model text is never stored in a variable:
// later steps may type variables into pages or navigate with them.
//
// Pure logic (base::Value, no content/), unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_AI_STEPS_H_
#define CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_AI_STEPS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace molt_ai {
namespace automation {
namespace ai_steps {

// The parts of a GenerationResult (browser_ai_runtime.h) the steps need.
struct ModelReply {
  ModelReply();
  ~ModelReply();
  ModelReply(const ModelReply&);
  ModelReply& operator=(const ModelReply&);
  ModelReply(ModelReply&&);
  ModelReply& operator=(ModelReply&&);

  bool success = false;
  bool cancelled = false;
  bool constraint_unsupported = false;
  bool invalid_output = false;
  bool truncated = false;
  std::string text;
  std::string error_message;
};

// ---------------------------------------------------------------------------
// AI_DECIDE
// ---------------------------------------------------------------------------

inline constexpr int kMaxDecisionTokens = 32;

// {"decision": "SUCCESS" | "FAIL"}, nothing else.
std::string BuildDecisionSchema();

// The decision in a reply, read exactly: an object with the single key
// "decision" whose value is "SUCCESS" (true) or "FAIL" (false).
std::optional<bool> ParseDecision(std::string_view json);

struct DecisionOutcome {
  std::optional<bool> decision;
  // Retrying can't help (the service refused the format): stop the run.
  bool terminal = false;
  // Step note when there is no decision. Fixed text.
  std::string note;
};
DecisionOutcome InterpretDecision(const ModelReply& reply);

// ---------------------------------------------------------------------------
// AI_EXTRACT
// ---------------------------------------------------------------------------

// Step::extra key holding the field list.
inline constexpr char kSchemaKey[] = "schema";

inline constexpr size_t kMaxExtractFields = 16;
inline constexpr size_t kMaxFieldNameBytes = 64;
inline constexpr size_t kMaxExtractStringChars = 256;
inline constexpr size_t kMaxExtractListItems = 16;
inline constexpr size_t kMaxExtractListStringChars = 128;
// Without a schema the step asks for one text answer.
inline constexpr size_t kMaxExtractAnswerChars = 1000;
inline constexpr int kMaxExtractTokens = 1024;

enum class FieldType { kString, kInteger, kNumber, kBoolean };

struct ExtractField {
  ExtractField();
  ExtractField(std::string name, FieldType type, bool list);
  ~ExtractField();
  ExtractField(const ExtractField&);
  ExtractField& operator=(const ExtractField&);
  ExtractField(ExtractField&&);
  ExtractField& operator=(ExtractField&&);

  // Letters, digits and '_', not starting with a digit or "__".
  std::string name;
  FieldType type = FieldType::kString;
  // A list of |type| (written "string[]" in the step's schema).
  bool list = false;
};

struct ExtractSpec {
  ExtractSpec();
  ~ExtractSpec();
  ExtractSpec(const ExtractSpec&);
  ExtractSpec& operator=(const ExtractSpec&);
  ExtractSpec(ExtractSpec&&);
  ExtractSpec& operator=(ExtractSpec&&);

  // Sorted by name. Empty: no schema, the answer is one piece of text.
  std::vector<ExtractField> fields;
};

struct ExtractSpecResult {
  ExtractSpecResult();
  ~ExtractSpecResult();
  ExtractSpecResult(ExtractSpecResult&&);
  ExtractSpecResult& operator=(ExtractSpecResult&&);

  std::optional<ExtractSpec> spec;
  // Why the step's schema was refused (fixed text, never the schema's own
  // names or values).
  std::string error;
};

// Reads |step_extra|["schema"] (untrusted: from the .molt file). Absent
// (or no extra dict at all): no fields. Otherwise it must be an object of
// 1 to kMaxExtractFields field names, each mapped to a type name:
// string | str | text, int | integer, number | float | double | decimal,
// bool | boolean (any case), or one of those followed by "[]" for a list.
ExtractSpecResult ExtractSpecFromStep(const base::Value& step_extra);

// The JSON Schema for a reply: every field required (null allowed, for
// "not on the page"), no other keys, strings and lists capped; or
// {"answer": string} without fields. Always inside the inference
// service's supported subset and caps (checked by the unit tests).
std::string BuildExtractSchema(const ExtractSpec& spec);

// "- price: whole number, or null\n" lines for the prompt.
std::string DescribeExtractFields(const ExtractSpec& spec);

struct ExtractOutcome {
  ExtractOutcome();
  ~ExtractOutcome();
  ExtractOutcome(ExtractOutcome&&);
  ExtractOutcome& operator=(ExtractOutcome&&);

  // What to store: a dict of exactly the spec's fields (each of its type,
  // or none), or the answer string without fields.
  std::optional<base::Value> value;
  bool terminal = false;
  std::string note;
};

// Strings are cleaned before they are stored: carriage returns dropped,
// other control characters (but \n and \t) turned into spaces, ends
// trimmed. Integral doubles become ints when they fit.
ExtractOutcome InterpretExtract(const ModelReply& reply,
                                const ExtractSpec& spec);

// ---------------------------------------------------------------------------
// Selector recovery (the runner's last resort for a lost element)
// ---------------------------------------------------------------------------

inline constexpr size_t kMaxSelectorChars = 200;
inline constexpr int kMaxSelectorTokens = 96;

// {"selector": string of 1 to kMaxSelectorChars characters}.
std::string BuildSelectorSchema();

// The selector in a usable reply: one line, no control characters, ends
// trimmed. nullopt otherwise.
std::optional<std::string> ParseSelectorReply(const ModelReply& reply);

}  // namespace ai_steps
}  // namespace automation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AUTOMATION_AUTOMATION_AI_STEPS_H_
