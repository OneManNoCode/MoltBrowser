// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/agent_tools.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"

namespace molt_ai {
namespace agent_tools {

namespace {

using agent_safety::FieldKind;

constexpr char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD
constexpr char kEllipsis[] = "\xE2\x80\xA6";     // U+2026

// An input schema's nesting: root > "properties" > argument > "items" >
// "enum". Annotations may nest a little more; deeper is refused.
constexpr size_t kMaxSchemaJsonDepth = 10;

// Bytes a field of the page's list may take before it is even counted
// (a character is at most 4 bytes; one more character shows it was cut).
constexpr size_t BytesFor(size_t chars) {
  return 4 * (chars + 1);
}

// ---- Text -------------------------------------------------------------------

bool IsContinuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Characters in valid UTF-8 (what base::Value holds).
size_t CountChars(std::string_view s) {
  size_t n = 0;
  for (char c : s) {
    if (!IsContinuation(c))
      ++n;
  }
  return n;
}

// |s| cut to at most |max_chars| characters, at a character boundary.
std::string_view TruncateChars(std::string_view s, size_t max_chars) {
  size_t chars = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!IsContinuation(s[i])) {
      if (chars == max_chars)
        return s.substr(0, i);
      ++chars;
    }
  }
  return s;
}

bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

bool IsBlank(std::string_view s) {
  return std::all_of(s.begin(), s.end(), IsAsciiSpace);
}

// C0 controls and DEL; |allow_newlines| keeps \n and \t.
bool HasControlChars(std::string_view s, bool allow_newlines) {
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (allow_newlines && (c == '\n' || c == '\t'))
      continue;
    if (c < 0x20 || c == 0x7F)
      return true;
  }
  return false;
}

bool IsAsciiAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

// Page text on one line, for the prompt: control characters and line
// breaks become spaces (it can't start a line of its own), double quotes
// single ones, runs of spaces collapse, the ends are trimmed, and a text
// over |max_chars| characters is cut with an ellipsis.
std::string OneLine(std::string_view raw, size_t max_chars) {
  std::string out;
  bool space = false;
  for (char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    out += (ch == '"') ? '\'' : ch;
    if (out.size() > 4 * max_chars + 4)
      break;  // more than |max_chars| characters already
  }
  if (max_chars == 0)
    return std::string();
  if (CountChars(out) <= max_chars)
    return out;
  std::string cut(TruncateChars(out, max_chars - 1));
  while (!cut.empty() && cut.back() == ' ')
    cut.pop_back();
  return cut + kEllipsis;
}

// A model-written string as the user reads it in the step trace and the
// approval prompt: on one line (C0 and C1 controls and DEL become spaces),
// the other characters that reorder or hide text (hidden_chars.h) shown as
// U+FFFD, and '"' and '\' escaped, so the quotes around it can't be closed
// early.
std::string Visible(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x20 || c == 0x7F) {
      out += ' ';
      ++i;
    } else if (c == 0xC2 && i + 1 < s.size() &&
               static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
               static_cast<unsigned char>(s[i + 1]) <= 0x9F) {
      out += ' ';  // U+0080-U+009F
      i += 2;
    } else if (const size_t n = hidden_chars::HiddenCharAt(s, i)) {
      out += kReplacement;
      i += n;
    } else {
      if (c == '"' || c == '\\')
        out += '\\';
      out += s[i];
      ++i;
    }
  }
  return out;
}

// |s| as a JSON string literal, for page scripts: base::JSONWriter escapes
// quotes, backslashes, control characters, '<' and U+2028 / U+2029.
std::string JsString(std::string_view s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

// JSON string literal for schema text (property names and enum values;
// none has control characters, which ConvertInputSchema() refuses).
std::string Quote(std::string_view s) {
  return JsString(s);
}

// ---- Names ------------------------------------------------------------------

// Blink's rule for a tool name (model_context.cc, IsValidToolName).
bool IsToolName(std::string_view s) {
  if (s.empty() || s.size() > kMaxToolNameChars)
    return false;
  return std::all_of(s.begin(), s.end(), [](char c) {
    return IsAsciiAlnum(c) || c == '_' || c == '-' || c == '.';
  });
}

// A document's mark, as BuildToolListScript() is given it: 1-64 of
// [A-Za-z0-9].
bool IsDocumentMark(std::string_view s) {
  return !s.empty() && s.size() <= kMaxDocumentMarkChars &&
         std::all_of(s.begin(), s.end(), IsAsciiAlnum);
}

// An argument name the agent takes: [A-Za-z0-9_-]{1,64}, not starting
// with '-' or "__".
bool IsArgumentName(std::string_view s) {
  if (s.empty() || s.size() > kMaxArgumentNameBytes || s[0] == '-' ||
      s.starts_with("__")) {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](char c) {
    return IsAsciiAlnum(c) || c == '_' || c == '-';
  });
}

// ---- Input schemas ----------------------------------------------------------

// Keywords that describe rather than constrain; ignored.
bool IsAnnotation(std::string_view key) {
  return key == "title" || key == "description" || key == "default" ||
         key == "examples" || key == "$schema" || key == "$id" ||
         key == "$comment" || key == "deprecated" || key == "readOnly" ||
         key == "writeOnly" || key == "format";
}

// |d| has only |keys| and annotations (and, at the root, unused
// definitions: $ref is refused).
bool OnlyKeys(const base::DictValue& d,
              std::initializer_list<std::string_view> keys,
              bool root = false) {
  for (const auto [key, value] : d) {
    if (IsAnnotation(key))
      continue;
    if (root && (key == "$defs" || key == "definitions"))
      continue;
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
      return false;
  }
  return true;
}

// A whole number within +-kMaxToolBound, written as an int or as an
// integral double.
std::optional<int64_t> ExactInteger(const base::Value& v) {
  if (v.is_int())
    return v.GetInt();
  if (!v.is_double())
    return std::nullopt;
  const double d = v.GetDouble();
  if (!std::isfinite(d) || std::trunc(d) != d ||
      std::fabs(d) > static_cast<double>(kMaxToolBound)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(d);
}

// A count keyword (minLength, maxItems, ...): absent, or a whole number
// >= 0. nullopt: refused.
std::optional<std::optional<int64_t>> ReadCount(const base::DictValue& d,
                                                std::string_view key) {
  const base::Value* v = d.Find(key);
  if (!v)
    return std::optional<int64_t>();
  std::optional<int64_t> n = ExactInteger(*v);
  if (!n || *n < 0)
    return std::nullopt;
  return n;
}

// Inclusive whole-number bounds; exclusive bounds become inclusive ones
// (only integers get here with those: for numbers they would need a
// fraction, and OnlyKeys() refuses them). nullopt: refused.
struct Bounds {
  std::optional<int64_t> low;
  std::optional<int64_t> high;
};
std::optional<Bounds> ReadBounds(const base::DictValue& d) {
  struct Key {
    std::string_view name;
    bool is_low;
    int64_t adjust;
  };
  static constexpr Key kKeys[] = {{"minimum", true, 0},
                                  {"maximum", false, 0},
                                  {"exclusiveMinimum", true, 1},
                                  {"exclusiveMaximum", false, -1}};
  Bounds b;
  for (const Key& key : kKeys) {
    const base::Value* v = d.Find(key.name);
    if (!v)
      continue;
    std::optional<int64_t> n = ExactInteger(*v);
    if (!n)
      return std::nullopt;
    const int64_t bound = *n + key.adjust;
    std::optional<int64_t>& slot = key.is_low ? b.low : b.high;
    slot = !slot
               ? bound
               : (key.is_low ? std::max(*slot, bound) : std::min(*slot, bound));
  }
  if (b.low && b.high && *b.low > *b.high)
    return std::nullopt;
  return b;
}

// The bounds the grammar can express (json_schema_grammar.h).
bool ExpressibleBounds(const Bounds& b, ArgType type) {
  if (!b.low && !b.high)
    return true;
  if (b.low && !b.high) {
    return *b.low == 0 || (type == ArgType::kInteger && *b.low == 1);
  }
  if (!b.low || !b.high)
    return false;
  if (type == ArgType::kNumber && *b.low < 0)
    return false;
  return *b.high - *b.low + 1 <= kMaxToolRangeSize;
}

// One argument's schema (or, |in_list|, the "items" of a list argument).
std::optional<ToolArgument> ConvertArgument(std::string_view name,
                                            const base::Value& schema,
                                            bool in_list) {
  if (!schema.is_dict())
    return std::nullopt;  // true / false schemas, or not a schema
  const base::DictValue& s = schema.GetDict();

  std::optional<ArgType> type;
  bool is_list = false;
  if (const base::Value* t = s.Find("type")) {
    if (!t->is_string())
      return std::nullopt;  // type lists
    const std::string& n = t->GetString();
    if (n == "string")
      type = ArgType::kString;
    else if (n == "integer")
      type = ArgType::kInteger;
    else if (n == "number")
      type = ArgType::kNumber;
    else if (n == "boolean")
      type = ArgType::kBoolean;
    else if (n == "array" && !in_list)
      is_list = true;
    else
      return std::nullopt;
  } else if (s.Find("enum")) {
    type = ArgType::kString;
  } else {
    return std::nullopt;  // any value
  }

  std::string description;
  if (const std::string* d = s.FindString("description"))
    description = OneLine(*d, kMaxShownArgumentDescriptionChars);

  if (is_list) {
    if (!OnlyKeys(s,
                  {"type", "items", "minItems", "maxItems", "uniqueItems"})) {
      return std::nullopt;
    }
    const base::Value* items = s.Find("items");
    if (!items)
      return std::nullopt;
    std::optional<ToolArgument> arg = ConvertArgument(name, *items, true);
    if (!arg)
      return std::nullopt;
    if (const base::Value* unique = s.Find("uniqueItems")) {
      if (!unique->is_bool() || unique->GetBool())
        return std::nullopt;
    }
    std::optional<std::optional<int64_t>> min_items = ReadCount(s, "minItems");
    std::optional<std::optional<int64_t>> max_items = ReadCount(s, "maxItems");
    if (!min_items || !max_items)
      return std::nullopt;
    const int64_t cap = static_cast<int64_t>(kMaxToolListItems);
    const int64_t low = min_items->value_or(0);
    const int64_t high = std::min(max_items->value_or(cap), cap);
    if (low > high)
      return std::nullopt;  // or a minimum over the cap
    arg->list = true;
    arg->min_items = static_cast<size_t>(low);
    arg->max_items = static_cast<size_t>(high);
    arg->description = std::move(description);
    return arg;
  }

  ToolArgument arg;
  arg.name = std::string(name);
  arg.type = *type;
  arg.description = std::move(description);
  switch (*type) {
    case ArgType::kString: {
      if (!OnlyKeys(s, {"type", "enum", "minLength", "maxLength"}))
        return std::nullopt;
      const size_t cap = in_list ? kMaxToolItemChars : kMaxToolStringChars;
      if (const base::Value* e = s.Find("enum")) {
        // The grammar takes an enum on its own.
        if (s.Find("minLength") || s.Find("maxLength") || !e->is_list())
          return std::nullopt;
        const base::ListValue& values = e->GetList();
        if (values.empty() || values.size() > kMaxToolEnumValues)
          return std::nullopt;
        for (const base::Value& v : values) {
          if (!v.is_string())
            return std::nullopt;
          const std::string& text = v.GetString();
          const size_t chars = CountChars(text);
          if (chars == 0 || chars > kMaxToolEnumValueChars ||
              HasControlChars(text, /*allow_newlines=*/false) ||
              std::find(arg.enum_values.begin(), arg.enum_values.end(), text) !=
                  arg.enum_values.end()) {
            return std::nullopt;
          }
          arg.enum_values.push_back(text);
        }
        arg.max_length = kMaxToolEnumValueChars;
        return arg;
      }
      std::optional<std::optional<int64_t>> min_length =
          ReadCount(s, "minLength");
      std::optional<std::optional<int64_t>> max_length =
          ReadCount(s, "maxLength");
      if (!min_length || !max_length)
        return std::nullopt;
      const int64_t low = min_length->value_or(0);
      const int64_t high =
          std::min(max_length->value_or(static_cast<int64_t>(cap)),
                   static_cast<int64_t>(cap));
      if (low > high)
        return std::nullopt;  // or a minimum over the cap
      arg.min_length = static_cast<size_t>(low);
      arg.max_length = static_cast<size_t>(high);
      return arg;
    }
    case ArgType::kInteger:
    case ArgType::kNumber: {
      const bool integer = *type == ArgType::kInteger;
      if (integer ? !OnlyKeys(s, {"type", "minimum", "maximum",
                                  "exclusiveMinimum", "exclusiveMaximum"})
                  : !OnlyKeys(s, {"type", "minimum", "maximum"})) {
        return std::nullopt;
      }
      std::optional<Bounds> b = ReadBounds(s);
      if (!b || !ExpressibleBounds(*b, *type))
        return std::nullopt;
      arg.minimum = b->low;
      arg.maximum = b->high;
      return arg;
    }
    case ArgType::kBoolean:
      if (!OnlyKeys(s, {"type"}))
        return std::nullopt;
      return arg;
  }
  return std::nullopt;
}

// ---- Arguments --------------------------------------------------------------

// One value of |arg|'s type (a list's item when |arg|.list). Writes the
// value the tool gets and how it is shown.
ArgumentsError ReadScalar(const base::Value& v,
                          const ToolArgument& arg,
                          base::Value* out,
                          std::string* shown) {
  switch (arg.type) {
    case ArgType::kString: {
      if (!v.is_string())
        return ArgumentsError::kBadValue;
      const std::string& s = v.GetString();
      const size_t chars = CountChars(s);
      if (chars > arg.max_length)
        return ArgumentsError::kTooLong;
      if (chars < arg.min_length)
        return ArgumentsError::kBadValue;
      if (HasControlChars(s, /*allow_newlines=*/true))
        return ArgumentsError::kBadCharacters;
      if (!arg.enum_values.empty() &&
          std::find(arg.enum_values.begin(), arg.enum_values.end(), s) ==
              arg.enum_values.end()) {
        return ArgumentsError::kBadValue;
      }
      *out = base::Value(s);
      *shown = "\"" + Visible(s) + "\"";
      return ArgumentsError::kNone;
    }
    case ArgType::kInteger: {
      std::optional<int64_t> n = ExactInteger(v);
      if (!n || (arg.minimum && *n < *arg.minimum) ||
          (arg.maximum && *n > *arg.maximum)) {
        return ArgumentsError::kBadValue;
      }
      if (*n >= std::numeric_limits<int>::min() &&
          *n <= std::numeric_limits<int>::max()) {
        *out = base::Value(static_cast<int>(*n));
      } else {
        *out = base::Value(static_cast<double>(*n));
      }
      *shown = base::NumberToString(*n);
      return ArgumentsError::kNone;
    }
    case ArgType::kNumber: {
      if (!v.is_int() && !v.is_double())
        return ArgumentsError::kBadValue;
      const double d = v.is_int() ? v.GetInt() : v.GetDouble();
      if (!std::isfinite(d) ||
          (arg.minimum && d < static_cast<double>(*arg.minimum)) ||
          (arg.maximum && d > static_cast<double>(*arg.maximum))) {
        return ArgumentsError::kBadValue;
      }
      if (v.is_int()) {
        *out = base::Value(v.GetInt());
        *shown = base::NumberToString(v.GetInt());
      } else {
        *out = base::Value(d);
        *shown = base::NumberToString(d);
      }
      return ArgumentsError::kNone;
    }
    case ArgType::kBoolean:
      if (!v.is_bool())
        return ArgumentsError::kBadValue;
      *out = base::Value(v.GetBool());
      *shown = v.GetBool() ? "true" : "false";
      return ArgumentsError::kNone;
  }
  return ArgumentsError::kBadValue;
}

// ---- Schemas ----------------------------------------------------------------

std::string ScalarSchema(const ToolArgument& arg, bool nullable) {
  auto type = [&](std::string_view name) {
    std::string t = Quote(name);
    return nullable ? "[" + t + ",\"null\"]" : t;
  };
  switch (arg.type) {
    case ArgType::kString: {
      if (!arg.enum_values.empty()) {
        std::string s = "{\"enum\":[";
        for (size_t i = 0; i < arg.enum_values.size(); ++i) {
          if (i)
            s += ',';
          s += Quote(arg.enum_values[i]);
        }
        return s + (nullable ? ",null]}" : "]}");
      }
      std::string s = "{\"type\":" + type("string");
      if (arg.min_length > 0)
        s += ",\"minLength\":" + base::NumberToString(arg.min_length);
      return s + ",\"maxLength\":" + base::NumberToString(arg.max_length) + "}";
    }
    case ArgType::kInteger:
    case ArgType::kNumber: {
      std::string s =
          "{\"type\":" +
          type(arg.type == ArgType::kInteger ? "integer" : "number");
      if (arg.minimum)
        s += ",\"minimum\":" + base::NumberToString(*arg.minimum);
      if (arg.maximum)
        s += ",\"maximum\":" + base::NumberToString(*arg.maximum);
      return s + "}";
    }
    case ArgType::kBoolean:
      return "{\"type\":" + type("boolean") + "}";
  }
  return "{}";
}

std::string ArgumentSchema(const ToolArgument& arg) {
  const bool nullable = !arg.required;
  if (!arg.list)
    return ScalarSchema(arg, nullable);
  std::string s = "{\"type\":" +
                  std::string(nullable ? "[\"array\",\"null\"]" : "\"array\"") +
                  ",\"items\":" + ScalarSchema(arg, /*nullable=*/false);
  if (arg.min_items > 0)
    s += ",\"minItems\":" + base::NumberToString(arg.min_items);
  return s + ",\"maxItems\":" + base::NumberToString(arg.max_items) + "}";
}

// How the prompt names an argument's type.
std::string TypeText(const ToolArgument& arg) {
  std::string t;
  switch (arg.type) {
    case ArgType::kString:
      if (!arg.enum_values.empty()) {
        t = "one of";
        size_t shown = 0;
        for (const std::string& v : arg.enum_values) {
          if (shown > 0 && t.size() > 160) {
            t += " ...";
            break;
          }
          t +=
              (shown ? ", '" : " '") + OneLine(v, kMaxToolEnumValueChars) + "'";
          ++shown;
        }
      } else {
        t = "text";
        if (arg.min_length > 0) {
          t += " of " + base::NumberToString(arg.min_length) + "-" +
               base::NumberToString(arg.max_length) + " characters";
        } else {
          t += " up to " + base::NumberToString(arg.max_length) + " characters";
        }
      }
      break;
    case ArgType::kInteger:
    case ArgType::kNumber:
      t = arg.type == ArgType::kInteger ? "whole number" : "number";
      if (arg.minimum && arg.maximum) {
        t += " " + base::NumberToString(*arg.minimum) + " to " +
             base::NumberToString(*arg.maximum);
      } else if (arg.minimum) {
        t += " >= " + base::NumberToString(*arg.minimum);
      }
      break;
    case ArgType::kBoolean:
      t = "true or false";
      break;
  }
  if (arg.list) {
    t = "list of " + base::NumberToString(arg.min_items) + "-" +
        base::NumberToString(arg.max_items) + " (" + t + ")";
  }
  return t;
}

// ---- Results ----------------------------------------------------------------

// The page's answer for the model: \r dropped, other control characters
// but \n and \t made spaces, ends trimmed, at most kMaxToolResultChars.
std::string CleanResult(std::string_view raw, bool* cut) {
  std::string out;
  size_t chars = 0;
  *cut = false;
  for (char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\r')
      continue;
    const char clean =
        ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) ? ' ' : ch;
    if (out.empty() && IsAsciiSpace(clean))
      continue;  // leading white space
    if (!IsContinuation(clean)) {
      if (chars == kMaxToolResultChars) {
        *cut = true;
        break;
      }
      ++chars;
    }
    out += clean;
  }
  while (!out.empty() && IsAsciiSpace(out.back()))
    out.pop_back();
  return out;
}

std::optional<ListedTool> ParseListedTool(const base::Value& item) {
  if (!item.is_dict())
    return std::nullopt;
  const base::DictValue& d = item.GetDict();
  const std::string* name = d.FindString("name");
  const std::string* description = d.FindString("description");
  if (!name || name->size() > BytesFor(kMaxToolNameChars) || !description ||
      description->size() > BytesFor(kMaxToolDescriptionChars)) {
    return std::nullopt;
  }
  ListedTool tool;
  tool.name = *name;
  tool.description = *description;
  if (const base::Value* schema = d.Find("schema");
      schema && !schema->is_none()) {
    if (!schema->is_string() ||
        schema->GetString().size() > kMaxToolSchemaBytes + 1) {
      return std::nullopt;
    }
    tool.input_schema = schema->GetString();
  }
  auto read_bool = [&](std::string_view key, bool* out) {
    const base::Value* v = d.Find(key);
    if (!v)
      return true;
    if (!v->is_bool())
      return false;
    *out = v->GetBool();
    return true;
  };
  if (!read_bool("readOnly", &tool.read_only) ||
      !read_bool("untrusted", &tool.untrusted_output) ||
      !read_bool("consequential", &tool.consequential)) {
    return std::nullopt;
  }
  if (const base::Value* form = d.Find("form"); form && !form->is_none()) {
    if (!form->is_list())
      return std::nullopt;
    tool.form_backed = true;
    const base::ListValue& fields = form->GetList();
    if (fields.size() > kMaxFormControls) {
      tool.form_overflow = true;
      return tool;
    }
    for (const base::Value& field : fields) {
      if (!field.is_list() || field.GetList().size() != 2 ||
          !field.GetList()[0].is_string() || !field.GetList()[1].is_string()) {
        return std::nullopt;
      }
      const std::string& type = field.GetList()[0].GetString();
      const std::string& ac = field.GetList()[1].GetString();
      if (type.size() > BytesFor(24) || ac.size() > BytesFor(64))
        return std::nullopt;
      tool.form_fields.emplace_back(type, ac);
    }
  }
  return tool;
}

CallState CallStateFromName(std::string_view name) {
  if (name == "pending")
    return CallState::kPending;
  if (name == "done")
    return CallState::kDone;
  if (name == "not_found")
    return CallState::kNotFound;
  if (name == "changed")
    return CallState::kChanged;
  if (name == "origin_changed")
    return CallState::kOriginChanged;
  if (name == "gone")
    return CallState::kGone;
  return CallState::kFailed;
}

// The parts both page scripts share: the <form toolname> of a tool and
// its controls' (type, autocomplete), at most kMaxFormControls + 1 (one
// more shows the form is over the cap). A form's named controls take
// precedence over its own members (HTMLFormElement is
// [LegacyOverrideBuiltIns]), so the controls and attributes are read
// through the prototypes' accessors.
constexpr char kFormHelpers[] = R"JS(
    const controlsOf = Object.getOwnPropertyDescriptor(
        HTMLFormElement.prototype, 'elements').get;
    const attrOf = Element.prototype.getAttribute;
    const formOf = (name) => {
      for (const f of document.querySelectorAll('form[toolname]')) {
        if (attrOf.call(f, 'toolname') === name) return f;
      }
      return null;
    };
    const fieldsOf = (f) => {
      const out = [];
      for (const el of controlsOf.call(f)) {
        if (out.length > 64) break;
        out.push([String(el.type || '').toLowerCase().slice(0, 24),
                  String(attrOf.call(el, 'autocomplete') || '').slice(0, 64)]);
      }
      return out;
    };
    const schemaOf = (t) => {
      const s = t.inputSchema;
      if (s === undefined || s === null) return null;
      if (typeof s !== 'object') return '!';
      try {
        const text = JSON.stringify(s);
        return typeof text === 'string' ? text : '!';
      } catch (e) {
        return '!';
      }
    };)JS";

}  // namespace

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

ToolArgument::ToolArgument() = default;
ToolArgument::~ToolArgument() = default;
ToolArgument::ToolArgument(const ToolArgument&) = default;
ToolArgument& ToolArgument::operator=(const ToolArgument&) = default;
ToolArgument::ToolArgument(ToolArgument&&) = default;
ToolArgument& ToolArgument::operator=(ToolArgument&&) = default;

SiteTool::SiteTool() = default;
SiteTool::~SiteTool() = default;
SiteTool::SiteTool(const SiteTool&) = default;
SiteTool& SiteTool::operator=(const SiteTool&) = default;
SiteTool::SiteTool(SiteTool&&) = default;
SiteTool& SiteTool::operator=(SiteTool&&) = default;

PageFrame::PageFrame() = default;
PageFrame::~PageFrame() = default;
PageFrame::PageFrame(const PageFrame&) = default;
PageFrame& PageFrame::operator=(const PageFrame&) = default;
PageFrame::PageFrame(PageFrame&&) = default;
PageFrame& PageFrame::operator=(PageFrame&&) = default;

ListedTool::ListedTool() = default;
ListedTool::~ListedTool() = default;
ListedTool::ListedTool(const ListedTool&) = default;
ListedTool& ListedTool::operator=(const ListedTool&) = default;
ListedTool::ListedTool(ListedTool&&) = default;
ListedTool& ListedTool::operator=(ListedTool&&) = default;

ToolList::ToolList() = default;
ToolList::~ToolList() = default;
ToolList::ToolList(ToolList&&) = default;
ToolList& ToolList::operator=(ToolList&&) = default;

CallOutcome::CallOutcome() = default;
CallOutcome::~CallOutcome() = default;
CallOutcome::CallOutcome(const CallOutcome&) = default;
CallOutcome& CallOutcome::operator=(const CallOutcome&) = default;
CallOutcome::CallOutcome(CallOutcome&&) = default;
CallOutcome& CallOutcome::operator=(CallOutcome&&) = default;

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

bool IsSameOriginFrame(const PageFrame& frame, std::string_view main_origin) {
  // Only an http(s) page's own origin: an opaque one ("null") is never the
  // same as another, however it serializes.
  return frame.in_page &&
         (main_origin.starts_with("https://") ||
          main_origin.starts_with("http://")) &&
         frame.origin == main_origin;
}

std::vector<size_t> ToolFrames(const std::vector<PageFrame>& frames) {
  std::vector<size_t> out;
  if (frames.empty() || frames[0].parent ||
      !IsSameOriginFrame(frames[0], frames[0].origin)) {
    return out;
  }
  const std::string& main_origin = frames[0].origin;
  // The children of each frame, in their order in |frames|. A parent
  // always comes before its children; anything else is not a frame tree.
  std::vector<std::vector<size_t>> children(frames.size());
  for (size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].parent && *frames[i].parent < i)
      children[*frames[i].parent].push_back(i);
  }
  // Depth first from the main document, into frames that count only: a
  // frame that doesn't (another origin, a fenced frame) hides its subtree.
  std::vector<size_t> pending = {0};
  while (!pending.empty() && out.size() < kMaxToolFrames + 1) {
    const size_t i = pending.back();
    pending.pop_back();
    out.push_back(i);
    for (auto it = children[i].rbegin(); it != children[i].rend(); ++it) {
      if (IsSameOriginFrame(frames[*it], main_origin))
        pending.push_back(*it);
    }
  }
  return out;
}

bool IsToolFrameChain(const std::vector<PageFrame>& chain) {
  if (chain.empty() || chain.size() > kMaxToolFrames + 1)
    return false;
  for (size_t i = 0; i < chain.size(); ++i) {
    const bool linked = i == 0 ? !chain[i].parent
                               : chain[i].parent && *chain[i].parent == i - 1;
    if (!linked || !IsSameOriginFrame(chain[i], chain[0].origin))
      return false;
  }
  return true;
}

bool IsCrossOriginToolFrame(const PageFrame& frame,
                            std::string_view main_origin) {
  const auto is_web = [](std::string_view o) {
    return o.starts_with("https://") || o.starts_with("http://");
  };
  // url::Origin::Serialize() of an http(s) origin has a host; an opaque
  // one is "null". Any difference from the main origin counts as another
  // site's frame (a subdomain, a port, http vs. https).
  return frame.in_page && frame.tools_policy && is_web(main_origin) &&
         is_web(frame.origin) && frame.origin.size() > 8 &&
         frame.origin != main_origin;
}

std::vector<size_t> CrossOriginToolFrames(
    const std::vector<PageFrame>& frames) {
  std::vector<size_t> out;
  const std::vector<size_t> own = ToolFrames(frames);
  if (own.empty())
    return out;
  const std::string& main_origin = frames[0].origin;
  // The direct children of the documents ToolFrames() lists, in its order
  // and then their own: the frames the main site itself embeds.
  std::vector<bool> is_own(frames.size(), false);
  for (size_t i : own)
    is_own[i] = true;
  for (size_t parent : own) {
    for (size_t i = 1; i < frames.size(); ++i) {
      if (out.size() >= kMaxCrossOriginToolFrames)
        return out;
      if (frames[i].parent && *frames[i].parent == parent &&
          *frames[i].parent < i && !is_own[i] &&
          IsCrossOriginToolFrame(frames[i], main_origin)) {
        out.push_back(i);
      }
    }
  }
  return out;
}

bool IsCrossOriginToolFrameChain(const std::vector<PageFrame>& chain) {
  if (chain.size() < 2)
    return false;
  const std::vector<PageFrame> own(chain.begin(), chain.end() - 1);
  const PageFrame& last = chain.back();
  return IsToolFrameChain(own) && last.parent &&
         *last.parent == chain.size() - 2 &&
         IsCrossOriginToolFrame(last, chain[0].origin);
}

bool MayOfferCrossOriginTool(const SiteTool& tool) {
  return !agent_safety::IsSensitive(tool.sensitive);
}

std::string FrameText(std::string_view origin, std::string_view path) {
  // An http(s) URL's path is never empty: no path is a document that took
  // its origin from its parent (about:blank, about:srcdoc, blob:).
  if (path.empty())
    return Visible(origin) + " (a frame without an address of its own)";
  std::string shown;
  if (CountChars(path) > kMaxShownFramePathChars) {
    shown = Visible(TruncateChars(path, kMaxShownFramePathChars - 1)) +
            kEllipsis;
  } else {
    shown = Visible(path);
  }
  return Visible(origin) + shown;
}

std::string VisibleText(std::string_view s) {
  return Visible(s);
}

bool IsValidToolName(std::string_view name) {
  return IsToolName(name);
}

bool IsValidArgumentName(std::string_view name) {
  return IsArgumentName(name);
}

std::string ToolLabel(const SiteTool& tool) {
  if (tool.frame == 0)
    return tool.name;
  std::string label =
      tool.name + " in frame " + base::NumberToString(tool.frame);
  // The browser's origin of another site's frame, never page text.
  if (tool.cross_origin)
    label += " from " + Visible(tool.frame_origin);
  return label;
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

bool ShouldListTools(const ListFacts& facts) {
  return facts.switched_on && facts.origin_allowed && facts.snapshot_is_current;
}

std::string BuildToolListScript(std::string_view request_id,
                                std::string_view document_mark) {
  // Runs in the agent's isolated world: the page shapes what getTools()
  // returns, not this code or the objects it calls. Only the document's
  // own tools (same window and origin) are listed, at most 32, each field
  // cut one character over its C++ cap so an oversize one is refused
  // there, not shortened. The isolated world's global object belongs to
  // this document: the mark set on it stays with the document (a new
  // document gets a new global), and the page can't read or set it.
  static_assert(kMaxListedTools == 32, "update the script");
  static_assert(kMaxToolNameChars == 128, "update the script");
  static_assert(kMaxToolDescriptionChars == 4096, "update the script");
  static_assert(kMaxToolSchemaBytes == 8192, "update the script");
  static_assert(kMaxFormControls == 64, "update kFormHelpers");
  return base::StringPrintf(
      R"JS((() => {
  try {
    const id = %s, mark = %s;
    const mc = window.isSecureContext ? document.modelContext : undefined;
    if (!mc || typeof mc.getTools !== 'function') return {state: 'unavailable'};
    %s
    if (typeof globalThis.__moltToolDocument !== 'string') {
      globalThis.__moltToolDocument = mark;
    }
    const st = {id: id, state: 'pending',
                document: globalThis.__moltToolDocument};
    globalThis.__moltAgentToolList = st;
    mc.getTools().then((tools) => {
      const out = [];
      for (const t of tools) {
        if (out.length >= 32) break;
        if (!t || t.window !== window || t.origin !== location.origin) continue;
        const a = t.annotations || {};
        const f = formOf(String(t.name));
        out.push({
          name: String(t.name).slice(0, 129),
          description: String(t.description).slice(0, 4097),
          schema: (() => {
            const s = schemaOf(t);
            return s === null ? null : s.slice(0, 8193);
          })(),
          readOnly: a.readOnlyHint === true,
          untrusted: a.untrustedContentHint === true,
          consequential: a.consequentialHint === true,
          form: f ? fieldsOf(f) : null,
        });
      }
      st.origin = String(location.origin);
      st.tools = out;
      st.state = 'done';
    }, () => { st.state = 'failed'; });
    return {state: 'pending'};
  } catch (e) {
    return {state: 'failed'};
  }
})())JS",
      JsString(request_id).c_str(), JsString(document_mark).c_str(),
      kFormHelpers);
}

std::string BuildToolListPollScript(std::string_view request_id) {
  return base::StringPrintf(
      R"JS((() => {
  try {
    const st = globalThis.__moltAgentToolList;
    if (!st || st.id !== %s) return {state: 'gone'};
    if (st.state !== 'done') return {state: st.state};
    return {state: 'done', origin: st.origin, document: st.document,
            tools: st.tools};
  } catch (e) {
    return {state: 'failed'};
  }
})())JS",
      JsString(request_id).c_str());
}

std::string BuildToolCountPollScript(std::string_view request_id) {
  return base::StringPrintf(
      R"JS((() => {
  try {
    const st = globalThis.__moltAgentToolList;
    if (!st || st.id !== %s) return {state: 'gone'};
    if (st.state !== 'done') return {state: st.state};
    return {state: 'done', origin: st.origin, document: st.document,
            count: Array.isArray(st.tools) ? st.tools.length : 0};
  } catch (e) {
    return {state: 'failed'};
  }
})())JS",
      JsString(request_id).c_str());
}

ToolCount ParseToolCount(const base::Value& result) {
  ToolCount count;
  if (!result.is_dict())
    return count;
  const base::DictValue& d = result.GetDict();
  const std::string* state = d.FindString("state");
  if (!state)
    return count;
  if (*state == "pending") {
    count.state = ListState::kPending;
    return count;
  }
  if (*state == "unavailable") {
    count.state = ListState::kUnavailable;
    return count;
  }
  if (*state == "gone") {
    count.state = ListState::kGone;
    return count;
  }
  if (*state != "done")
    return count;
  const std::string* origin = d.FindString("origin");
  const std::string* document = d.FindString("document");
  const std::optional<int> n = d.FindInt("count");
  if (!origin || origin->empty() || origin->size() > 512 ||
      HasControlChars(*origin, /*allow_newlines=*/false) || !document ||
      !IsDocumentMark(*document) || !n || *n < 0) {
    return count;
  }
  count.state = ListState::kDone;
  count.origin = *origin;
  count.count = std::min(static_cast<size_t>(*n), kMaxListedTools);
  return count;
}

ToolList ParseToolList(const base::Value& result) {
  ToolList list;
  list.state = ListState::kFailed;
  if (!result.is_dict())
    return list;
  const base::DictValue& d = result.GetDict();
  const std::string* state = d.FindString("state");
  if (!state)
    return list;
  if (*state == "pending") {
    list.state = ListState::kPending;
    return list;
  }
  if (*state == "unavailable") {
    list.state = ListState::kUnavailable;
    return list;
  }
  if (*state == "gone") {
    list.state = ListState::kGone;
    return list;
  }
  if (*state != "done")
    return list;
  const std::string* origin = d.FindString("origin");
  const std::string* document = d.FindString("document");
  const base::ListValue* tools = d.FindList("tools");
  if (!origin || origin->empty() || origin->size() > 512 ||
      HasControlChars(*origin, /*allow_newlines=*/false) || !document ||
      !IsDocumentMark(*document) || !tools) {
    return list;
  }
  list.origin = *origin;
  list.document = *document;
  const size_t n = std::min(tools->size(), kMaxListedTools);
  for (size_t i = 0; i < n; ++i) {
    std::optional<ListedTool> tool = ParseListedTool((*tools)[i]);
    if (tool)
      list.tools.push_back(std::move(*tool));
  }
  list.state = ListState::kDone;
  return list;
}

std::optional<SiteTool> MakeSiteTool(const ListedTool& listed) {
  if (!IsToolName(listed.name))
    return std::nullopt;
  if (CountChars(listed.description) > kMaxToolDescriptionChars ||
      IsBlank(listed.description)) {
    return std::nullopt;
  }
  SiteTool tool;
  tool.name = listed.name;
  tool.description = OneLine(listed.description, kMaxShownDescriptionChars);
  if (listed.input_schema) {
    if (listed.input_schema->size() > kMaxToolSchemaBytes)
      return std::nullopt;
    std::optional<std::vector<ToolArgument>> arguments =
        ConvertInputSchema(*listed.input_schema);
    if (!arguments)
      return std::nullopt;
    tool.arguments = std::move(*arguments);
  }

  // Never a credential: a tool named like a password field, with an
  // argument named like one, or whose form has a password or file field,
  // isn't offered. Payment cards and one-time codes make every call ask.
  FieldKind sensitive = FieldKind::kOrdinary;
  auto note = [&](FieldKind kind) {
    if (kind == FieldKind::kPassword)
      return false;
    if (agent_safety::IsSensitive(kind) && sensitive == FieldKind::kOrdinary)
      sensitive = kind;
    return true;
  };
  if (!note(agent_safety::ClassifySelector(tool.name)))
    return std::nullopt;
  for (const ToolArgument& arg : tool.arguments) {
    if (!note(agent_safety::ClassifySelector(arg.name)))
      return std::nullopt;
  }
  if (listed.form_backed) {
    if (listed.form_overflow)
      return std::nullopt;
    for (const auto& [type, autocomplete] : listed.form_fields) {
      if (type == "file" ||
          !note(agent_safety::ClassifyField(type, autocomplete))) {
        return std::nullopt;
      }
    }
  }
  tool.sensitive = sensitive;
  tool.read_only = listed.read_only;
  tool.consequential = listed.consequential;
  tool.untrusted_output = listed.untrusted_output;
  tool.form_backed = listed.form_backed;
  tool.listed_description = listed.description;
  tool.listed_input_schema = listed.input_schema;
  tool.listed_form_fields = listed.form_fields;
  return tool;
}

std::vector<SiteTool> MakeSiteTools(const ToolList& list,
                                    size_t frame,
                                    std::string_view frame_text) {
  std::vector<SiteTool> tools;
  if (list.state != ListState::kDone)
    return tools;
  for (const ListedTool& listed : list.tools) {
    std::optional<SiteTool> tool = MakeSiteTool(listed);
    if (!tool)
      continue;
    tool->frame = frame;
    tool->frame_text = frame ? std::string(frame_text) : std::string();
    tool->document = list.document;
    tools.push_back(std::move(*tool));
  }
  return tools;
}

std::optional<std::vector<ToolArgument>> ConvertInputSchema(
    std::string_view json) {
  std::optional<base::DictValue> root = base::JSONReader::ReadDict(
      json, base::JSON_PARSE_RFC, kMaxSchemaJsonDepth);
  if (!root)
    return std::nullopt;
  if (!OnlyKeys(*root,
                {"type", "properties", "required", "additionalProperties"},
                /*root=*/true)) {
    return std::nullopt;
  }
  if (const base::Value* type = root->Find("type")) {
    if (!type->is_string() || type->GetString() != "object")
      return std::nullopt;
  }
  if (const base::Value* additional = root->Find("additionalProperties")) {
    // Only the listed arguments are ever sent, either way.
    if (!additional->is_bool())
      return std::nullopt;
  }
  std::vector<ToolArgument> arguments;
  const base::Value* properties = root->Find("properties");
  if (properties) {
    if (!properties->is_dict() ||
        properties->GetDict().size() > kMaxToolArguments) {
      return std::nullopt;
    }
    for (const auto [name, schema] : properties->GetDict()) {
      if (!IsArgumentName(name))
        return std::nullopt;
      std::optional<ToolArgument> arg =
          ConvertArgument(name, schema, /*in_list=*/false);
      if (!arg)
        return std::nullopt;
      arguments.push_back(std::move(*arg));
    }
  }
  if (const base::Value* required = root->Find("required")) {
    if (!required->is_list())
      return std::nullopt;
    for (const base::Value& name : required->GetList()) {
      if (!name.is_string())
        return std::nullopt;
      auto it = std::find_if(
          arguments.begin(), arguments.end(),
          [&](const ToolArgument& a) { return a.name == name.GetString(); });
      if (it == arguments.end() || it->required)
        return std::nullopt;  // not a property, or named twice
      it->required = true;
    }
  }
  return arguments;
}

std::string BuildArgumentsSchema(const SiteTool& tool) {
  if (tool.arguments.empty())
    return "{\"type\":\"object\",\"additionalProperties\":false}";
  std::string properties;
  std::string required;
  for (size_t i = 0; i < tool.arguments.size(); ++i) {
    if (i) {
      properties += ',';
      required += ',';
    }
    const ToolArgument& arg = tool.arguments[i];
    properties += Quote(arg.name) + ":" + ArgumentSchema(arg);
    required += Quote(arg.name);
  }
  return "{\"type\":\"object\",\"properties\":{" + properties +
         "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

std::string DescribeTool(const SiteTool& tool) {
  std::string s = tool.ref + " " + ToolLabel(tool);
  if (tool.read_only && !tool.consequential && !tool.form_backed &&
      !tool.cross_origin && tool.sensitive == FieldKind::kOrdinary) {
    s += " [reads only]";
  } else {
    s += " [asks the user first]";
  }
  s += ": " + tool.description + "\n   arguments: ";
  if (tool.arguments.empty()) {
    s += "none";
  } else {
    for (size_t i = 0; i < tool.arguments.size(); ++i) {
      const ToolArgument& arg = tool.arguments[i];
      if (i)
        s += "; ";
      s += arg.name + " (" + TypeText(arg) +
           (arg.required ? ", required" : ", or null") + ")";
      if (!arg.description.empty())
        s += ": " + arg.description;
    }
  }
  if (CountChars(s) > kMaxToolTextChars) {
    s = std::string(TruncateChars(s, kMaxToolTextChars - 1)) + kEllipsis;
  }
  return s + "\n";
}

std::string DescribeTools(const std::vector<SiteTool>& tools) {
  std::string out;
  for (const SiteTool& tool : tools)
    out += DescribeTool(tool);
  return out;
}

// ---------------------------------------------------------------------------
// Calling
// ---------------------------------------------------------------------------

ArgumentsError ReadArguments(const base::Value& arguments,
                             const SiteTool& tool,
                             ToolArguments* out) {
  if (!arguments.is_dict())
    return ArgumentsError::kWrongKeys;
  const base::DictValue& d = arguments.GetDict();
  if (d.size() != tool.arguments.size())
    return ArgumentsError::kWrongKeys;
  base::DictValue call;
  std::string text;
  for (const ToolArgument& arg : tool.arguments) {
    const base::Value* v = d.Find(arg.name);
    if (!v)
      return ArgumentsError::kWrongKeys;
    if (v->is_none()) {
      if (arg.required)
        return ArgumentsError::kBadValue;
      continue;  // left out
    }
    base::Value value;
    std::string shown;
    if (arg.list) {
      if (!v->is_list())
        return ArgumentsError::kBadValue;
      const base::ListValue& items = v->GetList();
      if (items.size() > arg.max_items)
        return ArgumentsError::kTooLong;
      if (items.size() < arg.min_items)
        return ArgumentsError::kBadValue;
      base::ListValue list;
      shown = "[";
      for (const base::Value& item : items) {
        base::Value one;
        std::string one_shown;
        const ArgumentsError e = ReadScalar(item, arg, &one, &one_shown);
        if (e != ArgumentsError::kNone)
          return e;
        if (shown.size() > 1)
          shown += ", ";
        shown += one_shown;
        list.Append(std::move(one));
      }
      shown += "]";
      value = base::Value(std::move(list));
    } else {
      const ArgumentsError e = ReadScalar(*v, arg, &value, &shown);
      if (e != ArgumentsError::kNone)
        return e;
    }
    if (!text.empty())
      text += "; ";
    text += arg.name + " = " + shown;
    call.Set(arg.name, std::move(value));
  }
  std::string json;
  if (!base::JSONWriter::Write(call, &json))
    return ArgumentsError::kBadValue;
  out->json = std::move(json);
  out->text = std::move(text);
  return ArgumentsError::kNone;
}

std::vector<std::string> ToolCallEffects(const SiteTool& tool) {
  std::vector<std::string> why;
  // Another site's frame: its hints are that site's word to a run the user
  // started on the main site, and what a call is given (written from the
  // main site's pages) goes to the other site. Every call asks.
  if (tool.cross_origin)
    why.push_back("it is another site's tool");
  if (tool.form_backed)
    why.push_back("it fills a form there and may submit it");
  else if (!tool.read_only || tool.consequential)
    why.push_back("it may change something there");
  if (agent_safety::IsSensitive(tool.sensitive)) {
    why.push_back("it takes a " +
                  std::string(agent_safety::FieldKindLabel(tool.sensitive)));
  }
  return why;
}

std::string ToolPlaceText(const SiteTool& tool, std::string_view origin_text) {
  // A frame's tool names its frame (the browser's FrameText()), so the
  // user can tell it from the main document's.
  if (tool.frame == 0)
    return "on " + std::string(origin_text);
  if (tool.cross_origin) {
    return "in the frame " + tool.frame_text + " (another site) on " +
           std::string(origin_text);
  }
  return "in the frame " + tool.frame_text + " on " + std::string(origin_text);
}

std::string ToolCallTitle(const SiteTool& tool, std::string_view origin_text) {
  // Both sites for another site's frame: whose tool it is (the browser's
  // origin of the frame) and the page it is on.
  if (tool.cross_origin) {
    return Visible(tool.frame_origin) + "'s tool \"" + tool.name +
           "\" in its frame " + tool.frame_text + " on " +
           std::string(origin_text);
  }
  return "the site's tool \"" + tool.name + "\" " +
         ToolPlaceText(tool, origin_text);
}

CallCheck CheckToolCall(const SiteTool& tool,
                        std::string_view arguments_text,
                        std::string_view origin_text) {
  const std::vector<std::string> why = ToolCallEffects(tool);
  CallCheck check;
  check.ask = !why.empty();
  std::string message = "Run " + ToolCallTitle(tool, origin_text);
  for (size_t i = 0; i < why.size(); ++i)
    message += (i ? " and " : " \xE2\x80\x94 ") + why[i];
  message += arguments_text.empty() ? std::string(", with no arguments")
                                    : ", with " + std::string(arguments_text);
  check.message = std::move(message);
  return check;
}

std::string BuildToolCallScript(std::string_view request_id,
                                std::string_view origin,
                                const SiteTool& tool,
                                std::string_view arguments_json) {
  // The tool as it was listed: the call runs only in the document that
  // listed it (its mark: a document the frame navigated to, or another
  // document reusing the frame's host, has none or another), and only if
  // getTools() still shows exactly that tool: every field the agent read
  // when it listed it (a page can't swap a read-only tool for another
  // under the same name between the listing and the call, nor drop the
  // untrusted-content hint its answer is shown with).
  std::string form = "null";
  if (tool.form_backed) {
    base::ListValue fields;
    for (const auto& [type, autocomplete] : tool.listed_form_fields) {
      base::ListValue pair;
      pair.Append(type);
      pair.Append(autocomplete);
      fields.Append(std::move(pair));
    }
    base::JSONWriter::Write(fields, &form);
  }
  const std::string schema = tool.listed_input_schema
                                 ? JsString(*tool.listed_input_schema)
                                 : std::string("null");
  return base::StringPrintf(
      R"JS((() => {
  try {
    const id = %s, doc = %s, origin = %s, name = %s, desc = %s,
          schema = %s, readOnly = %s, untrusted = %s,
          consequential = %s, form = %s, args = %s;
    if (globalThis.__moltToolDocument !== doc) return 'document_changed';
    if (location.origin !== origin) return 'origin_changed';
    const mc = window.isSecureContext ? document.modelContext : undefined;
    if (!mc || typeof mc.getTools !== 'function' ||
        typeof mc.executeTool !== 'function') return 'unavailable';
    %s
    const sameFields = (a, b) => a === null ? b === null :
        (b !== null && a.length === b.length &&
         a.every((p, i) => p[0] === b[i][0] && p[1] === b[i][1]));
    const st = {id: id, state: 'pending', result: null};
    globalThis.__moltAgentToolCall = st;
    mc.getTools().then((tools) => {
      const t = tools.find((x) => x && x.window === window &&
                                  x.origin === origin && x.name === name);
      if (!t) { st.state = 'not_found'; return; }
      const a = t.annotations || {};
      const f = formOf(name);
      if (t.description !== desc || schemaOf(t) !== schema ||
          (a.readOnlyHint === true) !== readOnly ||
          (a.untrustedContentHint === true) !== untrusted ||
          (a.consequentialHint === true) !== consequential ||
          !sameFields(form, f ? fieldsOf(f) : null)) {
        st.state = 'changed';
        return;
      }
      if (location.origin !== origin) { st.state = 'origin_changed'; return; }
      // The browser's JSON, as the object executeTool() takes in 156.
      const input = JSON.parse(args);
      return mc.executeTool(t, input).then((r) => {
        st.result = (r === null || r === undefined) ? null :
            String(r).slice(0, 8192);
        st.state = 'done';
      }, () => { st.state = 'failed'; });
    }).catch(() => { st.state = 'failed'; });
    return 'started';
  } catch (e) {
    return 'error';
  }
})())JS",
      JsString(request_id).c_str(), JsString(tool.document).c_str(),
      JsString(origin).c_str(), JsString(tool.name).c_str(),
      JsString(tool.listed_description).c_str(),
      schema.c_str(), tool.read_only ? "true" : "false",
      tool.untrusted_output ? "true" : "false",
      tool.consequential ? "true" : "false", form.c_str(),
      JsString(arguments_json).c_str(), kFormHelpers);
}

std::string BuildToolCallPollScript(std::string_view request_id) {
  return base::StringPrintf(
      R"JS((() => {
  try {
    const st = globalThis.__moltAgentToolCall;
    if (!st || st.id !== %s) return {state: 'gone'};
    return {state: st.state, result: st.result};
  } catch (e) {
    return {state: 'failed'};
  }
})())JS",
      JsString(request_id).c_str());
}

CallOutcome ParseToolCallStart(const base::Value& result) {
  CallOutcome outcome;
  outcome.state = CallState::kFailed;
  if (!result.is_string())
    return outcome;
  const std::string& s = result.GetString();
  if (s == "started")
    outcome.state = CallState::kStarted;
  else if (s == "document_changed")
    outcome.state = CallState::kDocumentChanged;
  else if (s == "origin_changed")
    outcome.state = CallState::kOriginChanged;
  else if (s == "unavailable")
    outcome.state = CallState::kUnavailable;
  return outcome;
}

CallOutcome ParseToolCallPoll(const base::Value& result) {
  CallOutcome outcome;
  outcome.state = CallState::kFailed;
  if (!result.is_dict())
    return outcome;
  const base::DictValue& d = result.GetDict();
  const std::string* state = d.FindString("state");
  if (!state)
    return outcome;
  const CallState parsed = CallStateFromName(*state);
  if (parsed != CallState::kDone) {
    outcome.state = parsed;
    return outcome;
  }
  const base::Value* answer = d.Find("result");
  if (answer && answer->is_string()) {
    outcome.result = CleanResult(answer->GetString(), &outcome.result_cut);
  } else if (answer && !answer->is_none()) {
    return outcome;  // kFailed: not what the script writes
  }
  outcome.state = CallState::kDone;
  return outcome;
}

std::string_view CallFailureNote(CallState state) {
  static_assert(kCallTimeoutMs == 20000, "update the timeout note");
  switch (state) {
    case CallState::kFailed:
      return "The site's tool reported an error.";
    case CallState::kNotFound:
      return "The page no longer offers that tool.";
    case CallState::kChanged:
      return "Not run: the page changed that tool since it was listed.";
    case CallState::kOriginChanged:
      return "Not run: the page changed site.";
    case CallState::kUnavailable:
      return "Not run: the page no longer offers tools.";
    case CallState::kGone:
      return "The tool's page went away before it answered.";
    case CallState::kTimedOut:
      return "The site's tool didn't answer within 20 seconds.";
    case CallState::kDocumentChanged:
      return "Not run: the page or frame that offered the tool has changed "
             "or gone.";
    case CallState::kStarted:
    case CallState::kPending:
    case CallState::kDone:
      break;
  }
  return "";
}

std::string DescribeToolResult(const SiteTool& tool,
                               const CallOutcome& outcome) {
  std::string s = tool.cross_origin
                      ? "TOOL RESULT (another site's tool " + ToolLabel(tool) +
                            "; that site wrote it"
                      : "TOOL RESULT (the page's tool " + ToolLabel(tool);
  if (tool.untrusted_output)
    s += "; the site says it may contain other people's content";
  s += "):\n";
  if (!outcome.result || outcome.result->empty()) {
    s += "(no text)\n";
  } else {
    s += *outcome.result + "\n";
    if (outcome.result_cut) {
      s += "[cut at " + base::NumberToString(kMaxToolResultChars) +
           " characters]\n";
    }
  }
  return s;
}

}  // namespace agent_tools
}  // namespace molt_ai
