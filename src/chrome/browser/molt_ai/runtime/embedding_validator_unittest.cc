// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/embedding_validator.h"

#include <cmath>
#include <limits>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

std::vector<float> Unit(size_t dims, size_t hot) {
  std::vector<float> v(dims, 0.0f);
  v[hot] = 1.0f;
  return v;
}

std::vector<float> Concat(const std::vector<std::vector<float>>& rows) {
  std::vector<float> out;
  for (const auto& r : rows) out.insert(out.end(), r.begin(), r.end());
  return out;
}

TEST(EmbeddingReplyTest, AcceptsUnitAndZeroVectors) {
  const std::vector<float> spread(4, 0.5f);  // norm 1
  const std::vector<float> values =
      Concat({Unit(4, 0), spread, std::vector<float>(4, 0.0f)});
  EXPECT_EQ(CheckEmbeddingReply(values, 3, 4), EmbeddingReplyError::kNone);
}

TEST(EmbeddingReplyTest, AcceptsFloatRounding) {
  std::vector<float> v(1024, 1.0f / std::sqrt(1024.0f));
  v[3] *= 1.0001f;
  EXPECT_EQ(CheckEmbeddingReply(v, 1, 1024), EmbeddingReplyError::kNone);
}

TEST(EmbeddingReplyTest, RejectsWrongCounts) {
  const std::vector<float> values = Concat({Unit(4, 0), Unit(4, 1)});
  EXPECT_EQ(CheckEmbeddingReply(values, 3, 4),
            EmbeddingReplyError::kWrongCount);
  EXPECT_EQ(CheckEmbeddingReply(values, 1, 4),
            EmbeddingReplyError::kWrongCount);
  EXPECT_EQ(CheckEmbeddingReply(values, 2, 3),
            EmbeddingReplyError::kWrongCount);
  EXPECT_EQ(CheckEmbeddingReply(values, 2, 0),
            EmbeddingReplyError::kWrongCount);
  EXPECT_EQ(CheckEmbeddingReply({}, 1, 4), EmbeddingReplyError::kWrongCount);
  // texts * dimensions overflowing size_t can't match anything.
  EXPECT_EQ(CheckEmbeddingReply(values,
                                std::numeric_limits<size_t>::max() / 2, 4),
            EmbeddingReplyError::kWrongCount);
  EXPECT_EQ(CheckEmbeddingReply({}, 0, 4), EmbeddingReplyError::kNone);
}

TEST(EmbeddingReplyTest, RejectsNonFiniteValues) {
  for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(),
                    -std::numeric_limits<float>::infinity()}) {
    std::vector<float> v = Unit(4, 0);
    v[2] = bad;
    EXPECT_EQ(CheckEmbeddingReply(v, 1, 4), EmbeddingReplyError::kNotFinite);
  }
}

TEST(EmbeddingReplyTest, RejectsUnnormalizedVectors) {
  std::vector<float> big = Unit(4, 0);
  big[0] = 2.0f;
  EXPECT_EQ(CheckEmbeddingReply(big, 1, 4),
            EmbeddingReplyError::kNotNormalized);
  std::vector<float> tiny = Unit(4, 0);
  tiny[0] = 1e-20f;  // not zero, not unit
  EXPECT_EQ(CheckEmbeddingReply(tiny, 1, 4),
            EmbeddingReplyError::kNotNormalized);
  // A good first row doesn't excuse a bad second one.
  EXPECT_EQ(CheckEmbeddingReply(Concat({Unit(4, 0), big}), 2, 4),
            EmbeddingReplyError::kNotNormalized);
}

TEST(EmbeddingReplyTest, Split) {
  const std::vector<float> values = Concat({Unit(3, 0), Unit(3, 2)});
  const std::vector<std::vector<float>> rows = SplitEmbeddingReply(values, 3);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0], Unit(3, 0));
  EXPECT_EQ(rows[1], Unit(3, 2));
  EXPECT_TRUE(SplitEmbeddingReply(values, 0).empty());
}

EmbeddingExpectations Expect() {
  EmbeddingExpectations e;
  e.dimensions = 256;
  e.max_tokens_per_text = 512;
  e.native_dimensions = 1024;
  e.architecture = "qwen3";
  return e;
}

TEST(EmbeddingDescriptionTest, AcceptsTheExpectedModel) {
  EXPECT_TRUE(IsValidEmbeddingDescription("qwen3", "metal", 1024, 256, 512,
                                          151669, Expect()));
  EXPECT_TRUE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 256, 256,
                                          151669, Expect()))
      << "a smaller window than asked for is fine";
  EmbeddingExpectations any = Expect();
  any.native_dimensions = 0;
  any.architecture.clear();
  EXPECT_TRUE(IsValidEmbeddingDescription("nomic-bert", "vulkan", 768, 256,
                                          512, 30522, any));
}

TEST(EmbeddingDescriptionTest, RejectsLies) {
  const EmbeddingExpectations e = Expect();
  EXPECT_FALSE(IsValidEmbeddingDescription("llama", "cpu", 1024, 256, 512,
                                           100, e))
      << "another architecture";
  EXPECT_FALSE(IsValidEmbeddingDescription("Qwen3", "cpu", 1024, 256, 512,
                                           100, e));
  EXPECT_FALSE(IsValidEmbeddingDescription("", "cpu", 1024, 256, 512, 100, e));
  EXPECT_FALSE(IsValidEmbeddingDescription(std::string(65, 'a'), "cpu", 1024,
                                           256, 512, 100, EmbeddingExpectations{
                                               256, 512, 0, ""}));
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cuda", 1024, 256, 512,
                                           100, e));
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 128, 512,
                                           100, e))
      << "other dimensions";
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 768, 256, 512,
                                           100, e))
      << "another width";
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 256, 513,
                                           100, e))
      << "a bigger window than asked for";
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 256, 4,
                                           100, e));
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 256, 512, 0,
                                           e));
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 256, 512,
                                           (1u << 22) + 1, e));
  EmbeddingExpectations any = e;
  any.native_dimensions = 0;
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 128, 256, 512, 100,
                                           any))
      << "narrower than the vectors";
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 70000, 256, 512,
                                           100, any));
  EmbeddingExpectations none = e;
  none.dimensions = 0;
  EXPECT_FALSE(IsValidEmbeddingDescription("qwen3", "cpu", 1024, 0, 512, 100,
                                           none));
}

}  // namespace
}  // namespace molt_ai
