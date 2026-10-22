// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/image_input.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr uint32_t kU32Max = std::numeric_limits<uint32_t>::max();
constexpr size_t kSizeMax = std::numeric_limits<size_t>::max();

size_t Bytes(uint32_t w, uint32_t h) {
  return static_cast<size_t>(w) * h * 3;
}

TEST(ImageInputTest, AcceptsImagesWithinTheBounds) {
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(32, 32, Bytes(32, 32)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(1024, 1024, Bytes(1024, 1024)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(1024, 128, Bytes(1024, 128)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(128, 1024, Bytes(128, 1024)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(256, 32, Bytes(256, 32)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(768, 433, Bytes(768, 433)));
  EXPECT_EQ(kMaxImageBytes, Bytes(1024, 1024));
}

TEST(ImageInputTest, RefusesSidesOutOfRange) {
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(0, 0, 0));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(31, 32, Bytes(31, 32)));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(32, 31, Bytes(32, 31)));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(1025, 512, Bytes(1025, 512)));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(512, 1025, Bytes(512, 1025)));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(1, 1, 3));
}

TEST(ImageInputTest, HostileDimensionsDontOverflow) {
  // width * height * 3 wraps in 32 and 64 bits for these; the sides are
  // refused before any product is formed.
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(kU32Max, kU32Max, 0));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(kU32Max, 1, kSizeMax));
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(0x55555556, 3, 2));
  // 2^31 x 2: 3 * 2^32 wraps to 0 in 32 bits.
  EXPECT_EQ(ImageCheck::kBadSize, CheckModelImage(0x80000000u, 2, 0));
  // A buffer size that would match a wrapped product.
  EXPECT_EQ(ImageCheck::kBadByteLength,
            CheckModelImage(1024, 1024, Bytes(1024, 1024) + (size_t{1} << 32)));
}

TEST(ImageInputTest, RefusesExtremeAspectRatios) {
  EXPECT_EQ(ImageCheck::kBadAspectRatio, CheckModelImage(257, 32, Bytes(257, 32)));
  EXPECT_EQ(ImageCheck::kBadAspectRatio, CheckModelImage(32, 257, Bytes(32, 257)));
  EXPECT_EQ(ImageCheck::kBadAspectRatio,
            CheckModelImage(1024, 127, Bytes(1024, 127)));
  EXPECT_EQ(ImageCheck::kOk, CheckModelImage(1024, 128, Bytes(1024, 128)));
}

TEST(ImageInputTest, RequiresTheExactByteLength) {
  EXPECT_EQ(ImageCheck::kBadByteLength, CheckModelImage(64, 64, 0));
  EXPECT_EQ(ImageCheck::kBadByteLength,
            CheckModelImage(64, 64, Bytes(64, 64) - 1));
  EXPECT_EQ(ImageCheck::kBadByteLength,
            CheckModelImage(64, 64, Bytes(64, 64) + 1));
  // RGBA, not RGB.
  EXPECT_EQ(ImageCheck::kBadByteLength, CheckModelImage(64, 64, 64 * 64 * 4));
  EXPECT_EQ(ImageCheck::kBadByteLength, CheckModelImage(64, 64, kSizeMax));
}

void ExpectValidPlan(const ImagePlan& p,
                     uint32_t src_w,
                     uint32_t src_h,
                     uint32_t max_side) {
  SCOPED_TRACE(testing::Message() << src_w << "x" << src_h << " max "
                                  << max_side);
  EXPECT_EQ(ImageCheck::kOk,
            CheckModelImage(p.canvas_width, p.canvas_height,
                            Bytes(p.canvas_width, p.canvas_height)));
  EXPECT_GE(p.content_width, 1u);
  EXPECT_GE(p.content_height, 1u);
  // Never scaled up; the long side fits |max_side|.
  EXPECT_LE(p.content_width, src_w);
  EXPECT_LE(p.content_height, src_h);
  EXPECT_LE(std::max(p.content_width, p.content_height), max_side);
  // Orientation is kept (a near-square image may round to square).
  if (src_w > src_h) {
    EXPECT_GE(p.content_width, p.content_height);
    EXPECT_GE(p.canvas_width, p.canvas_height);
  } else if (src_w < src_h) {
    EXPECT_LE(p.content_width, p.content_height);
    EXPECT_LE(p.canvas_width, p.canvas_height);
  }
  // The content is inside the canvas, centred.
  EXPECT_LE(uint64_t{p.offset_x} + p.content_width, p.canvas_width);
  EXPECT_LE(uint64_t{p.offset_y} + p.content_height, p.canvas_height);
  EXPECT_EQ((p.canvas_width - p.content_width) / 2, p.offset_x);
  EXPECT_EQ((p.canvas_height - p.content_height) / 2, p.offset_y);
  // Only scaled when it had to be, and then to exactly max_side.
  const uint32_t src_long = std::max(src_w, src_h);
  if (src_long <= max_side) {
    EXPECT_EQ(src_w, p.content_width);
    EXPECT_EQ(src_h, p.content_height);
  } else {
    EXPECT_EQ(max_side, std::max(p.content_width, p.content_height));
    // The short side is the rounded proportional size (or 1).
    const double exact = static_cast<double>(std::min(src_w, src_h)) *
                         max_side / src_long;
    const double got = std::min(p.content_width, p.content_height);
    EXPECT_LE(std::abs(got - std::max(1.0, exact)), 0.5 + 1e-9);
  }
  // No padding where none is needed.
  if (p.content_width >= kMinImageSide && p.content_height >= kMinImageSide &&
      std::max(p.content_width, p.content_height) <=
          kMaxImageAspectRatio * std::min(p.content_width, p.content_height)) {
    EXPECT_EQ(p.content_width, p.canvas_width);
    EXPECT_EQ(p.content_height, p.canvas_height);
  }
}

TEST(ImageInputTest, PlansFittingImagesUnchanged) {
  std::optional<ImagePlan> p = PlanModelImage(800, 600, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(800u, p->content_width);
  EXPECT_EQ(600u, p->content_height);
  EXPECT_EQ(800u, p->canvas_width);
  EXPECT_EQ(600u, p->canvas_height);
  EXPECT_EQ(0u, p->offset_x);
  EXPECT_EQ(0u, p->offset_y);
  ExpectValidPlan(*p, 800, 600, 1024);
}

TEST(ImageInputTest, ScalesDownToTheLongSide) {
  std::optional<ImagePlan> p = PlanModelImage(1920, 1080, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1024u, p->content_width);
  EXPECT_EQ(576u, p->content_height);
  EXPECT_EQ(1024u, p->canvas_width);
  EXPECT_EQ(576u, p->canvas_height);
  ExpectValidPlan(*p, 1920, 1080, 1024);

  p = PlanModelImage(1080, 1920, 768);
  ASSERT_TRUE(p);
  EXPECT_EQ(432u, p->content_width);
  EXPECT_EQ(768u, p->content_height);
  ExpectValidPlan(*p, 1080, 1920, 768);

  // 1000 * 1024 / 3000 = 341.33 -> 341; 2000 * 1024 / 3000 = 682.67 -> 683.
  p = PlanModelImage(3000, 1000, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(341u, p->content_height);
  p = PlanModelImage(3000, 2000, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(683u, p->content_height);
}

TEST(ImageInputTest, PadsTinyImages) {
  std::optional<ImagePlan> p = PlanModelImage(1, 1, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1u, p->content_width);
  EXPECT_EQ(1u, p->content_height);
  EXPECT_EQ(32u, p->canvas_width);
  EXPECT_EQ(32u, p->canvas_height);
  EXPECT_EQ(15u, p->offset_x);
  EXPECT_EQ(15u, p->offset_y);
  ExpectValidPlan(*p, 1, 1, 1024);

  p = PlanModelImage(16, 100, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(32u, p->canvas_width);
  EXPECT_EQ(100u, p->canvas_height);
  EXPECT_EQ(8u, p->offset_x);
  ExpectValidPlan(*p, 16, 100, 1024);
}

TEST(ImageInputTest, PadsExtremeAspectRatios) {
  // A long strip: 1024 x 3 content on a 1024 x 128 canvas.
  std::optional<ImagePlan> p = PlanModelImage(4000, 10, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1024u, p->content_width);
  EXPECT_EQ(3u, p->content_height);
  EXPECT_EQ(1024u, p->canvas_width);
  EXPECT_EQ(128u, p->canvas_height);
  EXPECT_EQ(62u, p->offset_y);
  ExpectValidPlan(*p, 4000, 10, 1024);

  // A one-pixel-wide column scales to a 1 x 1024 line.
  p = PlanModelImage(1, 100000, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1u, p->content_width);
  EXPECT_EQ(1024u, p->content_height);
  EXPECT_EQ(128u, p->canvas_width);
  EXPECT_EQ(1024u, p->canvas_height);
  ExpectValidPlan(*p, 1, 100000, 1024);

  // 9:1 within the size bounds still gets padded to 8:1.
  p = PlanModelImage(900, 100, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(900u, p->canvas_width);
  EXPECT_EQ(113u, p->canvas_height);  // ceil(900 / 8)
  ExpectValidPlan(*p, 900, 100, 1024);
}

TEST(ImageInputTest, HostileSourceSizes) {
  std::optional<ImagePlan> p = PlanModelImage(kU32Max, kU32Max, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1024u, p->content_width);
  EXPECT_EQ(1024u, p->content_height);
  ExpectValidPlan(*p, kU32Max, kU32Max, 1024);

  p = PlanModelImage(kU32Max, 1, 1024);
  ASSERT_TRUE(p);
  EXPECT_EQ(1024u, p->content_width);
  EXPECT_EQ(1u, p->content_height);
  ExpectValidPlan(*p, kU32Max, 1, 1024);

  p = PlanModelImage(kU32Max - 1, kU32Max, 32);
  ASSERT_TRUE(p);
  ExpectValidPlan(*p, kU32Max - 1, kU32Max, 32);

  EXPECT_FALSE(PlanModelImage(0, 100, 1024));
  EXPECT_FALSE(PlanModelImage(100, 0, 1024));
  EXPECT_FALSE(PlanModelImage(0, 0, 1024));
}

TEST(ImageInputTest, RefusesLimitsOutsideTheBounds) {
  EXPECT_FALSE(PlanModelImage(100, 100, 0));
  EXPECT_FALSE(PlanModelImage(100, 100, kMinImageSide - 1));
  EXPECT_FALSE(PlanModelImage(100, 100, kMaxImageSide + 1));
  EXPECT_FALSE(PlanModelImage(100, 100, kU32Max));
  EXPECT_TRUE(PlanModelImage(100, 100, kMinImageSide));
  EXPECT_TRUE(PlanModelImage(100, 100, kMaxImageSide));
}

TEST(ImageInputTest, EveryPlanIsValid) {
  const uint32_t interesting[] = {1,    2,    3,    7,    8,     9,
                                  31,   32,   33,   127,  128,   129,
                                  255,  256,  257,  767,  768,   769,
                                  1023, 1024, 1025, 4096, 65535, 65536,
                                  1u << 31, kU32Max - 1, kU32Max};
  const uint32_t limits[] = {kMinImageSide, 33, 255, 256, 768, 1000,
                             kMaxImageSide};
  for (uint32_t w : interesting) {
    for (uint32_t h : interesting) {
      for (uint32_t max_side : limits) {
        std::optional<ImagePlan> p = PlanModelImage(w, h, max_side);
        ASSERT_TRUE(p);
        ExpectValidPlan(*p, w, h, max_side);
      }
    }
  }
  std::mt19937 rng(7);
  std::uniform_int_distribution<uint32_t> side(1, 20000);
  std::uniform_int_distribution<uint32_t> limit(kMinImageSide, kMaxImageSide);
  for (int i = 0; i < 20000; ++i) {
    const uint32_t w = side(rng);
    const uint32_t h = side(rng);
    const uint32_t max_side = limit(rng);
    std::optional<ImagePlan> p = PlanModelImage(w, h, max_side);
    ASSERT_TRUE(p);
    ExpectValidPlan(*p, w, h, max_side);
  }
}

// RGBA content of |w| x |h| with |stride| bytes per row.
std::vector<uint8_t> Rgba(uint32_t w,
                          uint32_t h,
                          size_t stride,
                          uint8_t r,
                          uint8_t g,
                          uint8_t b,
                          uint8_t a) {
  std::vector<uint8_t> v(stride * h, 0xEE);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* px = v.data() + y * stride + x * 4;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = a;
    }
  }
  return v;
}

TEST(ImageInputTest, ComposesOpaquePixelsExactly) {
  std::optional<ImagePlan> plan = PlanModelImage(40, 36, 1024);
  ASSERT_TRUE(plan);
  // Rows padded to 44 pixels; the padding bytes must not be read as
  // pixels.
  std::vector<uint8_t> src = Rgba(40, 36, 44 * 4, 10, 20, 30, 255);
  std::optional<std::vector<uint8_t>> out =
      ComposeModelImage(*plan, src.data(), 44 * 4, src.size());
  ASSERT_TRUE(out);
  ASSERT_EQ(Bytes(40, 36), out->size());
  for (size_t i = 0; i < out->size(); i += 3) {
    ASSERT_EQ(10, (*out)[i]);
    ASSERT_EQ(20, (*out)[i + 1]);
    ASSERT_EQ(30, (*out)[i + 2]);
  }
}

TEST(ImageInputTest, CompositesAlphaOverThePadColour) {
  std::optional<ImagePlan> plan = PlanModelImage(32, 32, 1024);
  ASSERT_TRUE(plan);
  std::vector<uint8_t> clear = Rgba(32, 32, 32 * 4, 255, 0, 77, 0);
  std::optional<std::vector<uint8_t>> out =
      ComposeModelImage(*plan, clear.data(), 32 * 4, clear.size());
  ASSERT_TRUE(out);
  for (uint8_t v : *out) {
    ASSERT_EQ(kImagePadValue, v);
  }
  // Half transparent white over 128: (255 * 128 + 128 * 127 + 127) / 255.
  std::vector<uint8_t> half = Rgba(32, 32, 32 * 4, 255, 255, 255, 128);
  out = ComposeModelImage(*plan, half.data(), 32 * 4, half.size());
  ASSERT_TRUE(out);
  EXPECT_EQ((255 * 128 + 128 * 127 + 127) / 255, (*out)[0]);
}

TEST(ImageInputTest, PadsAroundTheContent) {
  std::optional<ImagePlan> plan = PlanModelImage(2, 1, 1024);
  ASSERT_TRUE(plan);
  ASSERT_EQ(32u, plan->canvas_width);
  ASSERT_EQ(32u, plan->canvas_height);
  const uint8_t src[] = {1, 2, 3, 255, 4, 5, 6, 255};
  std::optional<std::vector<uint8_t>> out =
      ComposeModelImage(*plan, src, 8, sizeof(src));
  ASSERT_TRUE(out);
  size_t content = 0;
  for (uint32_t y = 0; y < 32; ++y) {
    for (uint32_t x = 0; x < 32; ++x) {
      const uint8_t* px = out->data() + (y * 32 + x) * 3;
      if (y == plan->offset_y && x == plan->offset_x) {
        EXPECT_EQ(1, px[0]);
        EXPECT_EQ(3, px[2]);
        ++content;
      } else if (y == plan->offset_y && x == plan->offset_x + 1) {
        EXPECT_EQ(4, px[0]);
        EXPECT_EQ(6, px[2]);
        ++content;
      } else {
        ASSERT_EQ(kImagePadValue, px[0]);
        ASSERT_EQ(kImagePadValue, px[1]);
        ASSERT_EQ(kImagePadValue, px[2]);
      }
    }
  }
  EXPECT_EQ(2u, content);
}

TEST(ImageInputTest, RefusesBuffersThatDontHoldThePixels) {
  std::optional<ImagePlan> plan = PlanModelImage(40, 36, 1024);
  ASSERT_TRUE(plan);
  std::vector<uint8_t> src = Rgba(40, 36, 40 * 4, 1, 2, 3, 255);
  EXPECT_TRUE(ComposeModelImage(*plan, src.data(), 40 * 4, src.size()));
  // One byte short.
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), 40 * 4, src.size() - 1));
  // Rows narrower than the content.
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), 40 * 4 - 1, src.size()));
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), 0, src.size()));
  // A row stride whose product with the height wraps around.
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), kSizeMax, src.size()));
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), kSizeMax / 35 + 1,
                                 src.size()));
  EXPECT_FALSE(ComposeModelImage(*plan, nullptr, 40 * 4, src.size()));
  EXPECT_FALSE(ComposeModelImage(*plan, src.data(), 40 * 4, 0));
}

TEST(ImageInputTest, RefusesPlansItDidntMake) {
  std::vector<uint8_t> src = Rgba(64, 64, 64 * 4, 1, 2, 3, 255);
  ImagePlan plan;
  plan.content_width = 64;
  plan.content_height = 64;
  plan.canvas_width = 64;
  plan.canvas_height = 64;
  EXPECT_TRUE(ComposeModelImage(plan, src.data(), 64 * 4, src.size()));

  ImagePlan bad = plan;
  bad.offset_x = 1;  // content would spill past the canvas
  EXPECT_FALSE(ComposeModelImage(bad, src.data(), 64 * 4, src.size()));
  bad = plan;
  bad.offset_y = kU32Max;  // wraps in 32 bits
  EXPECT_FALSE(ComposeModelImage(bad, src.data(), 64 * 4, src.size()));
  bad = plan;
  bad.canvas_width = 2000;  // over kMaxImageSide
  EXPECT_FALSE(ComposeModelImage(bad, src.data(), 64 * 4, src.size()));
  bad = plan;
  bad.canvas_height = 16;  // under kMinImageSide
  EXPECT_FALSE(ComposeModelImage(bad, src.data(), 64 * 4, src.size()));
  bad = plan;
  bad.content_width = 0;
  EXPECT_FALSE(ComposeModelImage(bad, src.data(), 64 * 4, src.size()));
}

}  // namespace
}  // namespace molt_ai
