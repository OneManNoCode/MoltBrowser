// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_image.h"

#include <vector>

#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

ModelImage Image(uint32_t w, uint32_t h, size_t bytes) {
  return ModelImage(w, h, std::vector<uint8_t>(bytes, 7));
}

TEST(ModelImageTest, ValidOnlyWithinTheServiceBounds) {
  EXPECT_TRUE(Image(32, 32, 32 * 32 * 3).IsValid());
  EXPECT_TRUE(Image(768, 432, 768 * 432 * 3).IsValid());
  EXPECT_FALSE(ModelImage().IsValid());
  EXPECT_FALSE(Image(32, 32, 32 * 32 * 3 - 1).IsValid());
  EXPECT_FALSE(Image(32, 32, 32 * 32 * 4).IsValid());
  EXPECT_FALSE(Image(16, 16, 16 * 16 * 3).IsValid());
  EXPECT_FALSE(Image(2048, 1024, 0).IsValid());
  EXPECT_FALSE(Image(256, 31, 256 * 31 * 3).IsValid());
}

TEST(ModelImageTest, TheWebSizeIsWithinTheBounds) {
  EXPECT_GE(kWebImageMaxSide, kMinImageSide);
  EXPECT_LE(kWebImageMaxSide, kMaxImageSide);
}

TEST(ModelImageTest, PlansTheNewMessageFirst) {
  // Nothing in the history: all four of the new message's.
  std::optional<std::vector<size_t>> sent = PlanRequestImages(4, {});
  ASSERT_TRUE(sent);
  EXPECT_TRUE(sent->empty());
  // Five in the new message: refused, not cut.
  EXPECT_FALSE(PlanRequestImages(5, {}));
  EXPECT_FALSE(PlanRequestImages(5, {0, 0}));
  EXPECT_FALSE(PlanRequestImages(static_cast<size_t>(-1), {}));
}

TEST(ModelImageTest, FillsWhatIsLeftFromTheNewestTurns) {
  // Oldest first: 2, 0, 3 images; the new message has 1. Room for 3: the
  // newest turn's 3, nothing older.
  std::optional<std::vector<size_t>> sent = PlanRequestImages(1, {2, 0, 3});
  ASSERT_TRUE(sent);
  EXPECT_EQ((std::vector<size_t>{0, 0, 3}), *sent);
  // Room for 4: the newest turn's 3, then 1 of the oldest turn's.
  sent = PlanRequestImages(0, {2, 0, 3});
  ASSERT_TRUE(sent);
  EXPECT_EQ((std::vector<size_t>{1, 0, 3}), *sent);
  // Everything fits.
  sent = PlanRequestImages(1, {1, 1, 1});
  ASSERT_TRUE(sent);
  EXPECT_EQ((std::vector<size_t>{1, 1, 1}), *sent);
  // No room.
  sent = PlanRequestImages(4, {1, 2});
  ASSERT_TRUE(sent);
  EXPECT_EQ((std::vector<size_t>{0, 0}), *sent);
  // A turn with more than the cap.
  sent = PlanRequestImages(0, {100});
  ASSERT_TRUE(sent);
  EXPECT_EQ((std::vector<size_t>{4}), *sent);
}

TEST(ModelImageTest, NeverMoreThanTheCap) {
  for (size_t user = 0; user <= kMaxImagesPerRequest; ++user) {
    for (size_t a = 0; a < 6; ++a) {
      for (size_t b = 0; b < 6; ++b) {
        for (size_t c = 0; c < 6; ++c) {
          std::optional<std::vector<size_t>> sent =
              PlanRequestImages(user, {a, b, c});
          ASSERT_TRUE(sent);
          const std::vector<size_t> have = {a, b, c};
          size_t total = user;
          for (size_t i = 0; i < 3; ++i) {
            ASSERT_LE((*sent)[i], have[i]);
            total += (*sent)[i];
          }
          ASSERT_LE(total, kMaxImagesPerRequest);
          // As many as fit are sent.
          EXPECT_EQ(std::min(kMaxImagesPerRequest, user + a + b + c), total);
          // A turn sends some only if every newer turn sends all.
          for (size_t i = 0; i < 3; ++i) {
            if ((*sent)[i] == 0) continue;
            for (size_t j = i + 1; j < 3; ++j) {
              EXPECT_EQ(have[j], (*sent)[j]);
            }
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace molt_ai
