// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/bounded_json.h"

namespace molt_ai {

namespace {

// What a value can start with, for counting the alternatives llama.cpp's
// grammar keeps alive at once: after the first character only the
// alternatives of the same kind survive.
enum Kind : uint32_t {
  kKindObject = 1u << 0,
  kKindArray = 1u << 1,
  kKindString = 1u << 2,
  kKindNumber = 1u << 3,
  kKindTrue = 1u << 4,
  kKindFalse = 1u << 5,
  kKindNull = 1u << 6,
  kKindScalar = kKindString | kKindNumber | kKindTrue | kKindFalse | kKindNull,
  kKindAny = kKindObject | kKindArray | kKindScalar,
};
constexpr int kKindCount = 7;

uint32_t KindOfFirstChar(char c) {
  switch (c) {
    case '{':
      return kKindObject;
    case '[':
      return kKindArray;
    case '"':
      return kKindString;
    case 't':
      return kKindTrue;
    case 'f':
      return kKindFalse;
    case 'n':
      return kKindNull;
    default:
      return kKindNumber;  // '-' or a digit
  }
}

// JSON Schema "type" names.
enum TypeBit : uint32_t {
  kTypeObject = 1u << 0,
  kTypeArray = 1u << 1,
  kTypeString = 1u << 2,
  kTypeInteger = 1u << 3,
  kTypeNumber = 1u << 4,
  kTypeBoolean = 1u << 5,
  kTypeNull = 1u << 6,
};

constexpr std::array<std::pair<const char*, uint32_t>, 7> kTypeNames = {{
    {"object", kTypeObject},
    {"array", kTypeArray},
    {"string", kTypeString},
    {"integer", kTypeInteger},
    {"number", kTypeNumber},
    {"boolean", kTypeBoolean},
    {"null", kTypeNull},
}};

// Ignored wherever they appear.
constexpr const char* kAnnotations[] = {
    "title",    "description", "default",  "examples",
    "$comment", "$schema",     "$id",      "deprecated",
    "readOnly", "writeOnly",   "$defs",    "definitions",
};

constexpr const char* kHandled[] = {
    "type",           "properties", "required",         "additionalProperties",
    "items",          "minItems",   "maxItems",         "minLength",
    "maxLength",      "minimum",    "maximum",          "exclusiveMinimum",
    "exclusiveMaximum", "enum",     "const",            "anyOf",
    "oneOf",          "uniqueItems",
};

// Refused, and named in the reason (other unknown keywords are refused
// without naming them: the name is page text).
constexpr const char* kKnownUnsupported[] = {
    "$ref",          "$dynamicRef",       "$recursiveRef",
    "$anchor",       "$dynamicAnchor",    "$vocabulary",
    "pattern",       "format",            "allOf",
    "not",           "if",                "then",
    "else",          "patternProperties", "propertyNames",
    "prefixItems",   "additionalItems",   "contains",
    "minContains",   "maxContains",       "minProperties",
    "maxProperties", "dependentRequired", "dependentSchemas",
    "dependencies",  "unevaluatedProperties", "unevaluatedItems",
    "multipleOf",    "contentEncoding",   "contentMediaType",
    "contentSchema", "nullable",          "discriminator",
};

template <size_t N>
bool OneOf(const std::string& key, const char* const (&list)[N]) {
  for (const char* k : list) {
    if (key == k) return true;
  }
  return false;
}

bool IsAnnotation(const std::string& key) {
  return OneOf(key, kAnnotations);
}

// Grammar rules that don't depend on the schema. Emitted when used, with
// the rules they reference. |repeat|: the upper bounds of their counted
// repetitions (llama.cpp expands each into that many rules).
struct FixedRule {
  const char* name;
  const char* body;
  std::array<const char*, 4> deps;
  size_t repeat;
};

// Bounded whitespace: at most a space, or a newline and 20 spaces/tabs, so
// a model can't stall the answer with blank lines.
// char: one character of a JSON string: anything but '"', '\', DEL and
// controls, or an escape; \u escapes exclude surrogates (D800-DFFF).
// Numbers: at most 16 integer digits (exact in a double), 16 fraction
// digits and a 2-digit exponent (always finite).
constexpr FixedRule kFixedRules[] = {
    {"ws", R"(| " " | "\n" [ \t]{0,20})", {}, 20},
    {"hex", R"([0-9a-fA-F])", {}, 0},
    {"char",
     R"([^"\\\x7F\x00-\x1F] | "\\" (["\\/bfnrt] | "u" ([0-9a-cA-CefEF] hex hex hex | [dD] [0-7] hex hex)))",
     {"hex"},
     1},
    {"string", R"("\"" char* "\"")", {"char"}, 1},
    {"int-part", R"("0" | [1-9] [0-9]{0,15})", {}, 15},
    {"frac", R"("." [0-9]{1,16})", {}, 16},
    {"exp", R"([eE] [-+]? [0-9]{1,2})", {}, 3},
    {"integer", R"("-"? int-part)", {"int-part"}, 1},
    {"number", R"("-"? int-part frac? exp?)", {"int-part", "frac", "exp"}, 3},
    {"pos-integer", R"([1-9] [0-9]{0,15})", {}, 15},
    {"nonneg-number", R"(int-part frac? exp?)", {"int-part", "frac", "exp"}, 2},
    {"zero-frac", R"("." "0"{1,16})", {}, 16},
    {"boolean", R"("true" | "false")", {}, 0},
    {"null", R"("null")", {}, 0},
    // Any JSON value, nested at most kAnyValueDepth (3) deep.
    {"any0", R"(string | number | boolean | null)",
     {"string", "number", "boolean", "null"},
     0},
    {"any1-member", R"(string ws ":" ws any0 ws)", {"string", "ws", "any0"}, 0},
    {"any1-object", R"("{" ws (any1-member ("," ws any1-member)*)? "}")",
     {"ws", "any1-member"},
     2},
    {"any1-array", R"("[" ws (any0 ws ("," ws any0 ws)*)? "]")",
     {"ws", "any0"},
     2},
    {"any1", R"(any1-object | any1-array | any0)",
     {"any1-object", "any1-array", "any0"},
     0},
    {"any2-member", R"(string ws ":" ws any1 ws)", {"string", "ws", "any1"}, 0},
    {"any2-object", R"("{" ws (any2-member ("," ws any2-member)*)? "}")",
     {"ws", "any2-member"},
     2},
    {"any2-array", R"("[" ws (any1 ws ("," ws any1 ws)*)? "]")",
     {"ws", "any1"},
     2},
    {"any2", R"(any2-object | any2-array | any0)",
     {"any2-object", "any2-array", "any0"},
     0},
    {"any3-member", R"(string ws ":" ws any2 ws)", {"string", "ws", "any2"}, 0},
    {"any3-object", R"("{" ws (any3-member ("," ws any3-member)*)? "}")",
     {"ws", "any3-member"},
     2},
    {"any3-array", R"("[" ws (any2 ws ("," ws any2 ws)*)? "]")",
     {"ws", "any2"},
     2},
    {"any3", R"(any3-object | any3-array | any0)",
     {"any3-object", "any3-array", "any0"},
     0},
};
static_assert(kAnyValueDepth == 3, "kFixedRules spells out any0..any3");

const FixedRule* FindFixed(const std::string& name) {
  for (const FixedRule& r : kFixedRules) {
    if (name == r.name) return &r;
  }
  return nullptr;
}

// A JSON number literal that is an exact integer: -?(0|[1-9][0-9]*)(.0+)?
// with at most 16 digits. (JSON.stringify writes 1.0 as 1.)
std::optional<int64_t> ExactInteger(const JsonValue& v) {
  if (!v.is_number()) return std::nullopt;
  const std::string& t = v.text;
  size_t i = 0;
  const bool negative = i < t.size() && t[i] == '-';
  if (negative) ++i;
  int64_t value = 0;
  size_t digits = 0;
  while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
    if (++digits > 16) return std::nullopt;
    value = value * 10 + (t[i] - '0');
    ++i;
  }
  if (digits == 0) return std::nullopt;
  if (i < t.size() && t[i] == '.') {
    ++i;
    if (i == t.size()) return std::nullopt;
    while (i < t.size() && t[i] == '0') ++i;
  }
  if (i != t.size()) return std::nullopt;  // a fraction or an exponent
  return negative ? -value : value;
}

// "..." for GBNF: printable ASCII as is (but '"' and '\'), everything else
// escaped, so the grammar text is ASCII.
void AppendGbnfChar(char32_t c, std::string* out) {
  static constexpr char kHexDigits[] = "0123456789ABCDEF";
  if (c == '"') {
    out->append("\\\"");
  } else if (c == '\\') {
    out->append("\\\\");
  } else if (c >= 0x20 && c < 0x7F) {
    out->push_back(static_cast<char>(c));
  } else if (c == '\n') {
    out->append("\\n");
  } else if (c == '\t') {
    out->append("\\t");
  } else if (c == '\r') {
    out->append("\\r");
  } else {
    int digits;
    if (c < 0x100) {
      out->append("\\x");
      digits = 2;
    } else if (c < 0x10000) {
      out->append("\\u");
      digits = 4;
    } else {
      out->append("\\U");
      digits = 8;
    }
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
      out->push_back(kHexDigits[(c >> shift) & 0xF]);
    }
  }
}

std::string GbnfLiteral(const std::u32string& s) {
  std::string out = "\"";
  for (char32_t c : s) AppendGbnfChar(c, &out);
  out.push_back('"');
  return out;
}

// Code points of valid UTF-8 (everything here passed the strict reader or
// is ASCII).
std::u32string CodePoints(const std::string& s) {
  std::u32string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    size_t n = 1;
    char32_t cp = b;
    if (b >= 0xF0) {
      n = 4;
      cp = b & 0x07u;
    } else if (b >= 0xE0) {
      n = 3;
      cp = b & 0x0Fu;
    } else if (b >= 0xC0) {
      n = 2;
      cp = b & 0x1Fu;
    }
    for (size_t k = 1; k < n && i + k < s.size(); ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

// "{a,b}" and friends for a counted repetition; max < 0 means unbounded.
std::string Repeat(int64_t min, int64_t max) {
  if (max < 0) {
    if (min == 0) return "*";
    if (min == 1) return "+";
    return "{" + std::to_string(min) + ",}";
  }
  if (min == max) return "{" + std::to_string(min) + "}";
  if (min == 0 && max == 1) return "?";
  return "{" + std::to_string(min) + "," + std::to_string(max) + "}";
}

struct Fragment {
  // Rule name (fixed or generated).
  std::string rule;
  // Kind bits of the values it can start with.
  uint32_t kinds = 0;
  // Alternatives that can be alive at once while reading such a value.
  size_t live = 1;
};

class Converter {
 public:
  SchemaGrammar Convert(const JsonValue& root) {
    SchemaGrammar result;
    std::optional<Fragment> top = Schema(root, 0);
    if (!top) {
      result.status = status_;
      result.reason = reason_;
      return result;
    }
    std::string gbnf = "root ::= " + top->rule + "\n";
    for (const std::string& name : fixed_order_) {
      const FixedRule* r = FindFixed(name);
      gbnf += std::string(r->name) + " ::= " + r->body + "\n";
    }
    for (const auto& [name, body] : rules_) {
      gbnf += name + " ::= " + body + "\n";
    }
    if (gbnf.size() > kMaxGrammarBytes) {
      result.status = SchemaGrammarStatus::kTooLarge;
      result.reason = "grammar too large";
      return result;
    }
    result.status = SchemaGrammarStatus::kOk;
    result.gbnf = std::move(gbnf);
    return result;
  }

  // For the failure paths below: records the first failure.
  std::nullopt_t Fail(SchemaGrammarStatus status, std::string reason) {
    if (status_ == SchemaGrammarStatus::kOk) {
      status_ = status;
      reason_ = std::move(reason);
    }
    return std::nullopt;
  }

 private:
  // ---- Grammar assembly ----

  bool Budget(size_t body_bytes, size_t repeat) {
    bytes_ += body_bytes + 16;
    repeat_ += repeat;
    if (bytes_ > kMaxGrammarBytes || rules_.size() > kMaxGrammarRules) {
      Fail(SchemaGrammarStatus::kTooLarge, "grammar too large");
      return false;
    }
    if (repeat_ > kMaxRepetitionBudget) {
      Fail(SchemaGrammarStatus::kTooLarge, "too many counted repetitions");
      return false;
    }
    return true;
  }

  // The fixed rule |name| (and what it references), once.
  std::optional<std::string> UseFixed(const std::string& name) {
    if (std::find(fixed_order_.begin(), fixed_order_.end(), name) !=
        fixed_order_.end()) {
      return name;
    }
    const FixedRule* r = FindFixed(name);
    fixed_order_.push_back(name);
    if (!Budget(std::char_traits<char>::length(r->body), r->repeat)) {
      return std::nullopt;
    }
    for (const char* dep : r->deps) {
      if (dep && !UseFixed(dep)) return std::nullopt;
    }
    return name;
  }

  // A generated rule with |body|; identical bodies share one rule.
  std::optional<std::string> AddRule(std::string body, size_t repeat = 0) {
    auto it = rules_by_body_.find(body);
    if (it != rules_by_body_.end()) return it->second;
    std::string name = "r" + std::to_string(rules_.size() + 1);
    if (!Budget(body.size(), repeat)) return std::nullopt;
    rules_by_body_.emplace(body, name);
    rules_.emplace_back(name, std::move(body));
    return name;
  }

  std::optional<Fragment> FixedFragment(const std::string& name,
                                        uint32_t kinds) {
    if (!UseFixed(name)) return std::nullopt;
    return Fragment{name, kinds, 1};
  }

  // ---- Schemas ----

  std::optional<Fragment> Schema(const JsonValue& s, size_t depth) {
    if (depth > kMaxSchemaDepth) {
      return Fail(SchemaGrammarStatus::kTooDeep, "schema nested too deep");
    }
    if (s.is_bool()) {
      if (!s.boolean) {
        return Fail(SchemaGrammarStatus::kUnsatisfiable, "false schema");
      }
      return FixedFragment("any3", kKindAny);
    }
    if (!s.is_object()) {
      return Fail(SchemaGrammarStatus::kInvalidJson,
                  "a schema must be an object or a boolean");
    }
    for (const auto& [key, value] : s.members) {
      if (IsAnnotation(key) || OneOf(key, kHandled)) continue;
      if (OneOf(key, kKnownUnsupported)) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    "unsupported keyword \"" + key + "\"");
      }
      return Fail(SchemaGrammarStatus::kUnsupported, "unknown keyword");
    }
    if (const JsonValue* unique = s.Find("uniqueItems")) {
      if (!unique->is_bool()) {
        return Fail(SchemaGrammarStatus::kUnsupported, "bad \"uniqueItems\"");
      }
      if (unique->boolean) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    "unsupported keyword \"uniqueItems\"");
      }
    }

    const JsonValue* any_of = s.Find("anyOf");
    const JsonValue* one_of = s.Find("oneOf");
    if (any_of || one_of) {
      size_t keywords = 0;
      for (const auto& member : s.members) {
        if (!IsAnnotation(member.first)) ++keywords;
      }
      if (keywords != 1) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    "\"anyOf\"/\"oneOf\" combined with other keywords");
      }
      return Alternatives(any_of ? *any_of : *one_of, depth);
    }

    uint32_t types = 0;
    if (const JsonValue* type = s.Find("type")) {
      std::optional<uint32_t> parsed = Types(*type);
      if (!parsed) return std::nullopt;
      types = *parsed;
    }

    const JsonValue* enum_values = s.Find("enum");
    const JsonValue* const_value = s.Find("const");
    if (enum_values || const_value) {
      for (const auto& [key, value] : s.members) {
        if (IsAnnotation(key) || key == "type" || key == "enum" ||
            key == "const") {
          continue;
        }
        return Fail(SchemaGrammarStatus::kUnsupported,
                    "\"enum\"/\"const\" combined with other constraints");
      }
      return Literals(enum_values, const_value, types);
    }

    if (types == 0) {
      std::optional<uint32_t> inferred = InferType(s);
      if (!inferred) return std::nullopt;
      types = *inferred;
    }
    if (types == 0) return FixedFragment("any3", kKindAny);

    std::vector<Fragment> alternatives;
    for (const auto& [name, bit] : kTypeNames) {
      if (!(types & bit)) continue;
      std::optional<Fragment> f;
      switch (bit) {
        case kTypeObject:
          f = Object(s, depth);
          break;
        case kTypeArray:
          f = Array(s, depth);
          break;
        case kTypeString:
          f = String(s);
          break;
        case kTypeInteger:
          f = Integer(s);
          break;
        case kTypeNumber:
          f = Number(s);
          break;
        case kTypeBoolean:
          f = FixedFragment("boolean", kKindTrue | kKindFalse);
          break;
        case kTypeNull:
          f = FixedFragment("null", kKindNull);
          break;
      }
      if (!f) return std::nullopt;
      alternatives.push_back(std::move(*f));
    }
    return Union(std::move(alternatives));
  }

  std::optional<uint32_t> Types(const JsonValue& type) {
    auto one = [&](const JsonValue& v) -> std::optional<uint32_t> {
      if (v.is_string()) {
        for (const auto& [name, bit] : kTypeNames) {
          if (v.text == name) return bit;
        }
      }
      return Fail(SchemaGrammarStatus::kUnsupported, "unknown \"type\"");
    };
    if (!type.is_array()) return one(type);
    if (type.items.empty() || type.items.size() > kTypeNames.size()) {
      return Fail(SchemaGrammarStatus::kUnsupported, "bad \"type\" list");
    }
    uint32_t bits = 0;
    for (const JsonValue& item : type.items) {
      std::optional<uint32_t> bit = one(item);
      if (!bit) return std::nullopt;
      if (bits & *bit) {
        return Fail(SchemaGrammarStatus::kUnsupported, "bad \"type\" list");
      }
      bits |= *bit;
    }
    return bits;
  }

  // Without "type": the family of the shape keywords used (0 = any value).
  std::optional<uint32_t> InferType(const JsonValue& s) {
    uint32_t bits = 0;
    for (const auto& [key, value] : s.members) {
      if (key == "properties" || key == "required" ||
          key == "additionalProperties") {
        bits |= kTypeObject;
      } else if (key == "items" || key == "minItems" || key == "maxItems" ||
                 key == "uniqueItems") {
        bits |= kTypeArray;
      } else if (key == "minLength" || key == "maxLength") {
        bits |= kTypeString;
      } else if (key == "minimum" || key == "maximum" ||
                 key == "exclusiveMinimum" || key == "exclusiveMaximum") {
        bits |= kTypeNumber;
      }
    }
    if (bits & (bits - 1)) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "keywords of several types without \"type\"");
    }
    return bits;
  }

  // Alternatives of one value. Identical rules are merged; the live count
  // is, per first character, the sum over the alternatives that can start
  // with it.
  std::optional<Fragment> Union(std::vector<Fragment> alternatives) {
    std::vector<Fragment> unique;
    for (Fragment& f : alternatives) {
      auto same =
          std::find_if(unique.begin(), unique.end(),
                       [&](const Fragment& u) { return u.rule == f.rule; });
      if (same == unique.end()) unique.push_back(std::move(f));
    }
    if (unique.empty()) {
      return Fail(SchemaGrammarStatus::kUnsatisfiable, "no alternative");
    }
    if (unique.size() == 1) return std::move(unique.front());
    Fragment out;
    std::string body;
    for (const Fragment& f : unique) {
      if (!body.empty()) body += " | ";
      body += f.rule;
      out.kinds |= f.kinds;
    }
    out.live = 0;
    for (int k = 0; k < kKindCount; ++k) {
      size_t live = 0;
      for (const Fragment& f : unique) {
        if (f.kinds & (1u << k)) live += f.live;
      }
      out.live = std::max(out.live, live);
    }
    if (out.live > kMaxLiveAlternatives) {
      return Fail(SchemaGrammarStatus::kTooAmbiguous,
                  "too many overlapping alternatives");
    }
    std::optional<std::string> rule = AddRule(std::move(body));
    if (!rule) return std::nullopt;
    out.rule = std::move(*rule);
    return out;
  }

  std::optional<Fragment> Alternatives(const JsonValue& list, size_t depth) {
    if (!list.is_array() || list.items.empty()) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "\"anyOf\"/\"oneOf\" must be a non-empty array");
    }
    if (list.items.size() > kMaxAnyOf) {
      return Fail(SchemaGrammarStatus::kTooLarge,
                  "too many \"anyOf\"/\"oneOf\" alternatives");
    }
    std::vector<Fragment> alternatives;
    for (const JsonValue& item : list.items) {
      if (item.is_bool() && !item.boolean) continue;  // matches nothing
      std::optional<Fragment> f = Schema(item, depth + 1);
      if (!f) return std::nullopt;
      alternatives.push_back(std::move(*f));
    }
    return Union(std::move(alternatives));
  }

  // A count keyword (minItems, maxLength, ...): a non-negative integer at
  // most |cap|. nullopt with the failure recorded otherwise.
  std::optional<int64_t> Count(const JsonValue& s,
                               const char* key,
                               int64_t cap,
                               int64_t absent) {
    const JsonValue* v = s.Find(key);
    if (!v) return absent;
    std::optional<int64_t> n = ExactInteger(*v);
    if (!n || *n < 0) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  std::string("bad \"") + key + "\"");
    }
    if (*n > cap) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  std::string("\"") + key + "\" over " + std::to_string(cap));
    }
    return n;
  }

  std::optional<Fragment> Object(const JsonValue& s, size_t depth) {
    const JsonValue* properties = s.Find("properties");
    const JsonValue* required = s.Find("required");
    const JsonValue* additional = s.Find("additionalProperties");
    if (properties && !properties->is_object()) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "\"properties\" must be an object");
    }
    if (additional && !additional->is_bool() && !additional->is_object()) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "bad \"additionalProperties\"");
    }
    std::vector<std::string> required_names;
    if (required) {
      if (!required->is_array() || required->items.size() > kMaxProperties) {
        return Fail(SchemaGrammarStatus::kUnsupported, "bad \"required\"");
      }
      for (const JsonValue& name : required->items) {
        if (!name.is_string() ||
            std::find(required_names.begin(), required_names.end(),
                      name.text) != required_names.end()) {
          return Fail(SchemaGrammarStatus::kUnsupported, "bad \"required\"");
        }
        if (!properties || !properties->Find(name.text)) {
          return Fail(SchemaGrammarStatus::kUnsupported,
                      "\"required\" names a property not in \"properties\"");
        }
        required_names.push_back(name.text);
      }
    }

    if (!properties || properties->members.empty()) {
      if (additional && additional->is_object()) {
        // A map: string keys, values of one schema.
        std::optional<Fragment> value = Schema(*additional, depth + 1);
        if (!value || !UseFixed("ws") || !UseFixed("string")) {
          return std::nullopt;
        }
        std::optional<std::string> member =
            AddRule("string ws \":\" ws " + value->rule + " ws");
        if (!member) return std::nullopt;
        std::optional<std::string> rule = AddRule(
            "\"{\" ws (" + *member + " (\",\" ws " + *member + ")*)? \"}\"",
            2);
        if (!rule) return std::nullopt;
        return Fragment{*rule, kKindObject, value->live};
      }
      if (additional && additional->is_bool() && !additional->boolean) {
        if (!UseFixed("ws")) return std::nullopt;
        std::optional<std::string> rule = AddRule("\"{\" ws \"}\"");
        if (!rule) return std::nullopt;
        return Fragment{*rule, kKindObject, 1};
      }
      return FixedFragment("any3-object", kKindObject);
    }

    if (additional && additional->is_object()) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "\"additionalProperties\" schema with \"properties\"");
    }
    if (properties->members.size() > kMaxProperties) {
      return Fail(SchemaGrammarStatus::kTooLarge, "too many properties");
    }
    if (!UseFixed("ws")) return std::nullopt;

    // key ws ":" ws value ws, one rule per property, in the order given.
    struct Member {
      std::string rule;
      bool required;
    };
    std::vector<Member> members;
    size_t live = 1;
    for (const auto& [name, schema] : properties->members) {
      const bool is_required =
          std::find(required_names.begin(), required_names.end(), name) !=
          required_names.end();
      if (schema.is_bool() && !schema.boolean) {
        if (is_required) {
          return Fail(SchemaGrammarStatus::kUnsatisfiable,
                      "a required property matches nothing");
        }
        continue;  // never generated
      }
      std::optional<Fragment> value = Schema(schema, depth + 1);
      if (!value) return std::nullopt;
      live = std::max(live, value->live);
      std::optional<std::string> rule =
          AddRule(GbnfLiteral(CodePoints(JsonQuote(name))) + " ws \":\" ws " +
                  value->rule + " ws");
      if (!rule) return std::nullopt;
      members.push_back({std::move(*rule), is_required});
    }

    // While a key is read, every member that may come next is alive: the
    // optional ones up to (and including) the next required one.
    size_t run = 0;
    for (size_t i = members.size(); i-- > 0;) {
      run = members[i].required ? 1 : run + 1;
      live = std::max(live, run);
    }

    // Built from the last property back:
    //   rest_i  ::= "," ws member_i rest_i+1   (| rest_i+1 if optional)
    //   first_i ::= member_i rest_i+1          (| first_i+1 if optional)
    // so every required member appears, optional ones may be skipped, and
    // a comma separates each pair. Empty alternatives come first: llama.cpp
    // reads past a line break after a trailing '|'. rest_0 is never used,
    // and first_i only while every member before i is optional.
    size_t last_first = 0;
    while (last_first + 1 < members.size() && !members[last_first].required) {
      ++last_first;
    }
    std::string rest_next;   // "" = empty
    std::string first_next;  // "" = empty
    for (size_t i = members.size(); i-- > 0;) {
      const Member& m = members[i];
      const std::string tail = rest_next.empty() ? "" : " " + rest_next;
      if (i <= last_first) {
        std::string first = m.rule + tail;
        if (!m.required) {
          first = (first_next.empty() ? "| " : first_next + " | ") + first;
        }
        std::optional<std::string> first_rule = AddRule(std::move(first));
        if (!first_rule) return std::nullopt;
        first_next = std::move(*first_rule);
      }
      if (i > 0) {
        std::string rest = "\",\" ws " + m.rule + tail;
        if (!m.required) {
          rest = (rest_next.empty() ? "| " : rest_next + " | ") + rest;
        }
        std::optional<std::string> rest_rule = AddRule(std::move(rest));
        if (!rest_rule) return std::nullopt;
        rest_next = std::move(*rest_rule);
      }
    }
    std::optional<std::string> rule =
        AddRule("\"{\" ws " + first_next + " \"}\"");
    if (!rule) return std::nullopt;
    return Fragment{*rule, kKindObject, live};
  }

  std::optional<Fragment> Array(const JsonValue& s, size_t depth) {
    std::optional<int64_t> min_items = Count(s, "minItems", kMaxItemsBound, 0);
    if (!min_items) return std::nullopt;
    std::optional<int64_t> max_items = Count(s, "maxItems", kMaxItemsBound, -1);
    if (!max_items) return std::nullopt;
    if (*max_items >= 0 && *min_items > *max_items) {
      return Fail(SchemaGrammarStatus::kUnsatisfiable,
                  "\"minItems\" over \"maxItems\"");
    }
    const JsonValue* items = s.Find("items");
    std::optional<Fragment> item;
    if (!items) {
      item = FixedFragment("any2", kKindAny);
    } else if (items->is_array()) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "\"items\" as an array (tuples)");
    } else if (items->is_bool() && !items->boolean) {
      if (*min_items > 0) {
        return Fail(SchemaGrammarStatus::kUnsatisfiable,
                    "\"items\": false with \"minItems\"");
      }
      max_items = 0;
    } else {
      item = Schema(*items, depth + 1);
    }
    if (!UseFixed("ws")) return std::nullopt;
    if (*max_items == 0) {
      std::optional<std::string> rule = AddRule("\"[\" ws \"]\"");
      if (!rule) return std::nullopt;
      return Fragment{*rule, kKindArray, 1};
    }
    if (!item) return std::nullopt;
    // item ws ("," ws item ws){min-1,max-1}
    const int64_t more_min = std::max<int64_t>(*min_items - 1, 0);
    const int64_t more_max = *max_items < 0 ? -1 : *max_items - 1;
    std::string body = item->rule + " ws";
    size_t repeat = 0;
    if (more_max != 0) {
      body += " (\",\" ws " + item->rule + " ws)" + Repeat(more_min, more_max);
      repeat = static_cast<size_t>(more_max < 0 ? more_min + 1 : more_max);
    }
    std::optional<std::string> list = AddRule(std::move(body), repeat);
    if (!list) return std::nullopt;
    std::optional<std::string> rule = AddRule(
        "\"[\" ws " + *list + (*min_items == 0 ? "?" : "") + " \"]\"");
    if (!rule) return std::nullopt;
    return Fragment{*rule, kKindArray, item->live};
  }

  std::optional<Fragment> String(const JsonValue& s) {
    std::optional<int64_t> min_length =
        Count(s, "minLength", kMaxLengthBound, 0);
    if (!min_length) return std::nullopt;
    std::optional<int64_t> max_length =
        Count(s, "maxLength", kMaxLengthBound, -1);
    if (!max_length) return std::nullopt;
    if (*max_length >= 0 && *min_length > *max_length) {
      return Fail(SchemaGrammarStatus::kUnsatisfiable,
                  "\"minLength\" over \"maxLength\"");
    }
    if (*min_length == 0 && *max_length < 0) {
      return FixedFragment("string", kKindString);
    }
    if (!UseFixed("char")) return std::nullopt;
    const size_t repeat = static_cast<size_t>(
        *max_length < 0 ? *min_length + 1 : *max_length);
    std::optional<std::string> rule = AddRule(
        "\"\\\"\" char" + Repeat(*min_length, *max_length) + " \"\\\"\"",
        repeat);
    if (!rule) return std::nullopt;
    return Fragment{*rule, kKindString, 1};
  }

  // Inclusive integer bounds from minimum/maximum/exclusive*; |integer|
  // allows exclusive bounds (for numbers they would need a fraction).
  struct Bounds {
    std::optional<int64_t> low;
    std::optional<int64_t> high;
  };
  std::optional<Bounds> IntegerBounds(const JsonValue& s, bool integer) {
    Bounds b;
    struct Key {
      const char* name;
      bool is_low;
      int64_t adjust;
    };
    constexpr Key kKeys[] = {{"minimum", true, 0},
                             {"maximum", false, 0},
                             {"exclusiveMinimum", true, 1},
                             {"exclusiveMaximum", false, -1}};
    for (const Key& key : kKeys) {
      const JsonValue* v = s.Find(key.name);
      if (!v) continue;
      if (key.adjust != 0 && !integer) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    std::string("\"") + key.name + "\" on a number");
      }
      std::optional<int64_t> n = ExactInteger(*v);
      constexpr int64_t kLimit = 1000000000000000;  // 10^15
      if (!n || *n > kLimit || *n < -kLimit) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    std::string("\"") + key.name +
                        "\" must be a whole number up to 10^15");
      }
      const int64_t bound = *n + key.adjust;
      std::optional<int64_t>& slot = key.is_low ? b.low : b.high;
      slot = !slot ? bound
                   : (key.is_low ? std::max(*slot, bound)
                                 : std::min(*slot, bound));
    }
    if (b.low && b.high && *b.low > *b.high) {
      return Fail(SchemaGrammarStatus::kUnsatisfiable, "empty numeric range");
    }
    return b;
  }

  std::optional<std::string> IntegerRange(int64_t low, int64_t high) {
    std::vector<std::string> literals;
    for (int64_t v = low; v <= high; ++v) literals.push_back(std::to_string(v));
    return LiteralSet(literals);
  }

  std::optional<Fragment> Integer(const JsonValue& s) {
    std::optional<Bounds> b = IntegerBounds(s, /*integer=*/true);
    if (!b) return std::nullopt;
    if (!b->low && !b->high) return FixedFragment("integer", kKindNumber);
    if (b->low && !b->high) {
      if (*b->low == 0) return FixedFragment("int-part", kKindNumber);
      if (*b->low == 1) return FixedFragment("pos-integer", kKindNumber);
    }
    if (!b->low || !b->high) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "integer bounds other than a range or a minimum of 0 or 1");
    }
    if (*b->high - *b->low + 1 > kMaxRangeSize) {
      return Fail(SchemaGrammarStatus::kUnsupported, "integer range too wide");
    }
    std::optional<std::string> rule = IntegerRange(*b->low, *b->high);
    if (!rule) return std::nullopt;
    return Fragment{*rule, kKindNumber, 1};
  }

  std::optional<Fragment> Number(const JsonValue& s) {
    std::optional<Bounds> b = IntegerBounds(s, /*integer=*/false);
    if (!b) return std::nullopt;
    if (!b->low && !b->high) return FixedFragment("number", kKindNumber);
    if (b->low && !b->high && *b->low == 0) {
      return FixedFragment("nonneg-number", kKindNumber);
    }
    if (!b->low || !b->high || *b->low < 0) {
      return Fail(SchemaGrammarStatus::kUnsupported,
                  "number bounds other than a non-negative range or a "
                  "minimum of 0");
    }
    if (*b->high - *b->low + 1 > kMaxRangeSize) {
      return Fail(SchemaGrammarStatus::kUnsupported, "number range too wide");
    }
    // An integer part in [low, high-1] with any fraction, or high itself
    // with a zero fraction.
    if (!UseFixed("zero-frac")) return std::nullopt;
    const std::string high = GbnfLiteral(CodePoints(std::to_string(*b->high)));
    std::string body = high + " zero-frac?";
    if (*b->low < *b->high) {
      std::optional<std::string> below = IntegerRange(*b->low, *b->high - 1);
      if (!below || !UseFixed("frac")) return std::nullopt;
      body = *below + " frac? | " + body;
    }
    std::optional<std::string> rule = AddRule(std::move(body));
    if (!rule) return std::nullopt;
    return Fragment{*rule, kKindNumber, 1};
  }

  // enum / const: the values' compact JSON, as a trie (one live alternative).
  std::optional<Fragment> Literals(const JsonValue* enum_values,
                                   const JsonValue* const_value,
                                   uint32_t types) {
    std::vector<const JsonValue*> values;
    if (enum_values) {
      if (!enum_values->is_array()) {
        return Fail(SchemaGrammarStatus::kUnsupported,
                    "\"enum\" must be an array");
      }
      if (enum_values->items.size() > kMaxEnumValues) {
        return Fail(SchemaGrammarStatus::kTooLarge, "too many \"enum\" values");
      }
      for (const JsonValue& v : enum_values->items) values.push_back(&v);
    }
    std::vector<std::string> literals;
    uint32_t kinds = 0;
    for (const JsonValue* v : values) {
      if (types && !HasType(*v, types)) continue;
      std::string literal = WriteJson(*v);
      if (literal.size() > kMaxEnumValueBytes) {
        return Fail(SchemaGrammarStatus::kTooLarge, "\"enum\" value too long");
      }
      if (std::find(literals.begin(), literals.end(), literal) ==
          literals.end()) {
        literals.push_back(std::move(literal));
      }
    }
    if (const_value) {
      std::string literal = WriteJson(*const_value);
      if (literal.size() > kMaxEnumValueBytes) {
        return Fail(SchemaGrammarStatus::kTooLarge, "\"const\" value too long");
      }
      const bool allowed =
          (!types || HasType(*const_value, types)) &&
          (!enum_values || std::find(literals.begin(), literals.end(),
                                     literal) != literals.end());
      literals.clear();
      if (allowed) literals.push_back(std::move(literal));
    }
    if (literals.empty()) {
      return Fail(SchemaGrammarStatus::kUnsatisfiable,
                  "no \"enum\"/\"const\" value matches");
    }
    for (const std::string& l : literals) kinds |= KindOfFirstChar(l[0]);
    std::optional<std::string> rule = LiteralSet(literals);
    if (!rule) return std::nullopt;
    return Fragment{*rule, kinds, 1};
  }

  static bool HasType(const JsonValue& v, uint32_t types) {
    switch (v.type) {
      case JsonValue::Type::kNull:
        return types & kTypeNull;
      case JsonValue::Type::kBool:
        return types & kTypeBoolean;
      case JsonValue::Type::kString:
        return types & kTypeString;
      case JsonValue::Type::kArray:
        return types & kTypeArray;
      case JsonValue::Type::kObject:
        return types & kTypeObject;
      case JsonValue::Type::kNumber:
        return (types & kTypeNumber) ||
               ((types & kTypeInteger) && ExactInteger(v).has_value());
    }
    return false;
  }

  // A rule matching exactly the strings in |literals| (non-empty, distinct,
  // valid UTF-8), built as a trie so at most two alternatives (a literal
  // ending here, or a longer one) are ever alive. Each branching node is a
  // rule; chains of single children are one quoted literal. Iterative: a
  // literal can be ~1000 characters long.
  std::optional<std::string> LiteralSet(
      const std::vector<std::string>& literals) {
    struct Node {
      std::map<char32_t, size_t> children;
      bool terminal = false;
    };
    std::vector<Node> nodes(1);
    for (const std::string& literal : literals) {
      size_t n = 0;
      for (char32_t c : CodePoints(literal)) {
        auto it = nodes[n].children.find(c);
        if (it == nodes[n].children.end()) {
          nodes.push_back(Node());
          it = nodes[n].children.emplace(c, nodes.size() - 1).first;
        }
        n = it->second;
      }
      nodes[n].terminal = true;
    }
    // Follows a chain of non-terminal single-child nodes from the edge
    // |c| -> |n|; returns the label and the node where it ends.
    auto chain = [&](char32_t c, size_t n) {
      std::u32string label(1, c);
      while (!nodes[n].terminal && nodes[n].children.size() == 1) {
        label.push_back(nodes[n].children.begin()->first);
        n = nodes[n].children.begin()->second;
      }
      return std::make_pair(label, n);
    };
    // Which nodes need a rule: the root and every chain end with children.
    std::vector<bool> needs(nodes.size(), false);
    needs[0] = true;
    std::vector<size_t> todo = {0};
    while (!todo.empty()) {
      const size_t n = todo.back();
      todo.pop_back();
      for (const auto& [c, child] : nodes[n].children) {
        const size_t end = chain(c, child).second;
        if (!nodes[end].children.empty() && !needs[end]) {
          needs[end] = true;
          todo.push_back(end);
        }
      }
    }
    // Children have larger indices than their parents: emit bottom-up.
    std::vector<std::string> names(nodes.size());
    for (size_t n = nodes.size(); n-- > 0;) {
      if (!needs[n]) continue;
      std::string body = nodes[n].terminal ? "| " : "";
      bool first = true;
      for (const auto& [c, child] : nodes[n].children) {
        if (!first) body += " | ";
        first = false;
        auto [label, end] = chain(c, child);
        body += GbnfLiteral(label);
        if (!nodes[end].children.empty()) body += " " + names[end];
      }
      std::optional<std::string> name = AddRule(std::move(body));
      if (!name) return std::nullopt;
      names[n] = std::move(*name);
    }
    return names[0];
  }

  std::map<std::string, std::string> rules_by_body_;
  std::vector<std::pair<std::string, std::string>> rules_;
  std::vector<std::string> fixed_order_;
  size_t bytes_ = 0;
  size_t repeat_ = 0;
  SchemaGrammarStatus status_ = SchemaGrammarStatus::kOk;
  std::string reason_;
};

}  // namespace

SchemaGrammar JsonSchemaToGrammar(std::string_view schema_json) {
  SchemaGrammar result;
  JsonLimits limits;
  limits.max_bytes = kMaxSchemaBytes;
  limits.max_depth = kMaxSchemaJsonDepth;
  limits.max_values = kMaxSchemaValues;
  JsonError error = JsonError::kNone;
  std::optional<JsonValue> schema = ParseJson(schema_json, limits, &error);
  if (!schema) {
    switch (error) {
      case JsonError::kTooLarge:
      case JsonError::kTooManyValues:
        result.status = SchemaGrammarStatus::kTooLarge;
        result.reason = "schema too large";
        break;
      case JsonError::kTooDeep:
        result.status = SchemaGrammarStatus::kTooDeep;
        result.reason = "schema nested too deep";
        break;
      case JsonError::kNone:
      case JsonError::kSyntax:
      case JsonError::kDuplicateKey:
        result.status = SchemaGrammarStatus::kInvalidJson;
        result.reason = "not strict JSON";
        break;
    }
    return result;
  }
  if (!schema->is_object() && !schema->is_bool()) {
    result.status = SchemaGrammarStatus::kInvalidJson;
    result.reason = "a schema must be an object or a boolean";
    return result;
  }
  return Converter().Convert(*schema);
}

}  // namespace molt_ai
