// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Agent mode's model decisions (automation/automation_ai_steps.h): the
// AI_EXTRACT field list from a workflow file and what the sandboxed model
// answers to AI_DECIDE, AI_EXTRACT and selector recovery. Both are
// untrusted; a stored value later reaches pages (typed, navigated to).
//
// Checks: every schema these steps build is one the inference service
// supports; a decision is read only from exactly {"decision": "SUCCESS" |
// "FAIL"}; an extracted value has exactly the spec's fields, each of its
// type (or null) within its caps; a free-form value stays within every cap
// (depth, keys, values, key names, string lengths, no "__" keys, no control
// characters but \n and \t); a recovered selector is one trimmed line.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_ai_steps.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

namespace molt_ai::automation::ai_steps {
namespace {

size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

bool CleanString(std::string_view s) {
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) return false;
  }
  return s.empty() || (s.front() != ' ' && s.back() != ' ' &&
                       s.front() != '\n' && s.back() != '\n' &&
                       s.front() != '\t' && s.back() != '\t');
}

struct Budget {
  size_t keys = 0;
  size_t values = 0;
};

void CheckFreeform(const base::Value& v, size_t depth, Budget* budget) {
  ++budget->values;
  if (v.is_dict()) {
    CHECK(depth <= kMaxFreeformDepth);
    for (const auto& [key, value] : v.GetDict()) {
      ++budget->keys;
      CHECK(!key.empty() && key.size() <= kMaxFreeformKeyBytes);
      CHECK(!key.starts_with("__")) << "a reserved key was stored";
      for (char c : key) {
        CHECK((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ');
      }
      CHECK(key.front() != ' ' && key.back() != ' ');
      CheckFreeform(value, depth + 1, budget);
    }
  } else if (v.is_list()) {
    CHECK(depth <= kMaxFreeformDepth);
    for (const base::Value& item : v.GetList()) {
      CheckFreeform(item, depth + 1, budget);
    }
  } else if (v.is_string()) {
    CHECK(CountChars(v.GetString()) <= kMaxFreeformStringChars);
    CHECK(CleanString(v.GetString())) << "an uncleaned string was stored";
  } else if (v.is_double()) {
    CHECK(std::isfinite(v.GetDouble()));
  }
}

void CheckExtracted(const base::Value& value, const ExtractSpec& spec) {
  CHECK(value.is_dict());
  if (spec.fields.empty()) {
    Budget budget;
    CheckFreeform(value, 1, &budget);
    CHECK(budget.keys <= kMaxFreeformKeys);
    CHECK(budget.values <= kMaxFreeformValues);
    return;
  }
  const base::DictValue& dict = value.GetDict();
  CHECK(dict.size() == spec.fields.size());
  for (const ExtractField& field : spec.fields) {
    const base::Value* v = dict.Find(field.name);
    CHECK(v) << "a spec field is missing";
    if (v->is_none()) continue;
    auto check_scalar = [&](const base::Value& s, size_t max_chars) {
      switch (field.type) {
        case FieldType::kString:
          CHECK(s.is_string() && CountChars(s.GetString()) <= max_chars);
          CHECK(CleanString(s.GetString()));
          break;
        case FieldType::kInteger:
          CHECK(s.is_int() ||
                (s.is_double() && std::trunc(s.GetDouble()) == s.GetDouble()));
          break;
        case FieldType::kNumber:
          CHECK(s.is_int() || (s.is_double() && std::isfinite(s.GetDouble())));
          break;
        case FieldType::kBoolean:
          CHECK(s.is_bool());
          break;
      }
    };
    if (field.list) {
      CHECK(v->is_list() && v->GetList().size() <= kMaxExtractListItems);
      for (const base::Value& item : v->GetList()) {
        check_scalar(item, kMaxExtractListStringChars);
      }
    } else {
      check_scalar(*v, kMaxExtractStringChars);
    }
  }
}

ModelReply Reply(FuzzedDataProvider* p) {
  ModelReply reply;
  reply.success = p->ConsumeIntegralInRange<int>(0, 7) != 0;
  reply.cancelled = p->ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.constraint_unsupported = p->ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.invalid_output = p->ConsumeIntegralInRange<int>(0, 15) == 0;
  reply.truncated = p->ConsumeBool();
  reply.text = p->ConsumeRandomLengthString(kMaxFreeformReplyBytes + 64);
  return reply;
}

void CheckSchema(const std::string& schema) {
  const SchemaGrammar g = JsonSchemaToGrammar(schema);
  CHECK(g.status == SchemaGrammarStatus::kOk)
      << "the service would refuse: " << g.reason << "\n" << schema;
}

}  // namespace
}  // namespace molt_ai::automation::ai_steps

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::automation::ai_steps;
  FuzzedDataProvider p(data, size);

  // The step's "extra" from the workflow file.
  std::optional<base::Value> extra =
      base::JSONReader::Read(p.ConsumeRandomLengthString(4096));
  const ExtractSpecResult spec =
      ExtractSpecFromStep(extra ? std::move(*extra) : base::Value());
  CHECK(spec.spec.has_value() != !spec.error.empty());
  ExtractSpec use;
  if (spec.spec) {
    CHECK(spec.spec->fields.size() <= kMaxExtractFields);
    use = *spec.spec;
  }
  CheckSchema(BuildExtractSchema(use));
  CheckSchema(BuildDecisionSchema());
  CheckSchema(BuildSelectorSchema());
  DescribeExtractFields(use);

  const ModelReply decide = Reply(&p);
  const std::optional<bool> decision = ParseDecision(decide.text);
  const DecisionOutcome outcome = InterpretDecision(decide);
  if (outcome.decision) {
    CHECK(decide.success && !decide.cancelled && !decide.invalid_output &&
          !decide.constraint_unsupported);
    CHECK(outcome.decision == decision);
  } else {
    CHECK(!outcome.note.empty());
  }
  if (decision) {
    std::optional<base::DictValue> dict = base::JSONReader::ReadDict(decide.text);
    CHECK(dict && dict->size() == 1);
  }

  const ModelReply extract = Reply(&p);
  ExtractOutcome extracted = InterpretExtract(extract, use);
  if (extracted.value) {
    CHECK(extract.success && !extract.cancelled);
    CheckExtracted(*extracted.value, use);
  } else {
    CHECK(!extracted.note.empty());
  }

  const ModelReply selector = Reply(&p);
  const std::optional<std::string> sel = ParseSelectorReply(selector);
  if (sel) {
    CHECK(!sel->empty() && CountChars(*sel) <= kMaxSelectorChars);
    CHECK(sel->find('\n') == std::string::npos);
    CHECK(CleanString(*sel));
  }
  return 0;
}
