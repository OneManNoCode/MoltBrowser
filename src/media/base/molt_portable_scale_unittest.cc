// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_scale.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace media::molt {
namespace {

uint64_t Fnv(std::span<const uint8_t> bytes) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t b : bytes) {
    h ^= b;
    h *= 1099511628211ull;
  }
  return h;
}

template <typename T>
std::vector<T> Noise(size_t n, uint32_t seed, uint32_t max) {
  std::vector<T> v(n);
  uint32_t s = seed;
  for (T& x : v) {
    s = s * 1664525u + 1013904223u;
    x = static_cast<T>((s >> 8) % (max + 1));
  }
  return v;
}

std::vector<uint8_t> Scale8(const std::vector<uint8_t>& src,
                            int sw,
                            int sh,
                            int dw,
                            int dh,
                            int ch) {
  std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * ch, 0xee);
  EXPECT_TRUE(PortableScalePlane(src, static_cast<size_t>(sw) * ch, sw, sh,
                                 dst, static_cast<size_t>(dw) * ch, dw, dh,
                                 ch));
  return dst;
}

TEST(MoltPortableScaleTest, AxisWeightsSumToOneAndStayInRange) {
  std::vector<std::pair<int, int>> sizes;
  for (int s = 1; s <= 40; ++s) {
    for (int d = 1; d <= 40; ++d) {
      sizes.emplace_back(s, d);
    }
  }
  sizes.insert(sizes.end(), {{1920, 640}, {640, 1920}, {1080, 719},
                             {3840, 7}, {7, 3840}, {16384, 1}, {1, 16384}});
  for (auto [s, d] : sizes) {
    SCOPED_TRACE(testing::Message() << s << " -> " << d);
    const PortableScaleAxis a = PortableScaleAxis::Make(s, d);
    ASSERT_EQ(a.first.size(), static_cast<size_t>(d));
    int last_first = 0;
    int last_end = 0;
    for (size_t o = 0; o < a.first.size(); ++o) {
      ASSERT_GE(a.count[o], 1u);
      int32_t sum = 0;
      for (uint32_t k = 0; k < a.count[o]; ++k) {
        const int32_t w = a.weights[a.offset[o] + k];
        EXPECT_GE(w, 0);
        sum += w;
      }
      EXPECT_EQ(sum, PortableScaleAxis::kOne);
      EXPECT_GE(a.first[o], 0);
      const int end = a.first[o] + static_cast<int>(a.count[o]);
      EXPECT_LE(end, s);
      // Both ends only move forward (the scaler's row window relies on it).
      EXPECT_GE(a.first[o], last_first);
      EXPECT_GE(end, last_end);
      last_first = a.first[o];
      last_end = end;
    }
  }
  EXPECT_TRUE(PortableScaleAxis::Make(0, 5).first.empty());
  EXPECT_TRUE(PortableScaleAxis::Make(5, -1).first.empty());
}

TEST(MoltPortableScaleTest, SameSizeCopies) {
  for (int ch = 1; ch <= 4; ++ch) {
    const int w = 13;
    const int h = 7;
    const size_t stride = static_cast<size_t>(w) * ch + 5;
    auto src = Noise<uint8_t>(stride * h, 7u + ch, 255);
    std::vector<uint8_t> dst(stride * h, 0);
    ASSERT_TRUE(PortableScalePlane(src, stride, w, h, dst, stride, w, h, ch));
    for (int y = 0; y < h; ++y) {
      for (size_t i = 0; i < static_cast<size_t>(w) * ch; ++i) {
        EXPECT_EQ(dst[y * stride + i], src[y * stride + i]);
      }
      // Padding past the row is never written.
      for (size_t i = static_cast<size_t>(w) * ch; i < stride; ++i) {
        EXPECT_EQ(dst[y * stride + i], 0);
      }
    }
    auto src16 = Noise<uint16_t>(stride * h, 9u + ch, 65535);
    std::vector<uint16_t> dst16(stride * h, 0);
    ASSERT_TRUE(
        PortableScalePlane16(src16, stride, w, h, dst16, stride, w, h, ch));
    for (int y = 0; y < h; ++y) {
      for (size_t i = 0; i < static_cast<size_t>(w) * ch; ++i) {
        EXPECT_EQ(dst16[y * stride + i], src16[y * stride + i]);
      }
    }
  }
}

TEST(MoltPortableScaleTest, ConstantPlanesStayConstant) {
  const std::pair<int, int> sizes[] = {{1, 1},  {2, 3},   {7, 5},  {16, 9},
                                       {33, 1}, {1, 33},  {64, 48}, {5, 64}};
  for (auto [sw, sh] : sizes) {
    for (auto [dw, dh] : sizes) {
      for (int ch : {1, 2, 4}) {
        for (uint8_t value : {uint8_t{0}, uint8_t{1}, uint8_t{128},
                              uint8_t{255}}) {
          std::vector<uint8_t> src(static_cast<size_t>(sw) * sh * ch, value);
          auto dst = Scale8(src, sw, sh, dw, dh, ch);
          EXPECT_TRUE(std::ranges::all_of(dst, [&](uint8_t v) {
            return v == value;
          })) << sw << "x" << sh << " -> " << dw << "x" << dh << " ch " << ch;
        }
      }
      std::vector<uint16_t> src16(static_cast<size_t>(sw) * sh, 65535);
      std::vector<uint16_t> dst16(static_cast<size_t>(dw) * dh, 0);
      ASSERT_TRUE(
          PortableScalePlane16(src16, sw, sw, sh, dst16, dw, dw, dh, 1));
      EXPECT_TRUE(
          std::ranges::all_of(dst16, [](uint16_t v) { return v == 65535; }));
    }
  }
}

TEST(MoltPortableScaleTest, IntegerFactorDownscaleIsTheRoundedBlockMean) {
  const int sw = 12;
  const int sh = 9;
  auto src = Noise<uint8_t>(static_cast<size_t>(sw) * sh, 3, 255);
  auto dst = Scale8(src, sw, sh, 4, 3, 1);  // 3x3 blocks
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 4; ++x) {
      int sum = 0;
      for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
          sum += src[(y * 3 + j) * sw + x * 3 + i];
        }
      }
      // Q14 weights of 1/3 aren't exact: within one of the exact mean.
      const double mean = sum / 9.0;
      EXPECT_NEAR(dst[y * 4 + x], mean, 0.5 + 1e-9) << x << "," << y;
    }
  }
  // Halving is exact: weights of 1/2 are.
  auto half = Scale8(src, sw, sh - 1, sw / 2, (sh - 1) / 2, 1);
  for (int y = 0; y < (sh - 1) / 2; ++y) {
    for (int x = 0; x < sw / 2; ++x) {
      const int sum = src[2 * y * sw + 2 * x] + src[2 * y * sw + 2 * x + 1] +
                      src[(2 * y + 1) * sw + 2 * x] +
                      src[(2 * y + 1) * sw + 2 * x + 1];
      EXPECT_EQ(half[y * (sw / 2) + x], (sum + 2) / 4);
    }
  }
}

TEST(MoltPortableScaleTest, DoublingInterpolatesWithCentresAligned) {
  // 1-D: [0, 100, 200] doubled. Output centres sit at -0.25, 0.25, 0.75,
  // 1.25, 1.75, 2.25 source samples; the ends replicate.
  const std::vector<uint8_t> src = {0, 100, 200};
  auto dst = Scale8(src, 3, 1, 6, 1, 1);
  EXPECT_EQ(dst, (std::vector<uint8_t>{0, 25, 75, 125, 175, 200}));
  // Vertical the same way.
  auto col = Scale8(src, 1, 3, 1, 6, 1);
  EXPECT_EQ(col, dst);
}

TEST(MoltPortableScaleTest, NeverLeavesTheInputRange) {
  const std::pair<int, int> sizes[] = {{17, 11}, {64, 3}, {3, 64}, {40, 40}};
  uint32_t seed = 1;
  for (auto [sw, sh] : sizes) {
    for (auto [dw, dh] : sizes) {
      auto src = Noise<uint8_t>(static_cast<size_t>(sw) * sh, seed++, 200);
      for (uint8_t& v : src) {
        v = static_cast<uint8_t>(v + 30);  // 30..230
      }
      auto dst = Scale8(src, sw, sh, dw, dh, 1);
      EXPECT_GE(*std::ranges::min_element(dst), 30);
      EXPECT_LE(*std::ranges::max_element(dst), 230);
    }
  }
}

TEST(MoltPortableScaleTest, ChannelsAreIndependent) {
  // Scaling an interleaved RGBA plane equals scaling each channel alone.
  const int sw = 19;
  const int sh = 13;
  const int dw = 31;
  const int dh = 6;
  auto src = Noise<uint8_t>(static_cast<size_t>(sw) * sh * 4, 11, 255);
  auto dst = Scale8(src, sw, sh, dw, dh, 4);
  for (int c = 0; c < 4; ++c) {
    std::vector<uint8_t> plane(static_cast<size_t>(sw) * sh);
    for (size_t i = 0; i < plane.size(); ++i) {
      plane[i] = src[i * 4 + c];
    }
    auto scaled = Scale8(plane, sw, sh, dw, dh, 1);
    for (size_t i = 0; i < scaled.size(); ++i) {
      EXPECT_EQ(dst[i * 4 + c], scaled[i]);
    }
  }
}

TEST(MoltPortableScaleTest, RejectsBadGeometry) {
  std::vector<uint8_t> src(100, 1);
  std::vector<uint8_t> dst(100, 0);
  EXPECT_FALSE(PortableScalePlane(src, 10, 0, 10, dst, 10, 10, 10, 1));
  EXPECT_FALSE(PortableScalePlane(src, 10, 10, 10, dst, 10, 10, -1, 1));
  EXPECT_FALSE(PortableScalePlane(src, 10, 10, 10, dst, 10, 10, 10, 0));
  EXPECT_FALSE(PortableScalePlane(src, 10, 10, 10, dst, 10, 10, 10, 5));
  EXPECT_FALSE(PortableScalePlane(src, 9, 10, 10, dst, 10, 10, 10, 1));
  EXPECT_FALSE(PortableScalePlane(src, 10, 10, 11, dst, 10, 10, 10, 1));
  EXPECT_FALSE(PortableScalePlane(src, 10, 10, 10, dst, 10, 10, 11, 1));
  EXPECT_FALSE(PortableScalePlane(src, 10, 5, 10, dst, 10, 5, 10, 3));
  EXPECT_FALSE(PortableScalePlane(src, SIZE_MAX, 10, 2, dst, 10, 10, 2, 1));
  EXPECT_FALSE(
      PortableScalePlane(src, 10, 1 << 20, 1, dst, 10, 10, 1, 1));
  EXPECT_TRUE(std::ranges::all_of(dst, [](uint8_t v) { return v == 0; }));
  // A short last row is enough: the stride's padding isn't read.
  std::vector<uint8_t> tight(2 * 16 + 10, 7);
  EXPECT_TRUE(PortableScalePlane(tight, 16, 10, 3, dst, 10, 5, 2, 1));
  EXPECT_FALSE(PortableScalePlane(std::span(tight).first(tight.size() - 1), 16,
                                  10, 3, dst, 10, 5, 2, 1));
}

// The scaler's output is part of what MoltNet pages read back (encoded video),
// so it must not change by accident, and it must be the same on every
// machine: these digests are checked on x86-64 and arm64.
TEST(MoltPortableScaleTest, GoldenDigests) {
  struct Case {
    int sw, sh, dw, dh, ch;
    uint64_t digest;
  };
  const Case cases[] = {
      {1920, 1080, 640, 360, 1, 0xe0ae5f2605f2df37},
      {1280, 720, 1920, 1080, 1, 0x6ff2453580cdaca8},
      {640, 480, 427, 731, 2, 0xe2e517df93ed9021},
      {333, 77, 1001, 50, 4, 0x01741c8efd16b941},
      {960, 540, 360, 202, 2, 0xbc7d024661872639},
  };
  uint32_t seed = 100;
  for (const Case& c : cases) {
    auto src = Noise<uint8_t>(static_cast<size_t>(c.sw) * c.sh * c.ch,
                              seed++, 255);
    auto dst = Scale8(src, c.sw, c.sh, c.dw, c.dh, c.ch);
    EXPECT_EQ(Fnv(dst), c.digest) << c.sw << "x" << c.sh << " -> " << c.dw
                                  << "x" << c.dh << " ch " << c.ch;
  }
  auto src16 = Noise<uint16_t>(static_cast<size_t>(1280) * 720, 9, 1023);
  std::vector<uint16_t> dst16(static_cast<size_t>(853) * 480);
  ASSERT_TRUE(
      PortableScalePlane16(src16, 1280, 1280, 720, dst16, 853, 853, 480, 1));
  std::vector<uint8_t> bytes;
  for (uint16_t v : dst16) {
    bytes.push_back(static_cast<uint8_t>(v));
    bytes.push_back(static_cast<uint8_t>(v >> 8));
  }
  EXPECT_EQ(Fnv(bytes), 0xf90eaa6d015d3888u);
}

}  // namespace
}  // namespace media::molt
