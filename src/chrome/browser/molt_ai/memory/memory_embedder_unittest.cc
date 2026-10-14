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

}  // namespace
}  // namespace memory
}  // namespace molt_ai
