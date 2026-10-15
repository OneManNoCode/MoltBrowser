// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

#include <array>
#include <cstdint>
#include <vector>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/skia_span_util.h"

namespace blink::molt_fingerprinting {
namespace {

// These tests never configure kUniform: that pins the process-wide time zone
// and locale, which would leak into other tests in blink_unittests.
class MoltFingerprintingTest : public testing::Test {
 protected:
  void SetUp() override { ResetForTesting(); }
  void TearDown() override { ResetForTesting(); }

  test::TaskEnvironment task_environment_;
};

SiteKey TestKey() {
  SiteKey key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(3 + 17 * i);
  }
  return key;
}

SkBitmap BusyBitmap(SkColorType type, SkAlphaType alpha, size_t row_bytes) {
  SkBitmap bitmap;
  const SkImageInfo info = SkImageInfo::Make(48, 16, type, alpha);
  EXPECT_TRUE(bitmap.tryAllocPixels(info, row_bytes));
  base::span<uint8_t> bytes = gfx::SkPixmapToWritableSpan(bitmap.pixmap());
  uint32_t state = 12345;
  for (uint8_t& b : bytes) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
  return bitmap;
}

std::vector<uint8_t> Bytes(const SkBitmap& bitmap) {
  const base::span<const uint8_t> span = gfx::SkPixmapToSpan(bitmap.pixmap());
  return std::vector<uint8_t>(span.begin(), span.end());
}

TEST_F(MoltFingerprintingTest, OffUntilConfigured) {
  EXPECT_EQ(GetMode(), Mode::kOff);
  EXPECT_FALSE(IsUniform());
  EXPECT_FALSE(FarblerFor(nullptr).has_value());
  EXPECT_EQ(HardwareConcurrency(nullptr, 8), 8u);
  EXPECT_FALSE(UniformScreenRect(nullptr).has_value());
  EXPECT_FALSE(UniformScreenPoint(nullptr, gfx::PointF(10, 20)).has_value());
}

TEST_F(MoltFingerprintingTest, WrongSizedSeedKeepsProtectionOff) {
  const std::array<uint8_t, 16> short_seed = {};
  Configure(Mode::kFarble, short_seed);
  EXPECT_EQ(GetMode(), Mode::kOff);
}

TEST_F(MoltFingerprintingTest, OnlyTheFirstConfigureCounts) {
  const std::array<uint8_t, kSessionSeedSize> seed = {1, 2, 3};
  Configure(Mode::kFarble, seed);
  EXPECT_EQ(GetMode(), Mode::kFarble);
  Configure(Mode::kOff, seed);
  EXPECT_EQ(GetMode(), Mode::kFarble);
  // Without an execution context there is no site, so nothing is farbled.
  EXPECT_FALSE(FarblerFor(nullptr).has_value());
  EXPECT_EQ(HardwareConcurrency(nullptr, 8), 8u);
  EXPECT_FALSE(UniformScreenRect(nullptr).has_value());
}

TEST_F(MoltFingerprintingTest, PixmapFarblingMatchesFarblerInBothOrders) {
  const Farbler farbler(TestKey());
  for (SkColorType type : {kRGBA_8888_SkColorType, kBGRA_8888_SkColorType}) {
    SkBitmap bitmap = BusyBitmap(type, kUnpremul_SkAlphaType, 0);
    std::vector<uint8_t> expected = Bytes(bitmap);
    farbler.FarbleRGBA8(expected, type == kRGBA_8888_SkColorType
                                      ? ChannelOrder::kRGBA
                                      : ChannelOrder::kBGRA);
    FarblePixmapWith(farbler, bitmap.pixmap());
    EXPECT_EQ(Bytes(bitmap), expected);
  }
}

TEST_F(MoltFingerprintingTest, PixmapRowPaddingIsNeverTouched) {
  const Farbler farbler(TestKey());
  constexpr size_t kRowBytes = 48 * 4 + 12;
  SkBitmap bitmap =
      BusyBitmap(kRGBA_8888_SkColorType, kUnpremul_SkAlphaType, kRowBytes);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  const std::vector<uint8_t> after = Bytes(bitmap);
  ASSERT_EQ(before.size(), after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    if (i % kRowBytes >= 48 * 4) {
      EXPECT_EQ(before[i], after[i]) << "padding byte " << i;
    }
  }
  EXPECT_NE(before, after);
}

TEST_F(MoltFingerprintingTest, PremultipliedPixmapsAreLeftAlone) {
  const Farbler farbler(TestKey());
  SkBitmap bitmap = BusyBitmap(kRGBA_8888_SkColorType, kPremul_SkAlphaType, 0);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  EXPECT_EQ(Bytes(bitmap), before);
}

TEST_F(MoltFingerprintingTest, UnsupportedFormatsAreLeftAlone) {
  const Farbler farbler(TestKey());
  SkBitmap bitmap = BusyBitmap(kRGB_565_SkColorType, kOpaque_SkAlphaType, 0);
  const std::vector<uint8_t> before = Bytes(bitmap);
  FarblePixmapWith(farbler, bitmap.pixmap());
  EXPECT_EQ(Bytes(bitmap), before);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
