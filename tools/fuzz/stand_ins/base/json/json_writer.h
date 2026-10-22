// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/json/json_writer.h, used
// only by tools/fuzz/molt_fuzz.py. Escaping as base/json/string_escape.cc
// does it: \b \f \n \r \t \\ \" short, '<' as <, U+2028 / U+2029
// escaped, other control characters as \u00XX, invalid UTF-8 replaced with
// U+FFFD. Doubles round-trip (%.17g) and keep a ".0" when integral, as
// upstream preserves the double type; Write() fails on NaN and infinities.
// Keys come out in DictValue's (sorted) order. Pretty printing indents
// with three spaces.

#ifndef MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_WRITER_H_
#define MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_WRITER_H_

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace base {

class JSONWriter {
 public:
  enum Options {
    OPTIONS_OMIT_BINARY_VALUES = 1 << 0,
    OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION = 1 << 1,
    OPTIONS_PRETTY_PRINT = 1 << 2,
  };

  static bool Write(const Value& v, std::string* out) {
    return WriteWithOptions(v, 0, out);
  }
  static bool Write(const DictValue& d, std::string* out) {
    return WriteWithOptions(d, 0, out);
  }
  static bool Write(const ListValue& l, std::string* out) {
    return WriteWithOptions(l, 0, out);
  }
  static std::optional<std::string> Write(const Value& v) {
    std::string out;
    if (!Write(v, &out)) return std::nullopt;
    return out;
  }
  static std::optional<std::string> Write(const DictValue& d) {
    std::string out;
    if (!Write(d, &out)) return std::nullopt;
    return out;
  }
  static std::optional<std::string> Write(const ListValue& l) {
    std::string out;
    if (!Write(l, &out)) return std::nullopt;
    return out;
  }
  static bool WriteWithOptions(const Value& v, int options, std::string* out) {
    out->clear();
    return Writer(options, out).Append(v, 0);
  }
  static bool WriteWithOptions(const DictValue& d,
                               int options,
                               std::string* out) {
    out->clear();
    return Writer(options, out).AppendDict(d, 0);
  }
  static bool WriteWithOptions(const ListValue& l,
                               int options,
                               std::string* out) {
    out->clear();
    return Writer(options, out).AppendList(l, 0);
  }
  static std::optional<std::string> WriteJsonWithOptions(const Value& v,
                                                         int options) {
    std::string out;
    if (!WriteWithOptions(v, options, &out)) return std::nullopt;
    return out;
  }

 private:
  class Writer {
   public:
    Writer(int options, std::string* out)
        : pretty_(options & OPTIONS_PRETTY_PRINT),
          omit_double_type_(options & OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION),
          out_(out) {}

    bool Append(const Value& v, int depth) {
      switch (v.type()) {
        case Value::Type::NONE:
          *out_ += "null";
          return true;
        case Value::Type::BOOLEAN:
          *out_ += v.GetBool() ? "true" : "false";
          return true;
        case Value::Type::INTEGER:
          *out_ += std::to_string(v.GetInt());
          return true;
        case Value::Type::DOUBLE:
          return AppendDouble(v.GetDouble());
        case Value::Type::STRING:
          AppendString(v.GetString());
          return true;
        case Value::Type::BINARY:
          return false;
        case Value::Type::DICT:
          return AppendDict(v.GetDict(), depth);
        case Value::Type::LIST:
          return AppendList(v.GetList(), depth);
      }
      return false;
    }

    bool AppendDict(const DictValue& d, int depth) {
      out_->push_back('{');
      bool first = true;
      bool ok = true;
      for (const auto& [key, value] : d) {
        if (!first) out_->push_back(',');
        first = false;
        Newline(depth + 1);
        AppendString(key);
        out_->push_back(':');
        if (pretty_) out_->push_back(' ');
        ok = Append(value, depth + 1) && ok;
      }
      if (!first) Newline(depth);
      out_->push_back('}');
      return ok;
    }

    bool AppendList(const ListValue& l, int depth) {
      out_->push_back('[');
      bool first = true;
      bool ok = true;
      for (const Value& v : l) {
        if (!first) out_->push_back(',');
        first = false;
        Newline(depth + 1);
        ok = Append(v, depth + 1) && ok;
      }
      if (!first) Newline(depth);
      out_->push_back(']');
      return ok;
    }

   private:
    void Newline(int depth) {
      if (!pretty_) return;
      out_->push_back('\n');
      out_->append(static_cast<size_t>(depth) * 3, ' ');
    }

    bool AppendDouble(double d) {
      if (!std::isfinite(d)) return false;
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      std::string s = buf;
      if (!omit_double_type_ && s.find_first_of(".eE") == std::string::npos) {
        s += ".0";
      }
      *out_ += s;
      return true;
    }

    // One code point of valid UTF-8 at |s[i]|, or 0 for invalid input.
    static size_t Utf8Length(std::string_view s, size_t i) {
      const auto c = static_cast<unsigned char>(s[i]);
      size_t n;
      uint32_t cp;
      if (c < 0x80) return 1;
      if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
      } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
      } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
      } else {
        return 0;
      }
      if (i + n > s.size()) return 0;
      for (size_t k = 1; k < n; ++k) {
        const auto cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
      }
      if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
          cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
      }
      return n;
    }

    void AppendString(std::string_view s) {
      out_->push_back('"');
      for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = Utf8Length(s, i);
        if (n == 0) {
          *out_ += "\xEF\xBF\xBD";
          ++i;
          continue;
        }
        if (n > 1) {
          if (s.substr(i, n) == "\xE2\x80\xA8") {
            *out_ += "\\u2028";
          } else if (s.substr(i, n) == "\xE2\x80\xA9") {
            *out_ += "\\u2029";
          } else {
            out_->append(s.substr(i, n));
          }
          i += n;
          continue;
        }
        switch (c) {
          case '\b':
            *out_ += "\\b";
            break;
          case '\f':
            *out_ += "\\f";
            break;
          case '\n':
            *out_ += "\\n";
            break;
          case '\r':
            *out_ += "\\r";
            break;
          case '\t':
            *out_ += "\\t";
            break;
          case '\\':
            *out_ += "\\\\";
            break;
          case '"':
            *out_ += "\\\"";
            break;
          case '<':
            *out_ += "\\u003C";
            break;
          default:
            if (c < 0x20) {
              char buf[8];
              std::snprintf(buf, sizeof(buf), "\\u%04X", c);
              *out_ += buf;
            } else {
              out_->push_back(static_cast<char>(c));
            }
        }
        ++i;
      }
      out_->push_back('"');
    }

    const bool pretty_;
    const bool omit_double_type_;
    std::string* out_;
  };
};

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_WRITER_H_
