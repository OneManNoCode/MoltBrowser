// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

// ---------------------------------------------------------------------------
// A small GBNF recognizer for what the converter emits (literals, classes,
// rule references, groups, ?, *, +, {m}, {m,}, {m,n}), so the tests can ask
// "does the grammar accept this text?" without llama.cpp. Whole-input
// matching from "root", over code points. Not linear; test inputs are short.
// ---------------------------------------------------------------------------
class Gbnf {
 public:
  static std::optional<Gbnf> Parse(const std::string& text) {
    Gbnf g;
    // Names first, so references can be resolved in one pass.
    size_t line_start = 0;
    std::vector<std::pair<std::string, std::string>> lines;
    while (line_start < text.size()) {
      size_t end = text.find('\n', line_start);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(line_start, end - line_start);
      line_start = end + 1;
      if (line.empty()) continue;
      const size_t sep = line.find(" ::= ");
      if (sep == std::string::npos) {
        // "name ::=" with an empty body is not emitted.
        return std::nullopt;
      }
      const std::string name = line.substr(0, sep);
      if (g.names_.count(name)) return std::nullopt;  // defined twice
      g.names_[name] = static_cast<int>(lines.size());
      lines.emplace_back(name, line.substr(sep + 5));
    }
    g.rules_.resize(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::u32string body = Decode(lines[i].second);
      size_t pos = 0;
      std::optional<Alts> alts = g.ParseAlts(body, &pos);
      if (!alts || pos != body.size()) return std::nullopt;
      g.rules_[i] = std::move(*alts);
    }
    if (!g.names_.count("root")) return std::nullopt;
    return g;
  }

  bool Matches(const std::string& utf8) const {
    input_ = Decode(utf8);
    memo_.clear();
    const std::set<size_t> ends = MatchRule(names_.at("root"), 0);
    return ends.count(input_.size()) > 0;
  }

 private:
  struct Item;
  using Seq = std::vector<Item>;
  using Alts = std::vector<Seq>;
  struct Item {
    enum Kind { kLiteral, kClass, kRef, kGroup } kind = kLiteral;
    std::u32string literal;
    bool negated = false;
    std::vector<std::pair<char32_t, char32_t>> ranges;
    int rule = -1;
    std::shared_ptr<Alts> group;
    int min = 1;
    int max = 1;  // -1: unbounded
  };

  static std::u32string Decode(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
      const auto b = static_cast<unsigned char>(s[i]);
      int n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
      char32_t cp = n == 1 ? b : (b & (0x7F >> n));
      for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
      }
      out.push_back(cp);
      i += static_cast<size_t>(n);
    }
    return out;
  }

  static void SkipSpaces(const std::u32string& s, size_t* pos) {
    while (*pos < s.size() && s[*pos] == ' ') ++*pos;
  }

  static bool Hex(const std::u32string& s, size_t* pos, int digits,
                  char32_t* out) {
    char32_t v = 0;
    for (int i = 0; i < digits; ++i) {
      if (*pos >= s.size()) return false;
      const char32_t c = s[(*pos)++];
      int d;
      if (c >= '0' && c <= '9') {
        d = static_cast<int>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<int>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<int>(c - 'A' + 10);
      } else {
        return false;
      }
      v = (v << 4) | static_cast<char32_t>(d);
    }
    *out = v;
    return true;
  }

  static bool Char(const std::u32string& s, size_t* pos, char32_t* out) {
    if (*pos >= s.size()) return false;
    char32_t c = s[(*pos)++];
    if (c != '\\') {
      *out = c;
      return true;
    }
    if (*pos >= s.size()) return false;
    c = s[(*pos)++];
    switch (c) {
      case 'n':
        *out = '\n';
        return true;
      case 't':
        *out = '\t';
        return true;
      case 'r':
        *out = '\r';
        return true;
      case 'x':
        return Hex(s, pos, 2, out);
      case 'u':
        return Hex(s, pos, 4, out);
      case 'U':
        return Hex(s, pos, 8, out);
      case '"':
      case '\\':
      case '[':
      case ']':
      case '-':
        *out = c;
        return true;
      default:
        return false;
    }
  }

  static bool IsNameChar(char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
  }

  std::optional<Alts> ParseAlts(const std::u32string& s, size_t* pos) {
    Alts alts;
    while (true) {
      std::optional<Seq> seq = ParseSeq(s, pos);
      if (!seq) return std::nullopt;
      alts.push_back(std::move(*seq));
      SkipSpaces(s, pos);
      if (*pos < s.size() && s[*pos] == '|') {
        ++*pos;
        continue;
      }
      return alts;
    }
  }

  std::optional<Seq> ParseSeq(const std::u32string& s, size_t* pos) {
    Seq seq;
    while (true) {
      SkipSpaces(s, pos);
      if (*pos >= s.size() || s[*pos] == '|' || s[*pos] == ')') return seq;
      Item item;
      const char32_t c = s[*pos];
      if (c == '"') {
        ++*pos;
        item.kind = Item::kLiteral;
        while (true) {
          if (*pos >= s.size()) return std::nullopt;
          if (s[*pos] == '"') {
            ++*pos;
            break;
          }
          char32_t ch;
          if (!Char(s, pos, &ch)) return std::nullopt;
          item.literal.push_back(ch);
        }
      } else if (c == '[') {
        ++*pos;
        item.kind = Item::kClass;
        if (*pos < s.size() && s[*pos] == '^') {
          item.negated = true;
          ++*pos;
        }
        while (true) {
          if (*pos >= s.size()) return std::nullopt;
          if (s[*pos] == ']') {
            ++*pos;
            break;
          }
          char32_t lo;
          if (!Char(s, pos, &lo)) return std::nullopt;
          char32_t hi = lo;
          if (*pos + 1 < s.size() && s[*pos] == '-' && s[*pos + 1] != ']') {
            ++*pos;
            if (!Char(s, pos, &hi)) return std::nullopt;
          }
          item.ranges.emplace_back(lo, hi);
        }
      } else if (c == '(') {
        ++*pos;
        item.kind = Item::kGroup;
        std::optional<Alts> alts = ParseAlts(s, pos);
        if (!alts || *pos >= s.size() || s[*pos] != ')') return std::nullopt;
        ++*pos;
        item.group = std::make_shared<Alts>(std::move(*alts));
      } else if (IsNameChar(c)) {
        std::string name;
        while (*pos < s.size() && IsNameChar(s[*pos])) {
          name.push_back(static_cast<char>(s[(*pos)++]));
        }
        auto it = names_.find(name);
        if (it == names_.end()) return std::nullopt;  // undefined rule
        item.kind = Item::kRef;
        item.rule = it->second;
      } else {
        return std::nullopt;
      }
      // Repetition.
      if (*pos < s.size()) {
        const char32_t r = s[*pos];
        if (r == '?') {
          item.min = 0;
          ++*pos;
        } else if (r == '*') {
          item.min = 0;
          item.max = -1;
          ++*pos;
        } else if (r == '+') {
          item.max = -1;
          ++*pos;
        } else if (r == '{') {
          ++*pos;
          auto number = [&](int* out) {
            int v = 0;
            bool any = false;
            while (*pos < s.size() && s[*pos] >= '0' && s[*pos] <= '9') {
              v = v * 10 + static_cast<int>(s[(*pos)++] - '0');
              any = true;
            }
            *out = v;
            return any;
          };
          if (!number(&item.min)) return std::nullopt;
          item.max = item.min;
          if (*pos < s.size() && s[*pos] == ',') {
            ++*pos;
            if (!number(&item.max)) item.max = -1;
          }
          if (*pos >= s.size() || s[*pos] != '}') return std::nullopt;
          ++*pos;
        }
      }
      seq.push_back(std::move(item));
    }
  }

  std::set<size_t> MatchRule(int rule, size_t pos) const {
    const auto key = std::make_pair(rule, pos);
    auto it = memo_.find(key);
    if (it != memo_.end()) return it->second;
    memo_[key] = {};  // a left-recursive re-entry matches nothing
    std::set<size_t> ends = MatchAlts(rules_[static_cast<size_t>(rule)], pos);
    memo_[key] = ends;
    return ends;
  }

  std::set<size_t> MatchAlts(const Alts& alts, size_t pos) const {
    std::set<size_t> out;
    for (const Seq& seq : alts) {
      std::set<size_t> cur = {pos};
      for (const Item& item : seq) {
        std::set<size_t> next;
        for (size_t p : cur) {
          for (size_t e : MatchRepeated(item, p)) next.insert(e);
        }
        cur = std::move(next);
        if (cur.empty()) break;
      }
      out.insert(cur.begin(), cur.end());
    }
    return out;
  }

  std::set<size_t> MatchRepeated(const Item& item, size_t pos) const {
    std::set<size_t> out;
    std::set<size_t> cur = {pos};
    std::set<size_t> seen = {pos};
    if (item.min == 0) out.insert(pos);
    for (int count = 1; item.max < 0 || count <= item.max; ++count) {
      std::set<size_t> next;
      for (size_t p : cur) {
        for (size_t e : MatchOnce(item, p)) next.insert(e);
      }
      if (item.max < 0) {
        // Unbounded: stop once nothing new is reached.
        std::set<size_t> fresh;
        for (size_t e : next) {
          if (seen.insert(e).second || count <= item.min) fresh.insert(e);
        }
        next = std::move(fresh);
      }
      if (next.empty()) break;
      if (count >= item.min) out.insert(next.begin(), next.end());
      cur = std::move(next);
    }
    return out;
  }

  std::set<size_t> MatchOnce(const Item& item, size_t pos) const {
    switch (item.kind) {
      case Item::kLiteral:
        if (input_.compare(pos, item.literal.size(), item.literal) == 0 &&
            pos + item.literal.size() <= input_.size()) {
          return {pos + item.literal.size()};
        }
        return {};
      case Item::kClass: {
        if (pos >= input_.size()) return {};
        bool in = false;
        for (const auto& [lo, hi] : item.ranges) {
          in = in || (input_[pos] >= lo && input_[pos] <= hi);
        }
        if (in != item.negated) return {pos + 1};
        return {};
      }
      case Item::kRef:
        return MatchRule(item.rule, pos);
      case Item::kGroup:
        return MatchAlts(*item.group, pos);
    }
    return {};
  }

  std::vector<Alts> rules_;
  std::map<std::string, int> names_;
  mutable std::u32string input_;
  mutable std::map<std::pair<int, size_t>, std::set<size_t>> memo_;
};

// ---------------------------------------------------------------------------

SchemaGrammarStatus StatusOf(const std::string& schema) {
  return JsonSchemaToGrammar(schema).status;
}

// The grammar for |schema|, checked for the converter's invariants: ASCII,
// one "name ::= body" per line, no body ending in '|' (llama.cpp would read
// the next line as part of it), parseable.
Gbnf GrammarOf(const std::string& schema) {
  const SchemaGrammar g = JsonSchemaToGrammar(schema);
  EXPECT_EQ(g.status, SchemaGrammarStatus::kOk) << schema << ": " << g.reason;
  EXPECT_TRUE(g.reason.empty());
  EXPECT_EQ(g.gbnf.rfind("root ::= ", 0), 0u) << g.gbnf;
  for (char c : g.gbnf) {
    EXPECT_TRUE(c == '\n' || (c >= 0x20 && c < 0x7F)) << g.gbnf;
  }
  size_t start = 0;
  while (start < g.gbnf.size()) {
    const size_t end = g.gbnf.find('\n', start);
    EXPECT_NE(end, std::string::npos);
    const std::string line = g.gbnf.substr(start, end - start);
    const size_t sep = line.find(" ::= ");
    EXPECT_NE(sep, std::string::npos) << line;
    EXPECT_GT(line.size(), sep + 5) << "empty rule: " << line;
    EXPECT_NE(line.back(), '|') << line;
    start = end + 1;
  }
  std::optional<Gbnf> parsed = Gbnf::Parse(g.gbnf);
  EXPECT_TRUE(parsed.has_value()) << g.gbnf;
  return parsed ? std::move(*parsed) : *Gbnf::Parse("root ::= \"\\x00\"\n");
}

void ExpectAccepts(const std::string& schema,
                   const std::vector<std::string>& yes,
                   const std::vector<std::string>& no) {
  const Gbnf g = GrammarOf(schema);
  for (const std::string& s : yes) {
    EXPECT_TRUE(g.Matches(s)) << schema << " should accept " << s;
  }
  for (const std::string& s : no) {
    EXPECT_FALSE(g.Matches(s)) << schema << " should refuse " << s;
  }
}

// ---- Values ----------------------------------------------------------------

TEST(JsonSchemaGrammarTest, Scalars) {
  ExpectAccepts(R"({"type":"boolean"})", {"true", "false"},
                {"True", "1", "\"true\"", " true", "true ", "null"});
  ExpectAccepts(R"({"type":"null"})", {"null"}, {"Null", "nil", ""});
  ExpectAccepts(R"({"type":"integer"})",
                {"0", "-0", "7", "-15", "1234567890123456"},
                {"01", "1.5", "+1", "12345678901234567", "1e3", "-", ""});
  ExpectAccepts(R"({"type":"number"})",
                {"0", "1.5", "-0.25e10", "1E+05", "2e-7", "0.1234567890123456"},
                {".5", "1.", "1e100", "1e", "--1", "0x10", "Infinity", "NaN",
                 "0.12345678901234567"});
}

TEST(JsonSchemaGrammarTest, Strings) {
  ExpectAccepts(
      R"({"type":"string"})",
      {"\"\"", "\"abc\"", "\"a b\"", "\"\\n\\t\\\"\\\\\\/\\b\\f\\r\"",
       "\"\\u00e9\"", "\"\\uFFFF\"", "\"\\ud7ff\"", "\"\\ue000\"",
       "\"é日本😀\"", "\"\x7E\""},
      {"\"", "abc", "\"a\nb\"", "\"\t\"", "\"\\x41\"", "\"\\u12\"",
       "\"\\ud800\"", "\"\\uDFFF\"", "\"\\uD83D\\uDE00\"", "\"\x7F\"",
       "\"a\" ", "'a'", "\"\\a\""});
  ExpectAccepts(R"({"type":"string","maxLength":3})",
                {"\"\"", "\"abc\"", "\"\\n\\nx\"", "\"日本語\""},
                {"\"abcd\"", "\"\\n\\n\\n\\n\""});
  ExpectAccepts(R"({"type":"string","minLength":2,"maxLength":2})",
                {"\"ab\"", "\"\\u0041b\""}, {"\"a\"", "\"abc\""});
  ExpectAccepts(R"({"type":"string","minLength":2})",
                {"\"ab\"", "\"abcdefghij\""}, {"\"a\"", "\"\""});
  ExpectAccepts(R"({"maxLength":1})", {"\"a\""}, {"\"ab\"", "1"});
}

TEST(JsonSchemaGrammarTest, IntegerBounds) {
  ExpectAccepts(R"({"type":"integer","minimum":1,"maximum":5})",
                {"1", "2", "3", "4", "5"}, {"0", "6", "05", "-1", "1.0"});
  ExpectAccepts(R"({"type":"integer","minimum":-3,"maximum":12})",
                {"-3", "-1", "0", "9", "10", "12"},
                {"-4", "13", "-0", "011", "1 "});
  ExpectAccepts(R"({"type":"integer","exclusiveMinimum":0,"exclusiveMaximum":3})",
                {"1", "2"}, {"0", "3"});
  ExpectAccepts(R"({"type":"integer","minimum":0})", {"0", "7", "10", "990"},
                {"-1", "01"});
  ExpectAccepts(R"({"type":"integer","minimum":1})", {"1", "10"},
                {"0", "-1"});
  ExpectAccepts(R"({"type":"integer","exclusiveMinimum":0})", {"1"}, {"0"});
  ExpectAccepts(R"({"type":"integer","minimum":2,"maximum":2})", {"2"},
                {"1", "3", "22"});
  // "1" both ends a value and starts "10".
  ExpectAccepts(R"({"type":"integer","minimum":1,"maximum":10})",
                {"1", "5", "10"}, {"0", "11", "100"});
  // 1.0 is an integer bound (JSON.stringify would write 1).
  ExpectAccepts(R"({"type":"integer","minimum":1.0,"maximum":3})", {"1", "3"},
                {"0", "4"});
  ExpectAccepts(R"({"type":"integer","minimum":0,"maximum":1000})",
                {"0", "1", "999", "1000"}, {"1001", "-1", "00"});
  // Both kinds of bound: the tighter one wins.
  ExpectAccepts(
      R"({"type":"integer","minimum":0,"exclusiveMinimum":2,"maximum":9,"exclusiveMaximum":5})",
      {"3", "4"}, {"2", "5", "0", "9"});
}

TEST(JsonSchemaGrammarTest, NumberBounds) {
  ExpectAccepts(R"({"type":"number","minimum":0,"maximum":1})",
                {"0", "0.5", "0.0001", "1", "1.0", "1.000", "0.999"},
                {"1.5", "1.01", "2", "-0.1", "0.5e1", "01", "-0"});
  ExpectAccepts(R"({"type":"number","minimum":0,"maximum":10})",
                {"0", "3", "9.99", "10", "10.0"},
                {"10.5", "11", "100", "-1"});
  ExpectAccepts(R"({"type":"number","minimum":3,"maximum":3})", {"3", "3.00"},
                {"3.5", "2"});
  ExpectAccepts(R"({"type":"number","minimum":0})", {"0", "0.5", "3e2"},
                {"-1", "-0.5"});
  ExpectAccepts(R"({"minimum":0,"maximum":2})", {"1.5"}, {"2.5", "\"1\""});
}

TEST(JsonSchemaGrammarTest, EnumAndConst) {
  ExpectAccepts(
      R"({"enum":["a","ab",1,null,true,{"x":[1,2]},[],"é","\u0001"]})",
      {"\"a\"", "\"ab\"", "1", "null", "true", "{\"x\":[1,2]}", "[]", "\"é\"",
       "\"\\u0001\""},
      {"\"b\"", "\"abc\"", "\"\"", "10", "false", "{\"x\": [1,2]}", "[ ]",
       "\"\\u00e9\"", "\"\x01\""});
  ExpectAccepts(R"({"const":"x"})", {"\"x\""}, {"\"y\"", "x"});
  ExpectAccepts(R"({"const":{"a":[true,null]}})", {"{\"a\":[true,null]}"},
                {"{\"a\":[true]}"});
  // "type" filters the values.
  ExpectAccepts(R"({"type":"string","enum":["a",1,"b"]})", {"\"a\"", "\"b\""},
                {"1"});
  ExpectAccepts(R"({"type":"integer","enum":[1,1.5,2.0,"3"]})", {"1", "2.0"},
                {"1.5", "\"3\""});
  ExpectAccepts(R"({"enum":["x","y"],"const":"y"})", {"\"y\""}, {"\"x\""});
  // Duplicates are fine.
  ExpectAccepts(R"({"enum":["a","a"]})", {"\"a\""}, {"\"aa\""});
  // Numbers as written; one may be a prefix of another.
  ExpectAccepts(R"({"enum":[1e3,-0.5,1,12]})", {"1e3", "-0.5", "1", "12"},
                {"1000", "121", "2"});
}

TEST(JsonSchemaGrammarTest, Whitespace) {
  const std::string schema =
      R"({"type":"object","properties":{"a":{"type":"integer"}},"required":["a"]})";
  ExpectAccepts(schema,
                {"{\"a\":1}", "{ \"a\" : 1 }", "{\n  \"a\":\n\t1\n}",
                 "{\n" + std::string(20, ' ') + "\"a\":1}"},
                {"{\"a\":1} ", " {\"a\":1}", "{  \"a\":1}",
                 "{\n" + std::string(21, ' ') + "\"a\":1}", "{\n\n\"a\":1}",
                 "{\r\"a\":1}"});
}

// ---- Objects ---------------------------------------------------------------

TEST(JsonSchemaGrammarTest, ObjectProperties) {
  const std::string schema = R"({"type":"object","properties":{
      "a":{"type":"integer"},"b":{"type":"boolean"},"c":{"type":"null"}},
      "required":["a","c"],"additionalProperties":false})";
  ExpectAccepts(schema,
                {"{\"a\":1,\"c\":null}", "{\"a\":1,\"b\":true,\"c\":null}",
                 "{ \"a\" : 1 , \"c\" : null }"},
                {"{\"c\":null,\"a\":1}", "{\"a\":1}", "{\"c\":null}", "{}",
                 "{\"a\":1,\"c\":null,\"d\":2}", "{\"a\":1,,\"c\":null}",
                 "{\"a\":1,\"c\":null,}", "{\"a\":\"1\",\"c\":null}",
                 "{\"a\":1 \"c\":null}"});
  // All optional, in order, any subset.
  ExpectAccepts(R"({"properties":{"x":{"type":"null"},"y":{"type":"null"},"z":{"type":"null"}}})",
                {"{}", "{\"x\":null}", "{\"y\":null}", "{\"z\":null}",
                 "{\"x\":null,\"z\":null}", "{\"x\":null,\"y\":null,\"z\":null}"},
                {"{\"z\":null,\"x\":null}", "{\"x\":null,\"x\":null}",
                 "{,\"x\":null}", "{\"w\":null}"});
  // Required last, optional before it.
  ExpectAccepts(R"({"properties":{"x":{"type":"null"},"y":{"type":"null"}},"required":["y"]})",
                {"{\"y\":null}", "{\"x\":null,\"y\":null}"},
                {"{\"x\":null}", "{}"});
  // additionalProperties true or absent: no other keys are generated.
  ExpectAccepts(R"({"properties":{"x":{"type":"null"}},"additionalProperties":true})",
                {"{}", "{\"x\":null}"}, {"{\"q\":null}"});
}

TEST(JsonSchemaGrammarTest, PropertyNamesAreJsonEscaped) {
  ExpectAccepts(
      R"({"properties":{"é \"q\" \\ \n":{"type":"null"}},"required":["é \"q\" \\ \n"]})",
      {"{\"é \\\"q\\\" \\\\ \\n\":null}"},
      {"{\"é \"q\" \\ \n\":null}", "{\"\\u00e9 \\\"q\\\" \\\\ \\n\":null}"});
}

TEST(JsonSchemaGrammarTest, ObjectsWithoutProperties) {
  ExpectAccepts(R"({"type":"object","additionalProperties":false})",
                {"{}", "{ }"}, {"{\"a\":1}"});
  ExpectAccepts(R"({"type":"object","additionalProperties":{"type":"integer"}})",
                {"{}", "{\"x\":1}", "{\"x\":1,\"y\":-2}", "{\"\":0}",
                 "{\"a\":1, \"b\":2,\n \"c\":3}"},
                {"{\"x\":\"s\"}", "{x:1}", "{\"x\":1,}"});
  ExpectAccepts(R"({"type":"object"})",
                {"{}", "{\"a\":[1,{\"b\":null}]}", "{\"k\":\"v\",\"n\":1.5}"},
                {"[]", "{\"a\":{\"b\":{\"c\":{}}}}"});
}

TEST(JsonSchemaGrammarTest, AnyValueIsBoundedInDepth) {
  for (const char* schema : {"{}", "true", R"({"title":"x"})"}) {
    ExpectAccepts(schema,
                  {"1", "\"s\"", "null", "true", "[]", "{}", "[[[1]]]",
                   "{\"a\":{\"b\":[1]}}", "[1, \"x\", {\"k\": null}]"},
                  {"[[[[1]]]]", "{\"a\":{\"b\":{\"c\":[]}}}", "", "nul"});
  }
}

// ---- Arrays ----------------------------------------------------------------

TEST(JsonSchemaGrammarTest, Arrays) {
  ExpectAccepts(R"({"type":"array","items":{"type":"integer"}})",
                {"[]", "[1]", "[1,2,3]", "[ 1 , 2 ]", "[\n 1,\n 2\n]"},
                {"[1,]", "[,1]", "[\"a\"]", "[1 2]", "1"});
  ExpectAccepts(
      R"({"type":"array","items":{"type":"integer"},"minItems":1,"maxItems":3})",
      {"[1]", "[1,2]", "[1,2,3]"}, {"[]", "[1,2,3,4]"});
  ExpectAccepts(R"({"type":"array","items":{"type":"null"},"minItems":2})",
                {"[null,null]", "[null,null,null,null]"}, {"[null]", "[]"});
  ExpectAccepts(R"({"type":"array","maxItems":0})", {"[]"}, {"[1]"});
  ExpectAccepts(R"({"type":"array","items":false})", {"[]"}, {"[1]"});
  ExpectAccepts(R"({"type":"array","minItems":2,"maxItems":2})",
                {"[1,\"a\"]", "[[],{}]"}, {"[1]", "[1,2,3]"});
  ExpectAccepts(R"({"type":"array","uniqueItems":false})", {"[1,1]"}, {});
  ExpectAccepts(R"({"maxItems":1})", {"[]", "[1]"}, {"[1,2]", "{}"});
}

// ---- Combinations ------------------------------------------------------------

TEST(JsonSchemaGrammarTest, AnyOfAndTypeLists) {
  ExpectAccepts(R"({"anyOf":[{"type":"string","maxLength":2},{"type":"null"}]})",
                {"\"ab\"", "null"}, {"\"abc\"", "1"});
  ExpectAccepts(R"({"oneOf":[{"type":"integer"},{"const":"x"}]})",
                {"1", "\"x\""}, {"\"y\""});
  ExpectAccepts(R"({"type":["string","null"],"maxLength":1})",
                {"\"a\"", "null"}, {"\"ab\"", "1"});
  ExpectAccepts(R"({"type":["integer","number"]})", {"1", "1.5"}, {"\"1\""});
  ExpectAccepts(
      R"({"anyOf":[{"type":"object","properties":{"k":{"const":"a"},"n":{"type":"integer"}},"required":["k","n"]},
                   {"type":"object","properties":{"k":{"const":"b"}},"required":["k"]}]})",
      {"{\"k\":\"a\",\"n\":1}", "{\"k\":\"b\"}"},
      {"{\"k\":\"a\"}", "{\"k\":\"b\",\"n\":1}", "{\"k\":\"c\"}"});
  // A false branch is dropped.
  ExpectAccepts(R"({"anyOf":[false,{"type":"null"}]})", {"null"}, {"1"});
  // Nested anyOf.
  ExpectAccepts(R"({"anyOf":[{"anyOf":[{"const":1},{"const":2}]},{"const":3}]})",
                {"1", "2", "3"}, {"4"});
}

TEST(JsonSchemaGrammarTest, AnnotationsAreIgnored) {
  ExpectAccepts(
      R"({"$schema":"http://json-schema.org/draft-07/schema#","$id":"x","title":"t",
          "description":"d","default":1,"examples":[1],"$comment":"c","deprecated":false,
          "readOnly":true,"writeOnly":false,"$defs":{"a":{"$ref":"#"}},"definitions":{},
          "type":"integer","minimum":1,"maximum":2})",
      {"1", "2"}, {"3"});
}

TEST(JsonSchemaGrammarTest, NestedRealisticSchema) {
  const std::string schema = R"({
    "type": "object",
    "properties": {
      "title": {"type": "string", "maxLength": 20},
      "rating": {"type": "integer", "minimum": 1, "maximum": 5},
      "sentiment": {"enum": ["positive", "neutral", "negative"]},
      "tags": {"type": "array", "items": {"type": "string", "maxLength": 10}, "maxItems": 3},
      "author": {"type": ["object", "null"], "properties": {"name": {"type": "string"}},
                 "required": ["name"], "additionalProperties": false},
      "score": {"type": "number", "minimum": 0, "maximum": 1}
    },
    "required": ["title", "rating", "sentiment"],
    "additionalProperties": false
  })";
  ExpectAccepts(
      schema,
      {R"({"title":"Good","rating":4,"sentiment":"positive"})",
       R"({"title":"x","rating":1,"sentiment":"neutral","tags":["a","b"],"author":null,"score":0.5})",
       R"({"title":"x","rating":5,"sentiment":"negative","author":{"name":"Ann"}})"},
      {R"({"title":"x","rating":6,"sentiment":"positive"})",
       R"({"title":"x","rating":1,"sentiment":"happy"})",
       R"({"title":"x","rating":1,"sentiment":"neutral","tags":["a","b","c","d"]})",
       R"({"title":"x","rating":1,"sentiment":"neutral","author":{}})",
       R"({"title":"x","rating":1,"sentiment":"neutral","score":2})"});
}

TEST(JsonSchemaGrammarTest, SameInputSameGrammar) {
  const std::string schema =
      R"({"properties":{"a":{"enum":["x","y"]},"b":{"type":"string","maxLength":4}}})";
  EXPECT_EQ(JsonSchemaToGrammar(schema).gbnf, JsonSchemaToGrammar(schema).gbnf);
}

TEST(JsonSchemaGrammarTest, IdenticalSubschemasShareRules) {
  std::string schema = R"({"properties":{)";
  for (int i = 0; i < 40; ++i) {
    if (i) schema += ",";
    schema += "\"p" + std::to_string(i) +
              R"(":{"type":"integer","minimum":0,"maximum":1000})";
  }
  schema += "}}";
  const SchemaGrammar g = JsonSchemaToGrammar(schema);
  ASSERT_EQ(g.status, SchemaGrammarStatus::kOk) << g.reason;
  // One copy of the 0..1000 range, not forty.
  EXPECT_LT(g.gbnf.size(), 12000u);
}

// ---- Refusals ----------------------------------------------------------------

TEST(JsonSchemaGrammarTest, NotJson) {
  for (const char* schema :
       {"", "{", "{\"type\":\"string\",}", "{'type':'string'}", "[]", "\"x\"",
        "1", "null", "{\"type\":\"string\"} x", "\xEF\xBB\xBF{}",
        "{\"a\":1,\"a\":2}", "{\"type\":\"str\xC3\x28ing\"}",
        "{\"type\":\"string\"/*c*/}", "{\"x\":\"\\ud800\"}",
        "{\"x\":\"a\tb\"}", "{\"x\":01}", "{\"x\":1.}", "{\"x\":NaN}"}) {
    EXPECT_EQ(StatusOf(schema), SchemaGrammarStatus::kInvalidJson) << schema;
  }
  EXPECT_EQ(StatusOf(R"({"properties":{"a":1}})"),
            SchemaGrammarStatus::kInvalidJson);
}

TEST(JsonSchemaGrammarTest, UnsupportedKeywordsAreNamed) {
  struct Case {
    const char* schema;
    const char* reason;
  };
  const Case cases[] = {
      {R"({"$ref":"#/$defs/a"})", "\"$ref\""},
      {R"({"type":"string","pattern":"^a+$"})", "\"pattern\""},
      {R"({"type":"string","format":"email"})", "\"format\""},
      {R"({"allOf":[{"type":"string"}]})", "\"allOf\""},
      {R"({"not":{"type":"string"}})", "\"not\""},
      {R"({"if":{},"then":{},"else":{}})", "\"if\""},
      {R"({"type":"array","prefixItems":[{}]})", "\"prefixItems\""},
      {R"({"type":"object","patternProperties":{"a":{}}})",
       "\"patternProperties\""},
      {R"({"type":"number","multipleOf":2})", "\"multipleOf\""},
      {R"({"type":"array","uniqueItems":true})", "\"uniqueItems\""},
      {R"({"type":"object","minProperties":1})", "\"minProperties\""},
      {R"({"type":"string","nullable":true})", "\"nullable\""},
  };
  for (const Case& c : cases) {
    const SchemaGrammar g = JsonSchemaToGrammar(c.schema);
    EXPECT_EQ(g.status, SchemaGrammarStatus::kUnsupported) << c.schema;
    EXPECT_NE(g.reason.find(c.reason), std::string::npos)
        << c.schema << ": " << g.reason;
    EXPECT_TRUE(g.gbnf.empty());
  }
  // An unknown keyword is refused without echoing its (page-chosen) name.
  const SchemaGrammar g =
      JsonSchemaToGrammar(R"({"type":"string","secretPageText":1})");
  EXPECT_EQ(g.status, SchemaGrammarStatus::kUnsupported);
  EXPECT_EQ(g.reason.find("secretPageText"), std::string::npos);
  // Deep inside, too.
  EXPECT_EQ(StatusOf(R"({"properties":{"a":{"items":{"anyOf":[{"$ref":"#"}]}}}})"),
            SchemaGrammarStatus::kUnsupported);
}

TEST(JsonSchemaGrammarTest, UnsupportedForms) {
  for (const char* schema : {
           R"({"type":"array","items":[{"type":"string"}]})",
           R"({"properties":{"a":{}},"additionalProperties":{"type":"string"}})",
           R"({"properties":{"a":{}},"required":["b"]})",
           R"({"required":["a"]})",
           R"({"required":"a","properties":{"a":{}}})",
           R"({"required":["a","a"],"properties":{"a":{}}})",
           R"({"anyOf":[{"type":"string"}],"type":"string"})",
           R"({"anyOf":[{}],"oneOf":[{}]})",
           R"({"anyOf":[]})",
           R"({"anyOf":{}})",
           R"({"enum":["a"],"minLength":1})",
           R"({"enum":"a"})",
           R"({"type":"date"})",
           R"({"type":["string","string"]})",
           R"({"type":[]})",
           R"({"type":7})",
           R"({"properties":{},"items":{}})",
           R"({"minLength":1,"minimum":1})",
           R"({"type":"string","maxLength":1025})",
           R"({"type":"string","maxLength":-1})",
           R"({"type":"string","maxLength":1.5})",
           R"({"type":"string","maxLength":"3"})",
           R"({"type":"array","maxItems":65})",
           R"({"type":"array","minItems":65})",
           R"({"type":"integer","maximum":5})",
           R"({"type":"integer","minimum":2})",
           R"({"type":"integer","minimum":0,"maximum":1001})",
           R"({"type":"integer","minimum":0.5,"maximum":3})",
           R"({"type":"integer","minimum":0,"exclusiveMinimum":true})",
           R"({"type":"integer","minimum":0,"maximum":1e3})",
           R"({"type":"integer","minimum":-10000000000000000,"maximum":0})",
           R"({"type":"number","minimum":-1,"maximum":1})",
           R"({"type":"number","minimum":0,"maximum":0.5})",
           R"({"type":"number","exclusiveMinimum":0})",
           R"({"type":"number","minimum":1})",
           R"({"type":"number","maximum":1})",
           R"({"type":"number","minimum":0,"maximum":2000})",
           R"({"properties":[]})",
           R"({"additionalProperties":1})",
           R"({"type":"array","uniqueItems":"no"})",
       }) {
    EXPECT_EQ(StatusOf(schema), SchemaGrammarStatus::kUnsupported) << schema;
  }
}

TEST(JsonSchemaGrammarTest, Unsatisfiable) {
  for (const char* schema : {
           "false",
           R"({"enum":[]})",
           R"({"type":"string","enum":[1,2]})",
           R"({"enum":["a"],"const":"b"})",
           R"({"type":"integer","const":1.5})",
           R"({"type":"array","minItems":3,"maxItems":2})",
           R"({"type":"string","minLength":3,"maxLength":2})",
           R"({"type":"integer","minimum":3,"maximum":2})",
           R"({"type":"integer","exclusiveMinimum":2,"exclusiveMaximum":3})",
           R"({"type":"array","items":false,"minItems":1})",
           R"({"properties":{"a":false},"required":["a"]})",
           R"({"anyOf":[false,false]})",
       }) {
    EXPECT_EQ(StatusOf(schema), SchemaGrammarStatus::kUnsatisfiable) << schema;
  }
  // An optional property that matches nothing is just never generated.
  ExpectAccepts(R"({"properties":{"a":false,"b":{"type":"null"}}})",
                {"{}", "{\"b\":null}"}, {"{\"a\":null}"});
}

// ---- Hostile input -------------------------------------------------------------

std::string Nest(const std::string& open, const std::string& inner,
                 const std::string& close, int depth) {
  std::string s;
  for (int i = 0; i < depth; ++i) s += open;
  s += inner;
  for (int i = 0; i < depth; ++i) s += close;
  return s;
}

TEST(JsonSchemaGrammarTest, DeepNestingIsRefused) {
  // Schema nesting just within the cap converts...
  const int ok_depth = static_cast<int>(kMaxSchemaDepth);
  EXPECT_EQ(StatusOf(Nest(R"({"items":)", "{}", "}", ok_depth)),
            SchemaGrammarStatus::kOk);
  EXPECT_EQ(StatusOf(Nest(R"({"items":)", "{}", "}", ok_depth + 1)),
            SchemaGrammarStatus::kTooDeep);
  EXPECT_EQ(StatusOf(Nest(R"({"anyOf":[)", "{}", "]}", ok_depth + 1)),
            SchemaGrammarStatus::kTooDeep);
  EXPECT_EQ(StatusOf(Nest(R"({"properties":{"a":)", "{}", "}}", 40)),
            SchemaGrammarStatus::kTooDeep);
  // ... and JSON nesting far beyond it is refused by the reader, before
  // any recursion that deep (no stack overflow on 100k levels).
  EXPECT_EQ(StatusOf(Nest("[", "", "]", 10000)), SchemaGrammarStatus::kTooDeep);
  EXPECT_EQ(StatusOf(Nest(R"({"a":)", "1", "}", 5000)),
            SchemaGrammarStatus::kTooDeep);
  // Deep inside an annotation counts too (the reader caps all nesting).
  EXPECT_EQ(StatusOf("{\"default\":" + Nest("[", "", "]", 100) + "}"),
            SchemaGrammarStatus::kTooDeep);
}

TEST(JsonSchemaGrammarTest, HugeInputIsRefused) {
  std::string big = R"({"description":")" + std::string(kMaxSchemaBytes, 'a') +
                    R"("})";
  EXPECT_EQ(StatusOf(big), SchemaGrammarStatus::kTooLarge);
  // Many values within the byte cap.
  std::string many = R"({"default":[)";
  for (size_t i = 0; i < kMaxSchemaValues; ++i) many += i ? ",0" : "0";
  many += "]}";
  ASSERT_LE(many.size(), kMaxSchemaBytes);
  EXPECT_EQ(StatusOf(many), SchemaGrammarStatus::kTooLarge);
}

TEST(JsonSchemaGrammarTest, HugeEnumsAreRefused) {
  std::string schema = R"({"enum":[)";
  for (size_t i = 0; i <= kMaxEnumValues; ++i) {
    if (i) schema += ",";
    schema += std::to_string(i);
  }
  schema += "]}";
  EXPECT_EQ(StatusOf(schema), SchemaGrammarStatus::kTooLarge);
  // The largest accepted enum: one value per alternative, as a trie.
  schema = R"({"enum":[)";
  for (size_t i = 0; i < kMaxEnumValues; ++i) {
    if (i) schema += ",";
    schema += "\"value_" + std::to_string(i) + "\"";
  }
  schema += "]}";
  const Gbnf g = GrammarOf(schema);
  EXPECT_TRUE(g.Matches("\"value_0\""));
  EXPECT_TRUE(g.Matches("\"value_255\""));
  EXPECT_FALSE(g.Matches("\"value_256\""));
  EXPECT_FALSE(g.Matches("\"value_\""));
  // A single value too long.
  EXPECT_EQ(StatusOf(R"({"const":")" + std::string(kMaxEnumValueBytes, 'x') +
                     R"("})"),
            SchemaGrammarStatus::kTooLarge);
}

TEST(JsonSchemaGrammarTest, WideSchemasAreRefused) {
  std::string props = R"({"properties":{)";
  for (size_t i = 0; i <= kMaxProperties; ++i) {
    if (i) props += ",";
    props += "\"p" + std::to_string(i) + "\":{}";
  }
  props += "}}";
  EXPECT_EQ(StatusOf(props), SchemaGrammarStatus::kTooLarge);
  std::string any_of = R"({"anyOf":[)";
  for (size_t i = 0; i <= kMaxAnyOf; ++i) {
    if (i) any_of += ",";
    any_of += "{\"const\":" + std::to_string(i) + "}";
  }
  any_of += "]}";
  EXPECT_EQ(StatusOf(any_of), SchemaGrammarStatus::kTooLarge);
}

// Alternatives that stay alive together cost a pass over the vocabulary
// each per token: their number is capped, even when built up by nesting
// (anyOf within anyOf, the "cycles" a schema without $ref can express).
TEST(JsonSchemaGrammarTest, AmbiguityIsCapped) {
  // 16 strings: fine. 16 x 8 overlapping strings nested: refused.
  std::string strings = R"({"anyOf":[)";
  for (int i = 0; i < 16; ++i) {
    if (i) strings += ",";
    strings += R"({"type":"string","maxLength":)" + std::to_string(i + 1) + "}";
  }
  strings += "]}";
  EXPECT_EQ(StatusOf(strings), SchemaGrammarStatus::kOk);
  std::string nested = R"({"anyOf":[)";
  for (int i = 0; i < 16; ++i) {
    if (i) nested += ",";
    nested += R"({"type":"array","maxItems":)" + std::to_string(i + 1) +
              R"(,"items":{"anyOf":[)";
    for (int j = 0; j < 8; ++j) {
      if (j) nested += ",";
      nested += R"({"type":"string","minLength":)" + std::to_string(j) + "}";
    }
    nested += "]}}";
  }
  nested += "]}";
  EXPECT_EQ(StatusOf(nested), SchemaGrammarStatus::kTooAmbiguous);
  // Exactly at the cap: 4 x 16 overlapping strings.
  auto strings_from = [](int first) {
    std::string s = R"({"anyOf":[)";
    for (int i = 0; i < 16; ++i) {
      if (i) s += ",";
      s += R"({"type":"string","maxLength":)" + std::to_string(first + i) + "}";
    }
    return s + "]}";
  };
  const std::string at_cap = R"({"anyOf":[)" + strings_from(1) + "," +
                             strings_from(17) + "," + strings_from(33) + "," +
                             strings_from(49) + "]}";
  EXPECT_EQ(StatusOf(at_cap), SchemaGrammarStatus::kOk);
  const std::string over_cap = R"({"anyOf":[)" + strings_from(1) + "," +
                               strings_from(17) + "," + strings_from(33) +
                               "," + strings_from(49) +
                               R"(,{"type":"string","minLength":2}]})";
  EXPECT_EQ(StatusOf(over_cap), SchemaGrammarStatus::kTooAmbiguous);
  // Different kinds don't overlap: a union of every type is fine.
  EXPECT_EQ(StatusOf(R"({"type":["object","array","string","number","boolean","null"]})"),
            SchemaGrammarStatus::kOk);
  // Optional keys alive together count too: two wide objects in a union.
  auto wide = [](const char* prefix) {
    std::string s = R"({"type":"object","properties":{)";
    for (size_t i = 0; i < kMaxProperties; ++i) {
      if (i) s += ",";
      s += "\"" + std::string(prefix) + std::to_string(i) + "\":{}";
    }
    return s + "}}";
  };
  EXPECT_EQ(StatusOf(wide("k")), SchemaGrammarStatus::kOk);
  EXPECT_EQ(StatusOf(R"({"anyOf":[)" + wide("k") + "," + wide("j") + "]}"),
            SchemaGrammarStatus::kTooAmbiguous);
}

TEST(JsonSchemaGrammarTest, OutputSizeIsCapped) {
  // Wide integer ranges compress well: tries share their sub-rules.
  std::string ranges = R"({"properties":{)";
  for (int i = 0; i < 60; ++i) {
    if (i) ranges += ",";
    ranges += "\"p" + std::to_string(i) + R"(":{"type":"integer","minimum":)" +
              std::to_string(i * 2000 + 7) + R"(,"maximum":)" +
              std::to_string(i * 2000 + 1007) + "}";
  }
  ranges += "}}";
  const SchemaGrammar g = JsonSchemaToGrammar(ranges);
  ASSERT_EQ(g.status, SchemaGrammarStatus::kOk) << g.reason;
  EXPECT_LT(g.gbnf.size(), 64000u);
  // Many distinct properties: three rules each, over kMaxGrammarRules.
  std::string objects = R"({"properties":{)";
  for (int o = 0; o < 16; ++o) {
    if (o) objects += ",";
    objects += "\"o" + std::to_string(o) + R"(":{"properties":{)";
    for (int i = 0; i < 64; ++i) {
      if (i) objects += ",";
      objects += "\"o" + std::to_string(o) + "p" + std::to_string(i) + "\":{}";
    }
    objects += "}}";
  }
  objects += "}}";
  ASSERT_LT(objects.size(), kMaxSchemaBytes);
  const SchemaGrammar big = JsonSchemaToGrammar(objects);
  EXPECT_EQ(big.status, SchemaGrammarStatus::kTooLarge);
  EXPECT_EQ(big.reason, "grammar too large");
  // Counted repetitions: llama.cpp expands each into a rule.
  std::string lengths = R"({"properties":{)";
  for (int i = 0; i < 20; ++i) {
    if (i) lengths += ",";
    lengths += "\"p" + std::to_string(i) +
               R"(":{"type":"string","maxLength":)" + std::to_string(1000 + i) +
               "}";
  }
  lengths += "}}";
  EXPECT_EQ(StatusOf(lengths), SchemaGrammarStatus::kTooLarge);
}

// Linear time: the largest inputs convert quickly.
TEST(JsonSchemaGrammarTest, LargeInputsConvertQuickly) {
  std::string schema = R"({"description":")" +
                       std::string(kMaxSchemaBytes - 64, 'x') + R"("})";
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(StatusOf(schema), SchemaGrammarStatus::kOk);
  }
  // Escapes in a value: written back as escapes, just within the cap.
  std::string escapes = R"({"const":")";
  while (escapes.size() < kMaxEnumValueBytes - 16) escapes += "\\u0001";
  escapes += "\"}";
  EXPECT_EQ(StatusOf(escapes), SchemaGrammarStatus::kOk);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  EXPECT_LT(ms, 2000.0);
}

}  // namespace
}  // namespace molt_ai
