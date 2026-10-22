// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/bounded_json.h"

#include <algorithm>
#include <cstdint>

namespace molt_ai {

namespace {

constexpr char kHex[] = "0123456789abcdef";

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
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

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits)
      : s_(text), limits_(limits) {}

  std::optional<JsonValue> Run(JsonError* error) {
    std::optional<JsonValue> result;
    if (s_.size() > limits_.max_bytes) {
      error_ = JsonError::kTooLarge;
    } else {
      SkipWhitespace();
      JsonValue value;
      if (ParseValue(0, &value)) {
        SkipWhitespace();
        if (pos_ == s_.size()) {
          result = std::move(value);
        } else {
          Fail(JsonError::kSyntax);
        }
      }
    }
    if (error) *error = result ? JsonError::kNone : error_;
    return result;
  }

 private:
  bool Fail(JsonError error) {
    if (error_ == JsonError::kNone) error_ = error;
    return false;
  }

  bool AtEnd() const { return pos_ >= s_.size(); }
  char Peek() const { return AtEnd() ? '\0' : s_[pos_]; }

  void SkipWhitespace() {
    while (!AtEnd()) {
      const char c = s_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool Literal(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) return Fail(JsonError::kSyntax);
    pos_ += word.size();
    return true;
  }

  bool CountValue() {
    if (++values_ > limits_.max_values) return Fail(JsonError::kTooManyValues);
    return true;
  }

  // |depth|: containers already open around this value.
  bool ParseValue(size_t depth, JsonValue* out) {
    if (!CountValue()) return false;
    switch (Peek()) {
      case '{':
        return ParseObject(depth + 1, out);
      case '[':
        return ParseArray(depth + 1, out);
      case '"':
        out->type = JsonValue::Type::kString;
        return ParseString(&out->text);
      case 't':
        out->type = JsonValue::Type::kBool;
        out->boolean = true;
        return Literal("true");
      case 'f':
        out->type = JsonValue::Type::kBool;
        out->boolean = false;
        return Literal("false");
      case 'n':
        out->type = JsonValue::Type::kNull;
        return Literal("null");
      default:
        out->type = JsonValue::Type::kNumber;
        return ParseNumber(&out->text);
    }
  }

  bool ParseObject(size_t depth, JsonValue* out) {
    if (depth > limits_.max_depth) return Fail(JsonError::kTooDeep);
    out->type = JsonValue::Type::kObject;
    ++pos_;  // '{'
    SkipWhitespace();
    if (Peek() == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      std::string key;
      if (Peek() != '"' || !ParseString(&key)) return Fail(JsonError::kSyntax);
      SkipWhitespace();
      if (Peek() != ':') return Fail(JsonError::kSyntax);
      ++pos_;
      SkipWhitespace();
      JsonValue value;
      if (!ParseValue(depth, &value)) return false;
      out->members.emplace_back(std::move(key), std::move(value));
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        SkipWhitespace();
        continue;
      }
      if (Peek() != '}') return Fail(JsonError::kSyntax);
      ++pos_;
      break;
    }
    // Duplicate keys: sort a list of pointers (n log n, no quadratic scan).
    std::vector<const std::string*> keys;
    keys.reserve(out->members.size());
    for (const auto& member : out->members) keys.push_back(&member.first);
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) {
                return *a < *b;
              });
    for (size_t i = 1; i < keys.size(); ++i) {
      if (*keys[i - 1] == *keys[i]) return Fail(JsonError::kDuplicateKey);
    }
    return true;
  }

  bool ParseArray(size_t depth, JsonValue* out) {
    if (depth > limits_.max_depth) return Fail(JsonError::kTooDeep);
    out->type = JsonValue::Type::kArray;
    ++pos_;  // '['
    SkipWhitespace();
    if (Peek() == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue value;
      if (!ParseValue(depth, &value)) return false;
      out->items.push_back(std::move(value));
      SkipWhitespace();
      if (Peek() == ',') {
        ++pos_;
        SkipWhitespace();
        continue;
      }
      if (Peek() != ']') return Fail(JsonError::kSyntax);
      ++pos_;
      return true;
    }
  }

  bool ParseNumber(std::string* out) {
    const size_t start = pos_;
    if (Peek() == '-') ++pos_;
    if (Peek() == '0') {
      ++pos_;
    } else if (Peek() >= '1' && Peek() <= '9') {
      while (IsDigit(Peek())) ++pos_;
    } else {
      return Fail(JsonError::kSyntax);
    }
    if (Peek() == '.') {
      ++pos_;
      if (!IsDigit(Peek())) return Fail(JsonError::kSyntax);
      while (IsDigit(Peek())) ++pos_;
    }
    if (Peek() == 'e' || Peek() == 'E') {
      ++pos_;
      if (Peek() == '+' || Peek() == '-') ++pos_;
      if (!IsDigit(Peek())) return Fail(JsonError::kSyntax);
      while (IsDigit(Peek())) ++pos_;
    }
    out->assign(s_.substr(start, pos_ - start));
    return true;
  }

  bool ReadHex4(uint32_t* out) {
    if (s_.size() - pos_ < 4) return Fail(JsonError::kSyntax);
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
      const int h = HexValue(s_[pos_ + i]);
      if (h < 0) return Fail(JsonError::kSyntax);
      v = (v << 4) | static_cast<uint32_t>(h);
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  // One strictly valid UTF-8 character starting at pos_ (a lead byte >=
  // 0x80), appended to |out|.
  bool ReadUtf8Char(std::string* out) {
    const auto b0 = static_cast<unsigned char>(s_[pos_]);
    size_t n;
    uint32_t cp;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      n = 2;
      cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      n = 3;
      cp = b0 & 0x0Fu;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      n = 4;
      cp = b0 & 0x07u;
    } else {
      return Fail(JsonError::kSyntax);
    }
    if (s_.size() - pos_ < n) return Fail(JsonError::kSyntax);
    for (size_t i = 1; i < n; ++i) {
      const auto b = static_cast<unsigned char>(s_[pos_ + i]);
      if ((b & 0xC0) != 0x80) return Fail(JsonError::kSyntax);
      cp = (cp << 6) | (b & 0x3Fu);
    }
    const bool overlong = (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
    if (overlong || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      return Fail(JsonError::kSyntax);
    }
    out->append(s_.substr(pos_, n));
    pos_ += n;
    return true;
  }

  bool ParseString(std::string* out) {
    ++pos_;  // '"'
    while (true) {
      if (AtEnd()) return Fail(JsonError::kSyntax);
      const auto c = static_cast<unsigned char>(s_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return Fail(JsonError::kSyntax);
      if (c >= 0x80) {
        if (!ReadUtf8Char(out)) return false;
        continue;
      }
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (AtEnd()) return Fail(JsonError::kSyntax);
      const char e = s_[pos_++];
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out->push_back(e);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t cp;
          if (!ReadHex4(&cp)) return false;
          if (cp >= 0xDC00 && cp <= 0xDFFF) return Fail(JsonError::kSyntax);
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // A high surrogate must be followed by an escaped low one.
            uint32_t low;
            if (s_.substr(pos_, 2) != "\\u") return Fail(JsonError::kSyntax);
            pos_ += 2;
            if (!ReadHex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail(JsonError::kSyntax);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return Fail(JsonError::kSyntax);
      }
    }
  }

  const std::string_view s_;
  const JsonLimits limits_;
  size_t pos_ = 0;
  size_t values_ = 0;
  JsonError error_ = JsonError::kNone;
};

void Write(const JsonValue& v, std::string* out) {
  switch (v.type) {
    case JsonValue::Type::kNull:
      out->append("null");
      return;
    case JsonValue::Type::kBool:
      out->append(v.boolean ? "true" : "false");
      return;
    case JsonValue::Type::kNumber:
      out->append(v.text);
      return;
    case JsonValue::Type::kString:
      out->append(JsonQuote(v.text));
      return;
    case JsonValue::Type::kArray:
      out->push_back('[');
      for (size_t i = 0; i < v.items.size(); ++i) {
        if (i > 0) out->push_back(',');
        Write(v.items[i], out);
      }
      out->push_back(']');
      return;
    case JsonValue::Type::kObject:
      out->push_back('{');
      for (size_t i = 0; i < v.members.size(); ++i) {
        if (i > 0) out->push_back(',');
        out->append(JsonQuote(v.members[i].first));
        out->push_back(':');
        Write(v.members[i].second, out);
      }
      out->push_back('}');
      return;
  }
}

}  // namespace

JsonValue::JsonValue() = default;
JsonValue::~JsonValue() = default;
JsonValue::JsonValue(const JsonValue&) = default;
JsonValue& JsonValue::operator=(const JsonValue&) = default;
JsonValue::JsonValue(JsonValue&&) = default;
JsonValue& JsonValue::operator=(JsonValue&&) = default;

const JsonValue* JsonValue::Find(std::string_view key) const {
  for (const auto& [name, value] : members) {
    if (name == key) return &value;
  }
  return nullptr;
}

std::optional<JsonValue> ParseJson(std::string_view text,
                                   const JsonLimits& limits,
                                   JsonError* error) {
  return Parser(text, limits).Run(error);
}

std::string WriteJson(const JsonValue& value) {
  std::string out;
  Write(value, &out);
  return out;
}

std::string JsonQuote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (c < 0x20) {
          out.append("\\u00");
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace molt_ai
