// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/grammar_tokens.h"

#include <random>
#include <string>

#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

GrammarToken Classify(std::string_view piece,
                      bool control = false,
                      bool user_defined = false,
                      bool eog = false,
                      std::string_view text = {}) {
  GrammarTokenFacts f;
  f.end_of_generation = eog;
  f.control = control;
  f.user_defined = user_defined;
  f.text = text.empty() ? piece : text;
  f.piece = piece;
  return ClassifyGrammarToken(f);
}

TEST(GrammarTokensTest, Classify) {
  EXPECT_EQ(Classify("{"), GrammarToken::kAllowed);
  EXPECT_EQ(Classify("\"name\":"), GrammarToken::kAllowed);
  EXPECT_EQ(Classify("\xE6\x97"), GrammarToken::kAllowed);  // partial: later
  // End of generation: the grammar decides, whatever else it is.
  EXPECT_EQ(Classify("<|im_end|>", /*control=*/true, false, /*eog=*/true),
            GrammarToken::kEndOfGeneration);
  EXPECT_EQ(Classify("", false, false, /*eog=*/true),
            GrammarToken::kEndOfGeneration);
  // Control / unknown / unused: never.
  EXPECT_EQ(Classify("<|im_start|>", /*control=*/true), GrammarToken::kBanned);
  EXPECT_EQ(Classify("a", /*control=*/true), GrammarToken::kBanned);
  // User-defined markup: never; other user-defined text is text.
  EXPECT_EQ(Classify("<think>", false, /*user_defined=*/true),
            GrammarToken::kBanned);
  EXPECT_EQ(Classify("<|channel>", false, true), GrammarToken::kBanned);
  EXPECT_EQ(Classify("[INST]", false, true), GrammarToken::kBanned);
  EXPECT_EQ(Classify("    ", false, true), GrammarToken::kAllowed);
  EXPECT_EQ(Classify("<think>"), GrammarToken::kAllowed);  // normal text
  // Empty, NUL, too long.
  EXPECT_EQ(Classify(""), GrammarToken::kBanned);
  EXPECT_EQ(Classify(std::string_view("a\0b", 3)), GrammarToken::kBanned);
  EXPECT_EQ(Classify(std::string_view("\0", 1)), GrammarToken::kBanned);
  EXPECT_EQ(Classify(std::string(kMaxGrammarPieceBytes, 'a')),
            GrammarToken::kAllowed);
  EXPECT_EQ(Classify(std::string(kMaxGrammarPieceBytes + 1, 'a')),
            GrammarToken::kBanned);
}

TEST(GrammarTokensTest, Utf8PrefixAcceptsValidTextInAnyPieces) {
  const std::string text =
      "a\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80\xED\x9F\xBF\xEE\x80\x80"
      "\xF4\x8F\xBF\xBF\xC2\x80";
  // Every split into two pieces, and byte by byte.
  for (size_t cut = 0; cut <= text.size(); ++cut) {
    Utf8Prefix p;
    ASSERT_TRUE(p.Append(text.substr(0, cut))) << cut;
    ASSERT_TRUE(p.Append(text.substr(cut))) << cut;
    EXPECT_FALSE(p.incomplete());
  }
  Utf8Prefix bytes;
  for (char c : text) ASSERT_TRUE(bytes.Append(std::string(1, c)));
  EXPECT_FALSE(bytes.incomplete());
  Utf8Prefix partial;
  EXPECT_TRUE(partial.Append("\xF0\x9F"));
  EXPECT_TRUE(partial.incomplete());
}

TEST(GrammarTokensTest, Utf8PrefixRefusesInvalidText) {
  for (const char* bad : {
           "\x80", "\xBF", "\xC0\x80", "\xC1\xBF", "\xE0\x80\x80",
           "\xE0\x9F\xBF", "\xED\xA0\x80", "\xED\xBF\xBF", "\xF0\x80\x80\x80",
           "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF",
           "\xC3\x41", "\xE6\x97\x41", "a\xC3\xC3",
       }) {
    Utf8Prefix p;
    EXPECT_FALSE(p.CanAppend(bad)) << bad;
    EXPECT_FALSE(p.Append(bad)) << bad;
  }
  // NUL, alone or inside.
  Utf8Prefix p;
  EXPECT_FALSE(p.CanAppend(std::string_view("\0", 1)));
  EXPECT_FALSE(p.CanAppend(std::string_view("a\0", 2)));
  // Across pieces: the lead decides what may follow.
  struct Split {
    const char* first;
    const char* second;
  };
  for (const Split& s : {Split{"\xE0", "\x80"}, Split{"\xED", "\xA0"},
                         Split{"\xF0", "\x8F"}, Split{"\xF4", "\x90"},
                         Split{"\xC3", "a"}, Split{"\xE6\x97", "\x41"},
                         Split{"\xE6", "\x97\x41"}}) {
    Utf8Prefix q;
    ASSERT_TRUE(q.Append(s.first));
    EXPECT_FALSE(q.CanAppend(s.second)) << s.first << "|" << s.second;
    EXPECT_FALSE(q.Append(s.second));
    // A refused piece leaves the state unchanged.
    EXPECT_TRUE(q.incomplete());
  }
  // An incomplete character can't be followed by a new one.
  Utf8Prefix r;
  ASSERT_TRUE(r.Append("\xE6"));
  EXPECT_FALSE(r.CanAppend("a"));
  EXPECT_TRUE(r.CanAppend("\x97\xA5"));
}

// Agrees with IsValidUtf8 (stream_filters) on random byte strings fed in
// random pieces: complete and accepted iff valid and NUL-free.
TEST(GrammarTokensTest, Utf8PrefixAgreesWithIsValidUtf8) {
  std::mt19937 rng(3);
  const unsigned char kBytes[] = {0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F,
                                  0xA0, 0xBF, 0xC0, 0xC2, 0xDF, 0xE0, 0xE1,
                                  0xED, 0xEF, 0xF0, 0xF3, 0xF4, 0xF5, 0xFF};
  for (int i = 0; i < 50000; ++i) {
    std::string s;
    const size_t n = rng() % 8;
    for (size_t k = 0; k < n; ++k) {
      s.push_back(static_cast<char>(kBytes[rng() % sizeof(kBytes)]));
    }
    Utf8Prefix p;
    bool ok = true;
    for (size_t at = 0; at < s.size() && ok;) {
      const size_t len = 1 + rng() % 3;
      ok = p.Append(s.substr(at, len));
      at += len;
    }
    const bool expected =
        IsValidUtf8(s) && s.find('\0') == std::string::npos;
    EXPECT_EQ(ok && !p.incomplete(), expected) << i;
  }
}

}  // namespace
}  // namespace molt_ai
