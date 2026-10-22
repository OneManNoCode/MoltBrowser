// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/embedding_utils.h"

#include <cmath>
#include <limits>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

using Tokens = std::vector<int32_t>;

constexpr int32_t kNone = -1;
constexpr int32_t kEnd = 99;

TEST(FitEmbeddingTokensTest, ShortTextIsUnchanged) {
  EXPECT_EQ(FitEmbeddingTokens({1, 2, 3}, 8, false, kNone), (Tokens{1, 2, 3}));
  EXPECT_EQ(FitEmbeddingTokens({1, 2, 3}, 8, true, kNone), (Tokens{1, 2, 3}));
}

TEST(FitEmbeddingTokensTest, LongTextKeepsItsHead) {
  EXPECT_EQ(FitEmbeddingTokens({1, 2, 3, 4, 5, 6}, 4, false, kNone),
            (Tokens{1, 2, 3, 4}));
}

// BERT-style [CLS] ... [SEP]: the end marker survives the cut.
TEST(FitEmbeddingTokensTest, KeepLastKeepsTheEndMarker) {
  EXPECT_EQ(FitEmbeddingTokens({101, 2, 3, 4, 5, 102}, 4, true, kNone),
            (Tokens{101, 2, 3, 102}));
}

TEST(FitEmbeddingTokensTest, TerminatorIsAppended) {
  EXPECT_EQ(FitEmbeddingTokens({1, 2}, 8, false, kEnd), (Tokens{1, 2, kEnd}));
}

TEST(FitEmbeddingTokensTest, TerminatorIsNotDoubled) {
  EXPECT_EQ(FitEmbeddingTokens({1, 2, kEnd}, 8, false, kEnd),
            (Tokens{1, 2, kEnd}));
}

TEST(FitEmbeddingTokensTest, TerminatorFitsInTheBudget) {
  const Tokens fitted = FitEmbeddingTokens({1, 2, 3, 4, 5, 6}, 4, false, kEnd);
  EXPECT_EQ(fitted, (Tokens{1, 2, 3, kEnd}));
}

// A tokenizer that already ends texts with the terminator: cutting keeps
// it even without keep_last.
TEST(FitEmbeddingTokensTest, ExistingTerminatorSurvivesTheCut) {
  EXPECT_EQ(FitEmbeddingTokens({1, 2, 3, 4, 5, kEnd}, 4, false, kEnd),
            (Tokens{1, 2, 3, kEnd}));
}

TEST(FitEmbeddingTokensTest, KeepLastAndTerminator) {
  // [CLS] a b c d [SEP] with a separate terminator: SEP kept, then the
  // terminator, within 5.
  EXPECT_EQ(FitEmbeddingTokens({101, 1, 2, 3, 4, 102}, 5, true, kEnd),
            (Tokens{101, 1, 2, 102, kEnd}));
}

TEST(FitEmbeddingTokensTest, EmptyText) {
  EXPECT_TRUE(FitEmbeddingTokens({}, 8, true, kNone).empty());
  EXPECT_EQ(FitEmbeddingTokens({}, 8, true, kEnd), (Tokens{kEnd}));
}

TEST(FitEmbeddingTokensTest, NeverLongerThanTheBudget) {
  for (size_t max = 2; max < 12; ++max) {
    for (size_t n = 0; n < 20; ++n) {
      Tokens t;
      for (size_t i = 0; i < n; ++i) t.push_back(static_cast<int32_t>(i));
      for (int32_t term : {kNone, kEnd}) {
        for (bool keep : {false, true}) {
          const Tokens fitted = FitEmbeddingTokens(t, max, keep, term);
          EXPECT_LE(fitted.size(), max);
          if (term >= 0) {
            ASSERT_FALSE(fitted.empty());
            EXPECT_EQ(fitted.back(), term);
          }
        }
      }
    }
  }
}

TEST(TruncateAndNormalizeTest, NormalizesTheLeadingDimensions) {
  const float v[] = {3.0f, 4.0f, 100.0f, -7.0f};
  float out[2] = {};
  ASSERT_TRUE(TruncateAndNormalize(v, 4, 2, out));
  EXPECT_FLOAT_EQ(out[0], 0.6f);
  EXPECT_FLOAT_EQ(out[1], 0.8f);
}

TEST(TruncateAndNormalizeTest, FullWidth) {
  const float v[] = {1.0f, 1.0f, 1.0f, 1.0f};
  float out[4] = {};
  ASSERT_TRUE(TruncateAndNormalize(v, 4, 4, out));
  for (float x : out) EXPECT_FLOAT_EQ(x, 0.5f);
}

TEST(TruncateAndNormalizeTest, ZeroVectorStaysZero) {
  const float v[] = {0.0f, 0.0f, 5.0f};
  float out[2] = {9.0f, 9.0f};
  ASSERT_TRUE(TruncateAndNormalize(v, 3, 2, out));
  EXPECT_EQ(out[0], 0.0f);
  EXPECT_EQ(out[1], 0.0f);
}

TEST(TruncateAndNormalizeTest, RejectsBadSizes) {
  const float v[] = {1.0f, 2.0f};
  float out[3] = {};
  EXPECT_FALSE(TruncateAndNormalize(v, 2, 0, out));
  EXPECT_FALSE(TruncateAndNormalize(v, 2, 3, out));
  EXPECT_FALSE(TruncateAndNormalize(nullptr, 2, 1, out));
  EXPECT_FALSE(TruncateAndNormalize(v, 2, 1, nullptr));
}

TEST(TruncateAndNormalizeTest, RejectsNonFiniteValues) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  float out[2] = {7.0f, 7.0f};
  const float with_nan[] = {1.0f, nan};
  EXPECT_FALSE(TruncateAndNormalize(with_nan, 2, 2, out));
  const float with_inf[] = {-inf, 1.0f};
  EXPECT_FALSE(TruncateAndNormalize(with_inf, 2, 2, out));
  // Finite values whose squares overflow a float but not a double are fine.
  const float huge[] = {3e38f, 3e38f};
  ASSERT_TRUE(TruncateAndNormalize(huge, 2, 2, out));
  EXPECT_NEAR(out[0], std::sqrt(0.5f), 1e-6f);
  // Unused trailing values aren't looked at.
  const float tail_nan[] = {1.0f, nan};
  ASSERT_TRUE(TruncateAndNormalize(tail_nan, 2, 1, out));
  EXPECT_FLOAT_EQ(out[0], 1.0f);
}

}  // namespace
}  // namespace molt_ai
