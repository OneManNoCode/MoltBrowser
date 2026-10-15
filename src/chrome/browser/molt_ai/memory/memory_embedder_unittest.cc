// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_embedder.h"

#include <cmath>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

float Dot(const std::vector<float>& a, const std::vector<float>& b) {
  float s = 0.0f;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    s += a[i] * b[i];
  }
  return s;
}

float Norm(const std::vector<float>& v) {
  return std::sqrt(Dot(v, v));
}

TEST(HashingEmbedderTest, OutputHasFixedDimension) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("hello world").size(), kEmbeddingDim);
  EXPECT_EQ(e.Embed("").size(), kEmbeddingDim);
}

TEST(HashingEmbedderTest, EmptyInputIsZeroVector) {
  HashingEmbedder e;
  EXPECT_EQ(Norm(e.Embed("")), 0.0f);
}

// Regression: uppercase bytes used to be classified as separators before
// lowercasing, so an all-caps acronym embedded to the zero vector.
TEST(HashingEmbedderTest, AllCapsWordIsNotDropped) {
  HashingEmbedder e;
  std::vector<float> v = e.Embed("NASA");
  EXPECT_NEAR(Norm(v), 1.0f, 1e-4f);
}

TEST(HashingEmbedderTest, CaseInsensitive) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("NASA"), e.Embed("nasa"));
  // "Paris" used to tokenize as "aris" (the capital P was a separator).
  EXPECT_EQ(e.Embed("Paris"), e.Embed("paris"));
  EXPECT_EQ(e.Embed("Trip To PARIS in May"), e.Embed("trip to paris in may"));
}

TEST(HashingEmbedderTest, CapitalizedQueryMatchesLowercaseDocument) {
  HashingEmbedder e;
  std::vector<float> doc =
      e.Embed("the nasa launch schedule for the artemis mission");
  std::vector<float> query = e.Embed("NASA Artemis");
  std::vector<float> unrelated = e.Embed("chocolate chip cookie recipe");
  EXPECT_GT(Dot(doc, query), Dot(unrelated, query));
  EXPECT_GT(Dot(doc, query), 0.2f);
}

TEST(HashingEmbedderTest, OutputIsL2Normalized) {
  HashingEmbedder e;
  EXPECT_NEAR(Norm(e.Embed("Some Mixed Case text, with punctuation!")), 1.0f,
              1e-4f);
}

TEST(HashingEmbedderTest, Deterministic) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("Paris is the capital of France"),
            e.Embed("Paris is the capital of France"));
}

// ---- Unicode (embedder v3) ----

TEST(HashingEmbedderTest, NonAsciiLettersAreKept) {
  HashingEmbedder e;
  EXPECT_NEAR(Norm(e.Embed("東京")), 1.0f, 1e-4f);
  EXPECT_NEAR(Norm(e.Embed("Москва")), 1.0f, 1e-4f);
  EXPECT_NEAR(Norm(e.Embed("ελληνικά")), 1.0f, 1e-4f);
  // The ï is part of the word, not a separator and not dropped.
  EXPECT_NE(e.Embed("naïve"), e.Embed("nave"));
  EXPECT_NE(e.Embed("naïve"), e.Embed("na ve"));
}

TEST(HashingEmbedderTest, NonAsciiCaseInsensitive) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("ÄRGER ÜBER BÄUME"), e.Embed("ärger über bäume"));
  EXPECT_EQ(e.Embed("ÉCOLE"), e.Embed("école"));
  EXPECT_EQ(e.Embed("ŁÓDŹ"), e.Embed("łódź"));
  EXPECT_EQ(e.Embed("İSTANBUL"), e.Embed("istanbul"));
  EXPECT_EQ(e.Embed("ȘTIINȚĂ"), e.Embed("știință"));
  EXPECT_EQ(e.Embed("ΑΘΗΝΑ"), e.Embed("αθηνα"));
  EXPECT_EQ(e.Embed("ΟΔΟΣ"), e.Embed("οδος"));  // final sigma
  EXPECT_EQ(e.Embed("Ά Έ Ώ"), e.Embed("ά έ ώ"));
  EXPECT_EQ(e.Embed("МОСКВА ЁЛКА"), e.Embed("москва ёлка"));
  EXPECT_EQ(e.Embed("VIỆT NAM"), e.Embed("việt nam"));
  // Fullwidth Latin folds to ASCII.
  EXPECT_EQ(e.Embed("ＮＡＳＡ２０２６"), e.Embed("nasa2026"));
}

TEST(HashingEmbedderTest, UnicodePunctuationSeparatesWords) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("naïve—café"), e.Embed("naïve café"));
  EXPECT_EQ(e.Embed("“quoted” «text»"), e.Embed("quoted text"));
  EXPECT_EQ(e.Embed("one two three"), e.Embed("one two three"));
  EXPECT_EQ(e.Embed("wait…what"), e.Embed("wait what"));
  EXPECT_EQ(e.Embed("東京。大阪、京都"), e.Embed("東京 大阪 京都"));
  EXPECT_EQ(e.Embed("price: 5€ ✓"), e.Embed("price 5"));
}

TEST(HashingEmbedderTest, InvisibleCharactersDoNotSplitWords) {
  HashingEmbedder e;
  EXPECT_EQ(e.Embed("co­operate"), e.Embed("cooperate"));
  EXPECT_EQ(e.Embed("﻿hello"), e.Embed("hello"));
  EXPECT_EQ(e.Embed("ab‍cd"), e.Embed("abcd"));
}

TEST(HashingEmbedderTest, MalformedUtf8IsKeptNotDropped) {
  HashingEmbedder e;
  EXPECT_NEAR(Norm(e.Embed("\xff\xfe")), 1.0f, 1e-4f);
  EXPECT_NEAR(Norm(e.Embed("\x80\x80\x80")), 1.0f, 1e-4f);
  // Truncated sequence at the end, overlong '/', encoded surrogate.
  EXPECT_NEAR(Norm(e.Embed("abc\xe2\x82")), 1.0f, 1e-4f);
  EXPECT_NEAR(Norm(e.Embed("\xc0\xaf")), 1.0f, 1e-4f);
  EXPECT_NEAR(Norm(e.Embed("\xed\xa0\x80")), 1.0f, 1e-4f);
  EXPECT_NE(e.Embed("abc\xff"), e.Embed("abc"));
}

TEST(HashingEmbedderTest, LongWordsAreCutOnCodePointBoundaries) {
  HashingEmbedder e;
  auto repeat = [](const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
  };
  // The hashed prefix is at most 64 bytes: 32 two-byte or 21 three-byte
  // code points; never part of one.
  EXPECT_EQ(e.Embed(repeat("ж", 40)), e.Embed(repeat("ж", 32)));
  EXPECT_NE(e.Embed(repeat("ж", 31)), e.Embed(repeat("ж", 32)));
  EXPECT_EQ(e.Embed(repeat("字", 30)), e.Embed(repeat("字", 21)));
  EXPECT_NE(e.Embed(repeat("字", 20)), e.Embed(repeat("字", 21)));
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
