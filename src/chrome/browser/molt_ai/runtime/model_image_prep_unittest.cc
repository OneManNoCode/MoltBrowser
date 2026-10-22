// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/model_image_prep.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "services/data_decoder/public/cpp/test_support/in_process_data_decoder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/png_codec.h"

namespace molt_ai {
namespace {

SkBitmap Solid(int w, int h, SkColor color) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(w, h);
  bitmap.eraseColor(color);
  return bitmap;
}

// The RGB pixel at (x, y) of |image|.
const uint8_t* Pixel(const ModelImage& image, uint32_t x, uint32_t y) {
  return image.rgb.data() + (static_cast<size_t>(y) * image.width + x) * 3;
}

bool Near(int a, int b) {
  return std::abs(a - b) <= 2;
}

TEST(ModelImagePrepTest, ScalesALargeBitmapDown) {
  std::optional<ModelImage> image =
      PrepareImageForModel(Solid(2000, 1000, SK_ColorRED), kMaxImageSide);
  ASSERT_TRUE(image);
  EXPECT_TRUE(image->IsValid());
  EXPECT_EQ(1024u, image->width);
  EXPECT_EQ(512u, image->height);
  const uint8_t* p = Pixel(*image, 500, 250);
  EXPECT_TRUE(Near(255, p[0]));
  EXPECT_TRUE(Near(0, p[1]));
  EXPECT_TRUE(Near(0, p[2]));
}

TEST(ModelImagePrepTest, KeepsAFittingBitmapAsIs) {
  std::optional<ModelImage> image =
      PrepareImageForModel(Solid(300, 200, SK_ColorBLUE), kWebImageMaxSide);
  ASSERT_TRUE(image);
  EXPECT_EQ(300u, image->width);
  EXPECT_EQ(200u, image->height);
  const uint8_t* p = Pixel(*image, 0, 0);
  EXPECT_EQ(0, p[0]);
  EXPECT_EQ(0, p[1]);
  EXPECT_EQ(255, p[2]);
}

TEST(ModelImagePrepTest, PadsTinyAndExtremeBitmaps) {
  std::optional<ModelImage> image =
      PrepareImageForModel(Solid(10, 10, SK_ColorGREEN), kWebImageMaxSide);
  ASSERT_TRUE(image);
  EXPECT_EQ(32u, image->width);
  EXPECT_EQ(32u, image->height);
  EXPECT_EQ(kImagePadValue, Pixel(*image, 0, 0)[0]);
  EXPECT_EQ(255, Pixel(*image, 16, 16)[1]);

  image = PrepareImageForModel(Solid(4000, 10, SK_ColorGREEN), kMaxImageSide);
  ASSERT_TRUE(image);
  EXPECT_TRUE(image->IsValid());
  EXPECT_EQ(1024u, image->width);
  EXPECT_EQ(128u, image->height);
}

TEST(ModelImagePrepTest, TransparentPixelsBecomeTheBackground) {
  std::optional<ModelImage> image = PrepareImageForModel(
      Solid(64, 64, SK_ColorTRANSPARENT), kWebImageMaxSide);
  ASSERT_TRUE(image);
  for (uint8_t v : image->rgb) {
    ASSERT_EQ(kImagePadValue, v);
  }
}

TEST(ModelImagePrepTest, ConvertsOtherColourTypes) {
  SkBitmap bitmap;
  bitmap.allocPixels(SkImageInfo::Make(48, 40, kRGB_565_SkColorType,
                                       kOpaque_SkAlphaType));
  bitmap.eraseColor(SK_ColorWHITE);
  std::optional<ModelImage> image =
      PrepareImageForModel(bitmap, kWebImageMaxSide);
  ASSERT_TRUE(image);
  EXPECT_EQ(48u, image->width);
  EXPECT_TRUE(Near(255, Pixel(*image, 1, 1)[0]));
}

TEST(ModelImagePrepTest, RefusesEmptyBitmapsAndBadLimits) {
  EXPECT_FALSE(PrepareImageForModel(SkBitmap(), kWebImageMaxSide));
  EXPECT_FALSE(PrepareImageForModel(Solid(64, 64, SK_ColorRED), 0));
  EXPECT_FALSE(
      PrepareImageForModel(Solid(64, 64, SK_ColorRED), kMaxImageSide + 1));
}

// Image files are decoded in the data decoder (in-process for the test),
// then scaled on a worker.
class ModelImageDecodeTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
  data_decoder::test::InProcessDataDecoder decoder_;
};

TEST_F(ModelImageDecodeTest, DecodesAPngFile) {
  std::optional<std::vector<uint8_t>> png = gfx::PNGCodec::EncodeBGRASkBitmap(
      Solid(1600, 800, SK_ColorRED), /*discard_transparency=*/false);
  ASSERT_TRUE(png);
  base::test::TestFuture<std::optional<ModelImage>> future;
  DecodeImageForModel(*png, kWebImageMaxSide, future.GetCallback());
  const std::optional<ModelImage>& image = future.Get();
  ASSERT_TRUE(image);
  EXPECT_TRUE(image->IsValid());
  EXPECT_EQ(768u, image->width);
  EXPECT_EQ(384u, image->height);
  EXPECT_TRUE(Near(255, Pixel(*image, 100, 100)[0]));
}

TEST_F(ModelImageDecodeTest, NotAnImageIsNothing) {
  const std::vector<uint8_t> junk = {'G', 'G', 'U', 'F', 0, 1, 2, 3};
  base::test::TestFuture<std::optional<ModelImage>> future;
  DecodeImageForModel(junk, kWebImageMaxSide, future.GetCallback());
  EXPECT_FALSE(future.Get());
}

}  // namespace
}  // namespace molt_ai
