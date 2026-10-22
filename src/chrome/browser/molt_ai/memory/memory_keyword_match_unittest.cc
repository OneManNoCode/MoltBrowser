// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Keyword ranking for the @memory omnibox shortcut's first answer
// (memory_keyword_match.h) and the hashing tokenizer's words it uses.

#include "chrome/browser/molt_ai/memory/memory_keyword_match.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace memory {
namespace {

using Words = std::vector<std::string>;

TEST(MemoryKeywordMatchTest, FoldedWordsReadLikeTheHashingEmbedder) {
  EXPECT_EQ(FoldedWords("Miso-Soup, RECIPE!", 10),
            (Words{"miso", "soup", "recipe"}));
  // Unicode case folding and separators; soft hyphen dropped in a word.
  EXPECT_EQ(FoldedWords("\xC3\x84RGER\xE2\x80\x94" "caf\xC3\xA9 "
                        "co\xC2\xAD" "operate",
                        10),
            (Words{"\xC3\xA4rger", "caf\xC3\xA9", "cooperate"}));
  EXPECT_EQ(FoldedWords("a b c d", 2), (Words{"a", "b"}));
  EXPECT_TRUE(FoldedWords("", 10).empty());
  EXPECT_TRUE(FoldedWords(" -- ", 10).empty());
  // Words are cut to 64 bytes.
  const Words long_word = FoldedWords(std::string(100, 'x'), 10);
  ASSERT_EQ(long_word.size(), 1u);
  EXPECT_EQ(long_word[0], std::string(64, 'x'));
}

TEST(MemoryKeywordMatchTest, QueryWordsAreFoldedDistinctAndCapped) {
  EXPECT_EQ(ParseKeywordQuery("Miso  miso SOUP").words, (Words{"miso", "soup"}));
  EXPECT_TRUE(ParseKeywordQuery("  ...  ").words.empty());
  const KeywordQuery many = ParseKeywordQuery("a b c d e f g h i j k");
  EXPECT_EQ(many.words.size(), kMaxKeywordQueryWords);
  EXPECT_EQ(many.words.front(), "a");
}

TEST(MemoryKeywordMatchTest, WordsMatchByPrefixOrInsideForNonAscii) {
  EXPECT_TRUE(KeywordWordMatches("mis", "miso"));
  EXPECT_TRUE(KeywordWordMatches("miso", "miso"));
  EXPECT_FALSE(KeywordWordMatches("iso", "miso"));
  EXPECT_FALSE(KeywordWordMatches("misos", "miso"));
  EXPECT_FALSE(KeywordWordMatches("", "miso"));
  // 味噌 inside 味噌汁 (no spaces in Japanese).
  EXPECT_TRUE(KeywordWordMatches("\xE5\x99\x8C",
                                 "\xE5\x91\xB3\xE5\x99\x8C\xE6\xB1\x81"));
}

TEST(MemoryKeywordMatchTest, EveryWordMustBeSomewhere) {
  const KeywordQuery q = ParseKeywordQuery("miso soup");
  EXPECT_GT(KeywordScore(q, "Miso soup recipe", "https://a.example/", ""), 0);
  EXPECT_EQ(KeywordScore(q, "Miso ramen", "https://a.example/", ""), 0);
  // "soup" from the URL or the text completes it.
  EXPECT_GT(KeywordScore(q, "Miso", "https://a.example/soup", ""), 0);
  EXPECT_GT(KeywordScore(q, "Miso", "https://a.example/", "a hot soup"), 0);
  EXPECT_EQ(KeywordScore(ParseKeywordQuery(""), "Miso", "", ""), 0);
}

TEST(MemoryKeywordMatchTest, TitleBeatsUrlBeatsText) {
  const KeywordQuery q = ParseKeywordQuery("miso");
  const float title = KeywordScore(q, "Miso", "https://a.example/", "");
  const float url = KeywordScore(q, "Food", "https://miso.example/", "");
  const float text = KeywordScore(q, "Food", "https://a.example/", "miso");
  EXPECT_FLOAT_EQ(title, kTitleMatchWeight);
  EXPECT_FLOAT_EQ(url, kUrlMatchWeight);
  EXPECT_FLOAT_EQ(text, kTextMatchWeight);
  // Mean over the words.
  EXPECT_FLOAT_EQ(KeywordScore(ParseKeywordQuery("miso soup"), "Miso",
                               "https://a.example/soup", ""),
                  (kTitleMatchWeight + kUrlMatchWeight) / 2);
}

TEST(MemoryKeywordMatchTest, SchemeIsNotAWord) {
  EXPECT_EQ(KeywordScore(ParseKeywordQuery("https"), "Page",
                         "https://a.example/", ""),
            0);
  EXPECT_EQ(KeywordScore(ParseKeywordQuery("http"), "Page",
                         "http://a.example/", ""),
            0);
  EXPECT_GT(KeywordScore(ParseKeywordQuery("example"), "Page",
                         "https://a.example/", ""),
            0);
}

TEST(MemoryKeywordMatchTest, RankingIsBestScoreThenRecencyOnePerPage) {
  std::vector<KeywordCandidate> c = {
      {1, 0.3f, 100}, {2, 1.0f, 50}, {3, 1.0f, 70},
      {1, 0.9f, 100},  // page 1 again, better
      {4, 0.6f, 10},  {5, 1.0f, 70},
  };
  std::vector<KeywordCandidate> ranked = RankKeywordCandidates(c, 10);
  ASSERT_EQ(ranked.size(), 5u);
  EXPECT_EQ(ranked[0].doc_id, 5);  // score 1, newest; doc id breaks the tie
  EXPECT_EQ(ranked[1].doc_id, 3);
  EXPECT_EQ(ranked[2].doc_id, 2);
  EXPECT_EQ(ranked[3].doc_id, 1);
  EXPECT_FLOAT_EQ(ranked[3].score, 0.9f);
  EXPECT_EQ(ranked[4].doc_id, 4);
  EXPECT_EQ(RankKeywordCandidates(c, 2).size(), 2u);
  EXPECT_TRUE(RankKeywordCandidates(c, 0).empty());
  EXPECT_TRUE(RankKeywordCandidates({}, 5).empty());
}

TEST(MemoryKeywordMatchTest, RankingSurvivesNonFiniteScores) {
  std::vector<KeywordCandidate> c = {
      {1, std::numeric_limits<float>::quiet_NaN(), 1},
      {2, 0.5f, 1},
      {3, std::numeric_limits<float>::infinity(), 1},
  };
  std::vector<KeywordCandidate> ranked = RankKeywordCandidates(c, 3);
  ASSERT_EQ(ranked.size(), 3u);
  EXPECT_EQ(ranked[0].doc_id, 2);
  for (const KeywordCandidate& k : ranked) {
    EXPECT_TRUE(std::isfinite(k.score));
  }
}

}  // namespace
}  // namespace memory
}  // namespace molt_ai
