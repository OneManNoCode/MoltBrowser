// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/json/json_reader.h (the
// real one is Rust, serde_json_lenient), used only by
// tools/fuzz/molt_fuzz.py. It keeps the parts of the contract MoltBrowser's
// parsers rely on:
//  * one value, optional surrounding whitespace, nothing after it;
//  * JSON_PARSE_RFC: no comments, trailing commas, single quotes,
//    NaN/Infinity, \x escapes or raw control characters in strings;
//    JSON_ALLOW_TRAILING_COMMAS allows "[1,]" and "{"a":1,}";
//  * strings must be valid UTF-8; \u escapes decode to UTF-8 (surrogate
//    pairs joined, lone surrogates refused); \u0000 is kept;
//  * integers that fit an int are ints, all other numbers doubles; a number
//    beyond double range is refused;
//  * duplicate keys: the last one wins;
//  * at most |max_depth| nested containers.
// As in the real header, |options| has no default: callers must say which
// grammar they accept.

#ifndef MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_READER_H_
#define MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_READER_H_

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace base {

enum JSONParserOptions {
  JSON_PARSE_RFC = 0,
  JSON_ALLOW_TRAILING_COMMAS = 1 << 0,
  JSON_REPLACE_INVALID_CHARACTERS = 1 << 1,
  JSON_ALLOW_COMMENTS = 1 << 2,
  JSON_ALLOW_VERT_TAB = 1 << 3,
  JSON_ALLOW_X_ESCAPES = 1 << 4,
  JSON_ALLOW_NEWLINES_IN_STRINGS = 1 << 5,
};

class JSONReader {
 public:
  static constexpr size_t kStackMaxDepth = 200;

  static std::optional<Value> Read(std::string_view json,
                                   int options,
                                   size_t max_depth = kStackMaxDepth) {
    Parser p(json, options, max_depth);
    std::optional<Value> v = p.ParseValue(0);
    if (!v) return std::nullopt;
    p.SkipWs();
    if (p.pos != json.size()) return std::nullopt;
    return v;
  }

  static std::optional<DictValue> ReadDict(std::string_view json,
                                           int options,
                                           size_t max_depth = kStackMaxDepth) {
    std::optional<Value> v = Read(json, options, max_depth);
    if (!v || !v->is_dict()) return std::nullopt;
    return std::move(v->GetDict());
  }

  static std::optional<ListValue> ReadList(std::string_view json,
                                           int options,
                                           size_t max_depth = kStackMaxDepth) {
    std::optional<Value> v = Read(json, options, max_depth);
    if (!v || !v->is_list()) return std::nullopt;
    return std::move(v->GetList());
  }

 private:
  struct Parser {
    Parser(std::string_view s, int o, size_t d)
        : in(s), options(o), max_depth(d) {}
    std::string_view in;
    int options;
    size_t max_depth;
    size_t pos = 0;

    bool trailing_commas() const {
      return options & JSON_ALLOW_TRAILING_COMMAS;
    }

    void SkipWs() {
      while (pos < in.size() && (in[pos] == ' ' || in[pos] == '\t' ||
                                 in[pos] == '\n' || in[pos] == '\r')) {
        ++pos;
      }
    }
    bool Lit(std::string_view w) {
      if (in.substr(pos, w.size()) != w) return false;
      pos += w.size();
      return true;
    }

    std::optional<Value> ParseValue(size_t depth) {
      SkipWs();
      if (pos >= in.size()) return std::nullopt;
      const char c = in[pos];
      if (c == '{') return ParseObject(depth + 1);
      if (c == '[') return ParseArray(depth + 1);
      if (c == '"') {
        std::optional<std::string> s = ParseString();
        if (!s) return std::nullopt;
        return Value(std::move(*s));
      }
      if (Lit("true")) return Value(true);
      if (Lit("false")) return Value(false);
      if (Lit("null")) return Value();
      return ParseNumber();
    }

    std::optional<Value> ParseObject(size_t depth) {
      if (depth > max_depth) return std::nullopt;
      ++pos;  // {
      DictValue d;
      SkipWs();
      if (pos < in.size() && in[pos] == '}') {
        ++pos;
        return Value(std::move(d));
      }
      while (true) {
        SkipWs();
        if (pos >= in.size() || in[pos] != '"') return std::nullopt;
        std::optional<std::string> key = ParseString();
        if (!key) return std::nullopt;
        SkipWs();
        if (pos >= in.size() || in[pos] != ':') return std::nullopt;
        ++pos;
        std::optional<Value> v = ParseValue(depth);
        if (!v) return std::nullopt;
        d.Set(*key, std::move(*v));
        SkipWs();
        if (pos >= in.size()) return std::nullopt;
        if (in[pos] == ',') {
          ++pos;
          SkipWs();
          if (trailing_commas() && pos < in.size() && in[pos] == '}') {
            ++pos;
            return Value(std::move(d));
          }
          continue;
        }
        if (in[pos] == '}') {
          ++pos;
          return Value(std::move(d));
        }
        return std::nullopt;
      }
    }

    std::optional<Value> ParseArray(size_t depth) {
      if (depth > max_depth) return std::nullopt;
      ++pos;  // [
      ListValue l;
      SkipWs();
      if (pos < in.size() && in[pos] == ']') {
        ++pos;
        return Value(std::move(l));
      }
      while (true) {
        std::optional<Value> v = ParseValue(depth);
        if (!v) return std::nullopt;
        l.Append(std::move(*v));
        SkipWs();
        if (pos >= in.size()) return std::nullopt;
        if (in[pos] == ',') {
          ++pos;
          SkipWs();
          if (trailing_commas() && pos < in.size() && in[pos] == ']') {
            ++pos;
            return Value(std::move(l));
          }
          continue;
        }
        if (in[pos] == ']') {
          ++pos;
          return Value(std::move(l));
        }
        return std::nullopt;
      }
    }

    static void AppendUtf8(uint32_t cp, std::string* out) {
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

    std::optional<uint32_t> Hex4() {
      if (pos + 4 > in.size()) return std::nullopt;
      uint32_t v = 0;
      for (int i = 0; i < 4; ++i) {
        const char c = in[pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') {
          v |= static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
          v |= static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
          v |= static_cast<uint32_t>(c - 'A' + 10);
        } else {
          return std::nullopt;
        }
      }
      return v;
    }

    // One strictly valid UTF-8 sequence starting at pos (lead byte >= 0x80).
    bool CopyUtf8(std::string* out) {
      const auto c = static_cast<unsigned char>(in[pos]);
      size_t n;
      uint32_t cp;
      if (c >= 0xC2 && c <= 0xDF) {
        n = 1;
        cp = c & 0x1F;
      } else if (c >= 0xE0 && c <= 0xEF) {
        n = 2;
        cp = c & 0x0F;
      } else if (c >= 0xF0 && c <= 0xF4) {
        n = 3;
        cp = c & 0x07;
      } else {
        return false;
      }
      if (pos + n >= in.size()) return false;
      for (size_t i = 1; i <= n; ++i) {
        const auto cc = static_cast<unsigned char>(in[pos + i]);
        if ((cc & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (cc & 0x3F);
      }
      if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
          cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return false;
      }
      out->append(in.substr(pos, n + 1));
      pos += n + 1;
      return true;
    }

    std::optional<std::string> ParseString() {
      ++pos;  // "
      std::string out;
      while (true) {
        if (pos >= in.size()) return std::nullopt;
        const auto c = static_cast<unsigned char>(in[pos]);
        if (c == '"') {
          ++pos;
          return out;
        }
        if (c < 0x20) return std::nullopt;
        if (c >= 0x80) {
          if (!CopyUtf8(&out)) return std::nullopt;
          continue;
        }
        if (c != '\\') {
          out.push_back(static_cast<char>(c));
          ++pos;
          continue;
        }
        ++pos;
        if (pos >= in.size()) return std::nullopt;
        const char e = in[pos++];
        switch (e) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            std::optional<uint32_t> u = Hex4();
            if (!u) return std::nullopt;
            uint32_t cp = *u;
            if (cp >= 0xDC00 && cp <= 0xDFFF) return std::nullopt;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (!Lit("\\u")) return std::nullopt;
              std::optional<uint32_t> lo = Hex4();
              if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return std::nullopt;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
            }
            AppendUtf8(cp, &out);
            break;
          }
          default:
            return std::nullopt;
        }
      }
    }

    static bool IsDigit(char c) { return c >= '0' && c <= '9'; }

    std::optional<Value> ParseNumber() {
      const size_t start = pos;
      bool integral = true;
      if (pos < in.size() && in[pos] == '-') ++pos;
      if (pos >= in.size()) return std::nullopt;
      if (in[pos] == '0') {
        ++pos;
      } else if (in[pos] >= '1' && in[pos] <= '9') {
        while (pos < in.size() && IsDigit(in[pos])) ++pos;
      } else {
        return std::nullopt;
      }
      if (pos < in.size() && in[pos] == '.') {
        integral = false;
        ++pos;
        if (pos >= in.size() || !IsDigit(in[pos])) return std::nullopt;
        while (pos < in.size() && IsDigit(in[pos])) ++pos;
      }
      if (pos < in.size() && (in[pos] == 'e' || in[pos] == 'E')) {
        integral = false;
        ++pos;
        if (pos < in.size() && (in[pos] == '+' || in[pos] == '-')) ++pos;
        if (pos >= in.size() || !IsDigit(in[pos])) return std::nullopt;
        while (pos < in.size() && IsDigit(in[pos])) ++pos;
      }
      const std::string text(in.substr(start, pos - start));
      if (integral) {
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && v >= std::numeric_limits<int>::min() &&
            v <= std::numeric_limits<int>::max()) {
          return Value(static_cast<int>(v));
        }
      }
      const double d = std::strtod(text.c_str(), nullptr);
      if (!std::isfinite(d)) return std::nullopt;
      return Value(d);
    }
  };
};

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_JSON_JSON_READER_H_
