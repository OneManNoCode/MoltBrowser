// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_ai_steps.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"

namespace molt_ai {
namespace automation {
namespace ai_steps {

namespace {

// Deepest reply: object > list > scalar. A deeper one is refused by the
// reader itself.
constexpr size_t kMaxReplyJsonDepth = 3;
// Longest type name looked at ("decimal[]").
constexpr size_t kMaxTypeNameBytes = 16;

// ---- Text -------------------------------------------------------------------

// Characters in valid UTF-8 (what base::JSONReader returns).
size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  }
  return n;
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

std::string_view TrimAscii(std::string_view s) {
  while (!s.empty() && IsAsciiSpace(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && IsAsciiSpace(s.back()))
    s.remove_suffix(1);
  return s;
}

bool HasControlChars(std::string_view s) {
  return std::any_of(s.begin(), s.end(), [](char ch) {
    const unsigned char c = static_cast<unsigned char>(ch);
    return c < 0x20 || c == 0x7F;
  });
}

// Extracted text as stored: \r dropped, other control characters except
// \n and \t become spaces (the grammar allows \u escapes; a compromised
// service sends anything), ends trimmed.
std::string CleanText(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\r')
      continue;
    if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)
      out += ' ';
    else
      out += ch;
  }
  return std::string(TrimAscii(out));
}

// JSON string literal for the schema text. Field names are checked
// identifiers by now; escape anyway.
std::string Quote(std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += ch;
    } else if (c < 0x20 || c == 0x7F) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    } else {
      out += ch;
    }
  }
  out += '"';
  return out;
}

// One required string property, no others.
std::string SingleStringObjectSchema(std::string_view key,
                                     size_t min_chars,
                                     size_t max_chars) {
  std::string s = "{\"type\":\"object\",\"properties\":{" + Quote(key) +
                  ":{\"type\":\"string\"";
  if (min_chars > 0)
    s += ",\"minLength\":" + base::NumberToString(min_chars);
  return s + ",\"maxLength\":" + base::NumberToString(max_chars) +
         "}},\"required\":[" + Quote(key) + "],\"additionalProperties\":false}";
}

// The reply as an object with exactly one key, |key|, holding a string.
const std::string* SingleString(const std::optional<base::DictValue>& dict,
                                std::string_view key) {
  if (!dict || dict->size() != 1)
    return nullptr;
  return dict->FindString(key);
}

// ---- Replies ----------------------------------------------------------------

// Why |reply| can't be parsed at all ("" if it can), and whether retrying
// can help.
std::string ReplyProblem(const ModelReply& reply, bool* terminal) {
  *terminal = false;
  if (reply.cancelled)
    return "the request was cancelled";
  if (reply.constraint_unsupported) {
    *terminal = true;
    return "the on-device model couldn't use the answer format";
  }
  if (reply.invalid_output) {
    return reply.truncated ? "the answer was cut off"
                           : "the answer wasn't in the required format";
  }
  if (!reply.success) {
    return reply.error_message.empty() ? std::string("no answer")
                                       : reply.error_message;
  }
  return std::string();
}

// ---- Step schema ------------------------------------------------------------

bool IsFieldName(std::string_view name) {
  if (name.empty() || name.size() > kMaxFieldNameBytes)
    return false;
  if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
    return false;  // reserved (__ai_decision, __proto__)
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || c == '_' || (digit && i > 0)))
      return false;
  }
  return true;
}

std::optional<ExtractField> ParseFieldType(std::string_view type) {
  if (type.empty() || type.size() > kMaxTypeNameBytes)
    return std::nullopt;
  std::string t(type);
  for (char& c : t) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  ExtractField field;
  if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) {
    field.list = true;
    t.resize(t.size() - 2);
  }
  if (t == "string" || t == "str" || t == "text") {
    field.type = FieldType::kString;
  } else if (t == "int" || t == "integer") {
    field.type = FieldType::kInteger;
  } else if (t == "number" || t == "float" || t == "double" || t == "decimal") {
    field.type = FieldType::kNumber;
  } else if (t == "bool" || t == "boolean") {
    field.type = FieldType::kBoolean;
  } else {
    return std::nullopt;
  }
  return field;
}

std::string ScalarSchema(FieldType type, bool nullable, size_t max_chars) {
  const char* name = "string";
  switch (type) {
    case FieldType::kString:
      name = "string";
      break;
    case FieldType::kInteger:
      name = "integer";
      break;
    case FieldType::kNumber:
      name = "number";
      break;
    case FieldType::kBoolean:
      name = "boolean";
      break;
  }
  std::string s = nullable
                      ? std::string("{\"type\":[\"") + name + "\",\"null\"]"
                      : std::string("{\"type\":\"") + name + "\"";
  if (type == FieldType::kString)
    s += ",\"maxLength\":" + base::NumberToString(max_chars);
  return s + "}";
}

std::string FieldSchema(const ExtractField& field) {
  if (!field.list)
    return ScalarSchema(field.type, /*nullable=*/true, kMaxExtractStringChars);
  return "{\"type\":[\"array\",\"null\"],\"items\":" +
         ScalarSchema(field.type, /*nullable=*/false,
                      kMaxExtractListStringChars) +
         ",\"maxItems\":" + base::NumberToString(kMaxExtractListItems) + "}";
}

std::string_view TypeDescription(FieldType type) {
  switch (type) {
    case FieldType::kString:
      return "text";
    case FieldType::kInteger:
      return "whole number";
    case FieldType::kNumber:
      return "number";
    case FieldType::kBoolean:
      return "true or false";
  }
  return "";
}

// ---- Extracted values -------------------------------------------------------

// One scalar of |type| as stored, or nullopt (with |error|).
std::optional<base::Value> ReadScalar(const base::Value& v,
                                      FieldType type,
                                      size_t max_chars,
                                      std::string* error) {
  switch (type) {
    case FieldType::kString:
      if (!v.is_string())
        break;
      if (CountChars(v.GetString()) > max_chars) {
        *error = "a text field was too long";
        return std::nullopt;
      }
      return base::Value(CleanText(v.GetString()));
    case FieldType::kInteger:
      if (v.is_int())
        return base::Value(v.GetInt());
      if (v.is_double()) {
        // base::JSONReader reads integers outside int's range as doubles.
        const double d = v.GetDouble();
        if (!std::isfinite(d) || std::trunc(d) != d)
          break;
        if (d >= std::numeric_limits<int>::min() &&
            d <= std::numeric_limits<int>::max()) {
          return base::Value(static_cast<int>(d));
        }
        return base::Value(d);
      }
      break;
    case FieldType::kNumber:
      if (v.is_int())
        return base::Value(v.GetInt());
      if (v.is_double() && std::isfinite(v.GetDouble()))
        return base::Value(v.GetDouble());
      break;
    case FieldType::kBoolean:
      if (v.is_bool())
        return base::Value(v.GetBool());
      break;
  }
  *error = "a field had the wrong type";
  return std::nullopt;
}

std::optional<base::Value> ReadField(const base::Value& v,
                                     const ExtractField& field,
                                     std::string* error) {
  if (v.is_none())
    return base::Value();
  if (!field.list)
    return ReadScalar(v, field.type, kMaxExtractStringChars, error);
  if (!v.is_list()) {
    *error = "a field had the wrong type";
    return std::nullopt;
  }
  const base::ListValue& items = v.GetList();
  if (items.size() > kMaxExtractListItems) {
    *error = "a list field was too long";
    return std::nullopt;
  }
  base::ListValue out;
  for (const base::Value& item : items) {
    std::optional<base::Value> read =
        ReadScalar(item, field.type, kMaxExtractListStringChars, error);
    if (!read)
      return std::nullopt;
    out.Append(std::move(*read));
  }
  return base::Value(std::move(out));
}

}  // namespace

// ---------------------------------------------------------------------------
// AI_DECIDE
// ---------------------------------------------------------------------------

std::string BuildDecisionSchema() {
  return "{\"type\":\"object\",\"properties\":{\"decision\":"
         "{\"enum\":[\"SUCCESS\",\"FAIL\"]}},\"required\":[\"decision\"],"
         "\"additionalProperties\":false}";
}

std::optional<bool> ParseDecision(std::string_view json) {
  const std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      json, base::JSON_PARSE_RFC, kMaxReplyJsonDepth);
  const std::string* d = SingleString(dict, "decision");
  if (!d)
    return std::nullopt;
  if (*d == "SUCCESS")
    return true;
  if (*d == "FAIL")
    return false;
  return std::nullopt;
}

DecisionOutcome InterpretDecision(const ModelReply& reply) {
  DecisionOutcome out;
  std::string problem = ReplyProblem(reply, &out.terminal);
  if (problem.empty()) {
    out.decision = ParseDecision(reply.text);
    if (out.decision)
      return out;
    problem = "the answer wasn't in the required format";
  }
  out.note = "AI decision failed: " + problem;
  return out;
}

// ---------------------------------------------------------------------------
// AI_EXTRACT
// ---------------------------------------------------------------------------

ExtractField::ExtractField() = default;
ExtractField::ExtractField(std::string name, FieldType type, bool list)
    : name(std::move(name)), type(type), list(list) {}
ExtractField::~ExtractField() = default;
ExtractField::ExtractField(const ExtractField&) = default;
ExtractField& ExtractField::operator=(const ExtractField&) = default;
ExtractField::ExtractField(ExtractField&&) = default;
ExtractField& ExtractField::operator=(ExtractField&&) = default;

ExtractSpec::ExtractSpec() = default;
ExtractSpec::~ExtractSpec() = default;
ExtractSpec::ExtractSpec(const ExtractSpec&) = default;
ExtractSpec& ExtractSpec::operator=(const ExtractSpec&) = default;
ExtractSpec::ExtractSpec(ExtractSpec&&) = default;
ExtractSpec& ExtractSpec::operator=(ExtractSpec&&) = default;

ExtractSpecResult::ExtractSpecResult() = default;
ExtractSpecResult::~ExtractSpecResult() = default;
ExtractSpecResult::ExtractSpecResult(ExtractSpecResult&&) = default;
ExtractSpecResult& ExtractSpecResult::operator=(ExtractSpecResult&&) = default;

ExtractOutcome::ExtractOutcome() = default;
ExtractOutcome::~ExtractOutcome() = default;
ExtractOutcome::ExtractOutcome(ExtractOutcome&&) = default;
ExtractOutcome& ExtractOutcome::operator=(ExtractOutcome&&) = default;

ExtractSpecResult ExtractSpecFromStep(const base::Value& step_extra) {
  ExtractSpecResult result;
  const base::Value* schema =
      step_extra.is_dict() ? step_extra.GetDict().Find(kSchemaKey) : nullptr;
  if (!schema) {
    result.spec = ExtractSpec();
    return result;
  }
  if (!schema->is_dict()) {
    result.error =
        "the step's \"schema\" must be an object of field names and types, "
        "like {\"price\": \"int\", \"airline\": \"string\"}";
    return result;
  }
  const base::DictValue& dict = schema->GetDict();
  // Checked before anything is read: the size is O(1).
  if (dict.empty()) {
    result.error = "the step's \"schema\" has no fields";
    return result;
  }
  if (dict.size() > kMaxExtractFields) {
    result.error = "the step's \"schema\" has more than " +
                   base::NumberToString(kMaxExtractFields) + " fields";
    return result;
  }
  ExtractSpec spec;
  for (const auto& [name, type] : dict) {
    if (!IsFieldName(name)) {
      result.error =
          "a field name in the step's \"schema\" isn't usable (letters, "
          "digits and _, at most 64, not starting with a digit or __)";
      return result;
    }
    std::optional<ExtractField> field =
        type.is_string() ? ParseFieldType(type.GetString()) : std::nullopt;
    if (!field) {
      result.error =
          "a field type in the step's \"schema\" isn't one of string, int, "
          "number or bool, or a list of one (like \"string[]\")";
      return result;
    }
    field->name = name;
    spec.fields.push_back(std::move(*field));
  }
  std::sort(spec.fields.begin(), spec.fields.end(),
            [](const ExtractField& a, const ExtractField& b) {
              return a.name < b.name;
            });
  result.spec = std::move(spec);
  return result;
}

std::string BuildExtractSchema(const ExtractSpec& spec) {
  if (spec.fields.empty()) {
    return SingleStringObjectSchema("answer", 0, kMaxExtractAnswerChars);
  }
  std::string properties;
  std::string required;
  for (const ExtractField& field : spec.fields) {
    if (!properties.empty()) {
      properties += ',';
      required += ',';
    }
    properties += Quote(field.name) + ":" + FieldSchema(field);
    required += Quote(field.name);
  }
  return "{\"type\":\"object\",\"properties\":{" + properties +
         "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

std::string DescribeExtractFields(const ExtractSpec& spec) {
  std::string out;
  for (const ExtractField& field : spec.fields) {
    out += "- " + field.name + ": ";
    if (field.list)
      out += "list of " + std::string(TypeDescription(field.type)) + " values";
    else
      out += std::string(TypeDescription(field.type));
    out += ", or null\n";
  }
  return out;
}

ExtractOutcome InterpretExtract(const ModelReply& reply,
                                const ExtractSpec& spec) {
  ExtractOutcome out;
  std::string problem = ReplyProblem(reply, &out.terminal);
  if (problem.empty()) {
    std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
        reply.text, base::JSON_PARSE_RFC, kMaxReplyJsonDepth);
    if (spec.fields.empty()) {
      const std::string* answer = SingleString(dict, "answer");
      if (answer && CountChars(*answer) <= kMaxExtractAnswerChars) {
        out.value = base::Value(CleanText(*answer));
        return out;
      }
      problem = answer ? "the answer was too long"
                       : "the answer wasn't in the required format";
    } else if (!dict || dict->size() != spec.fields.size()) {
      problem = "the answer wasn't in the required format";
    } else {
      base::DictValue values;
      for (const ExtractField& field : spec.fields) {
        const base::Value* v = dict->Find(field.name);
        if (!v) {
          problem = "the answer wasn't in the required format";
          break;
        }
        std::optional<base::Value> read = ReadField(*v, field, &problem);
        if (!read)
          break;
        values.Set(field.name, std::move(*read));
      }
      if (problem.empty()) {
        out.value = base::Value(std::move(values));
        return out;
      }
    }
  }
  out.note = "AI extract failed: " + problem;
  return out;
}

// ---------------------------------------------------------------------------
// Selector recovery
// ---------------------------------------------------------------------------

std::string BuildSelectorSchema() {
  return SingleStringObjectSchema("selector", 1, kMaxSelectorChars);
}

std::optional<std::string> ParseSelectorReply(const ModelReply& reply) {
  bool terminal = false;
  if (!ReplyProblem(reply, &terminal).empty())
    return std::nullopt;
  const std::optional<base::DictValue> dict = base::JSONReader::ReadDict(
      reply.text, base::JSON_PARSE_RFC, kMaxReplyJsonDepth);
  const std::string* selector = SingleString(dict, "selector");
  if (!selector || CountChars(*selector) > kMaxSelectorChars ||
      HasControlChars(*selector)) {
    return std::nullopt;
  }
  std::string_view trimmed = TrimAscii(*selector);
  if (trimmed.empty())
    return std::nullopt;
  return std::string(trimmed);
}

}  // namespace ai_steps
}  // namespace automation
}  // namespace molt_ai
