// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/bounded_json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

std::optional<JsonValue> Parse(const std::string& text,
                               JsonError* error = nullptr) {
  return ParseJson(text, JsonLimits(), error);
}

TEST(BoundedJsonTest, ParsesValues) {
  std::optional<JsonValue> v = Parse(
      R"( {"a": [1, -2.5e+3, true, false, null], "b": {"c": "d"}, "e": ""} )");
  ASSERT_TRUE(v.has_value());
  ASSERT_TRUE(v->is_object());
  ASSERT_EQ(v->members.size(), 3u);
  EXPECT_EQ(v->members[0].first, "a");  // source order
  const JsonValue* a = v->Find("a");
  ASSERT_TRUE(a && a->is_array());
  ASSERT_EQ(a->items.size(), 5u);
  EXPECT_EQ(a->items[0].text, "1");
  EXPECT_EQ(a->items[1].text, "-2.5e+3");  // as written
  EXPECT_TRUE(a->items[2].is_bool() && a->items[2].boolean);
  EXPECT_TRUE(a->items[3].is_bool() && !a->items[3].boolean);
  EXPECT_TRUE(a->items[4].is_null());
  EXPECT_EQ(v->Find("b")->Find("c")->text, "d");
  EXPECT_EQ(v->Find("e")->text, "");
  EXPECT_EQ(v->Find("missing"), nullptr);
}

TEST(BoundedJsonTest, DecodesStrings) {
  std::optional<JsonValue> v =
      Parse(R"("a\"b\\c\/d\b\f\n\r\t\u0041\u00e9\u65e5\ud83d\ude00\u0000é")");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->text, std::string("a\"b\\c/d\b\f\n\r\tA\xC3\xA9\xE6\x97\xA5"
                                 "\xF0\x9F\x98\x80") +
                         std::string(1, '\0') + "\xC3\xA9");
}

TEST(BoundedJsonTest, RefusesWhatRfc8259Refuses) {
  for (const char* text : {
           "", " ", "{", "}", "[1,]", "{\"a\":1,}", "{,}", "[,1]", "{a:1}",
           "{'a':1}", "{\"a\" 1}", "{\"a\":}", "01", "1.", ".5", "-", "+1",
           "1e", "1e+", "0x1", "NaN", "Infinity", "-Infinity", "tru", "nul",
           "True", "\"abc", "\"\\x41\"", "\"\\u12\"", "\"\\uZZZZ\"",
           "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\t\"",
           "\"\n\"", "\"\x01\"", "[1] [2]", "1 2", "/*c*/1", "// c\n1",
           "\xEF\xBB\xBF" "1", "\"\xC0\x80\"", "\"\xE0\x80\x80\"",
           "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "\"\xF5\"", "\"\x80\"",
           "\"\xC3\"", "\"\xE6\x97\"", "[\"\xFF\"]", "\"a\" \"b\"",
       }) {
    JsonError error = JsonError::kNone;
    EXPECT_FALSE(Parse(text, &error).has_value()) << text;
    EXPECT_EQ(error, JsonError::kSyntax) << text;
  }
}

TEST(BoundedJsonTest, RefusesDuplicateKeys) {
  JsonError error;
  EXPECT_FALSE(Parse(R"({"a":1,"b":2,"a":3})", &error).has_value());
  EXPECT_EQ(error, JsonError::kDuplicateKey);
  // Same text after decoding counts as the same key.
  EXPECT_FALSE(Parse(R"({"a":1,"\u0061":2})", &error).has_value());
  EXPECT_EQ(error, JsonError::kDuplicateKey);
  EXPECT_TRUE(Parse(R"({"a":{"a":1},"b":{"a":2}})").has_value());
}

TEST(BoundedJsonTest, EnforcesLimits) {
  JsonLimits limits;
  limits.max_bytes = 10;
  limits.max_depth = 3;
  limits.max_values = 5;
  JsonError error;
  EXPECT_FALSE(ParseJson("[1,2,3,4,5]", limits, &error).has_value());
  EXPECT_EQ(error, JsonError::kTooLarge);  // 11 bytes
  EXPECT_TRUE(ParseJson("[[[1]]]", limits, &error).has_value());
  EXPECT_FALSE(ParseJson("[[[[1]]]]", limits, &error).has_value());
  EXPECT_EQ(error, JsonError::kTooDeep);
  EXPECT_FALSE(ParseJson("{\"a\":{\"b\":{\"c\":{}}}}", JsonLimits{100, 3, 100},
                         &error)
                   .has_value());
  EXPECT_EQ(error, JsonError::kTooDeep);
  EXPECT_TRUE(ParseJson("[1,2,3,4]", limits, &error).has_value());  // 5
  EXPECT_FALSE(ParseJson("[1,2,3,45]", JsonLimits{100, 3, 4}, &error)
                   .has_value());
  EXPECT_EQ(error, JsonError::kTooManyValues);
  // Deep input is refused without recursing that deep.
  const std::string deep = std::string(1000000, '[') + std::string(1000000, ']');
  EXPECT_FALSE(ParseJson(deep, JsonLimits{2000000, 64, 2000000}, &error)
                   .has_value());
  EXPECT_EQ(error, JsonError::kTooDeep);
}

TEST(BoundedJsonTest, WritesLikeJsonStringify) {
  std::optional<JsonValue> v = Parse(
      "{ \"k\" : [ 1 , -0.5e3 , true , null , \"\\u0001\\u001f\\b\\f\\n\\r\\t"
      "\\\"\\\\\\/\\u007f\\u2028é\" ] , \"o\" : { } , \"a\" : [ ] }");
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(WriteJson(*v),
            "{\"k\":[1,-0.5e3,true,null,\"\\u0001\\u001f\\b\\f\\n\\r\\t"
            "\\\"\\\\/\x7F\xE2\x80\xA8\xC3\xA9\"],\"o\":{},\"a\":[]}");
  EXPECT_EQ(JsonQuote(std::string("a\0b", 3)), "\"a\\u0000b\"");
}

// WriteJson(ParseJson(x)) is a fixed point, and parses back to the same
// value, for random structures.
TEST(BoundedJsonTest, RoundTripsRandomValues) {
  std::mt19937 rng(42);
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  const char* const kStrings[] = {"", "a", "é", "\\n", "\\u0000", "\\\"",
                                  "日本", "\\ud83d\\ude00", "x y"};
  const char* const kNumbers[] = {"0", "-1", "1.5", "2e10", "-3.25E-2"};
  std::function<std::string(int)> gen = [&](int depth) -> std::string {
    const int kind = depth > 4 ? pick(4) : pick(6);
    switch (kind) {
      case 0:
        return "null";
      case 1:
        return pick(2) ? "true" : "false";
      case 2:
        return kNumbers[pick(5)];
      case 3:
        return std::string("\"") + kStrings[pick(9)] + "\"";
      case 4: {
        std::string s = "[";
        const int n = pick(4);
        for (int i = 0; i < n; ++i) s += (i ? ", " : "") + gen(depth + 1);
        return s + "]";
      }
      default: {
        std::string s = "{";
        const int n = pick(4);
        for (int i = 0; i < n; ++i) {
          s += (i ? ", " : "") + std::string("\"k") + std::to_string(i) +
               "\": " + gen(depth + 1);
        }
        return s + "}";
      }
    }
  };
  for (int i = 0; i < 500; ++i) {
    const std::string text = gen(0);
    std::optional<JsonValue> v = Parse(text);
    ASSERT_TRUE(v.has_value()) << text;
    const std::string once = WriteJson(*v);
    std::optional<JsonValue> again = Parse(once);
    ASSERT_TRUE(again.has_value()) << once;
    EXPECT_EQ(WriteJson(*again), once);
  }
}

// Garbage never crashes the reader (and nothing it accepts fails to write).
TEST(BoundedJsonTest, SurvivesRandomBytes) {
  std::mt19937 rng(7);
  const std::string alphabet = "{}[]\",:0123456789-+.eEtrufalsn \\u\xC3\xA9\x80";
  for (int i = 0; i < 20000; ++i) {
    std::string text;
    const size_t n = rng() % 40;
    for (size_t k = 0; k < n; ++k) text += alphabet[rng() % alphabet.size()];
    std::optional<JsonValue> v = Parse(text);
    if (v) {
      EXPECT_TRUE(Parse(WriteJson(*v)).has_value()) << text;
    }
  }
}

}  // namespace
}  // namespace molt_ai
