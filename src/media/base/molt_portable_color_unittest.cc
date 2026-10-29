// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/base/molt_portable_color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace media::molt {
namespace {

constexpr YuvMatrix kMatrices[] = {YuvMatrix::kBt601, YuvMatrix::kBt709,
                                   YuvMatrix::kFcc, YuvMatrix::kSmpte240m,
                                   YuvMatrix::kBt2020};
constexpr YuvRange kRanges[] = {YuvRange::kLimited, YuvRange::kFull};

// ITU-T H.273's Kr and Kb.
std::pair<double, double> KrKb(YuvMatrix matrix) {
  switch (matrix) {
    case YuvMatrix::kBt601:
      return {0.299, 0.114};
    case YuvMatrix::kBt709:
      return {0.2126, 0.0722};
    case YuvMatrix::kFcc:
      return {0.30, 0.11};
    case YuvMatrix::kSmpte240m:
      return {0.212, 0.087};
    case YuvMatrix::kBt2020:
      return {0.2627, 0.0593};
  }
  return {0.299, 0.114};
}

// An 8-bit sample's offset and steps (luma, chroma).
struct RangeScale {
  double y_offset;
  double y_steps;
  double c_steps;
};

RangeScale Scale(YuvRange range) {
  return range == YuvRange::kLimited ? RangeScale{16, 219, 224}
                                     : RangeScale{0, 255, 255};
}

struct Yuv {
  double y;
  double u;
  double v;
};

// R'G'B' (0 to 1) as exact (unrounded) samples of `encoding`.
Yuv Encode(double r, double g, double b, YuvEncoding encoding) {
  const auto [kr, kb] = KrKb(encoding.matrix);
  const double y = kr * r + (1 - kr - kb) * g + kb * b;
  const double cb = (b - y) / (2 * (1 - kb));
  const double cr = (r - y) / (2 * (1 - kr));
  const RangeScale s = Scale(encoding.range);
  return {s.y_offset + s.y_steps * y, 128 + s.c_steps * cb,
          128 + s.c_steps * cr};
}

// The exact map's column for one unit of the source's luma (0), U (1) or
// V (2): the change of the target's luma, U and V.
Yuv Column(YuvEncoding from, int unit) {
  const auto [kr, kb] = KrKb(from.matrix);
  const RangeScale s = Scale(from.range);
  const double y = unit == 0 ? 1 / s.y_steps : 0;
  const double cb = unit == 1 ? 1 / s.c_steps : 0;
  const double cr = unit == 2 ? 1 / s.c_steps : 0;
  const double r = y + 2 * (1 - kr) * cr;
  const double b = y + 2 * (1 - kb) * cb;
  const double g = (y - kr * r - kb * b) / (1 - kr - kb);
  const Yuv out = Encode(r, g, b, kPortableCameraEncoding);
  return {out.y - 16, out.u - 128, out.v - 128};
}

int32_t Q14(double value) {
  return static_cast<int32_t>(std::lround(value * 16384));
}

// The table is the exact map, rounded: luma from luma alone is the ratio of
// the ranges' steps, chroma doesn't depend on luma.
TEST(MoltPortableColorTest, CoefficientsAreTheExactMapRounded) {
  for (YuvMatrix matrix : kMatrices) {
    for (YuvRange range : kRanges) {
      const YuvEncoding from = {matrix, range};
      const YuvToPortableCameraCoefficients c =
          PortableCameraCoefficients(from);
      const Yuv luma = Column(from, 0);
      const Yuv cb = Column(from, 1);
      const Yuv cr = Column(from, 2);
      EXPECT_NEAR(luma.u, 0.0, 1e-12);
      EXPECT_NEAR(luma.v, 0.0, 1e-12);
      EXPECT_EQ(c.y_offset, range == YuvRange::kLimited ? 16 : 0);
      EXPECT_EQ(c.y_y, Q14(luma.y));
      EXPECT_EQ(c.y_u, Q14(cb.y));
      EXPECT_EQ(c.y_v, Q14(cr.y));
      EXPECT_EQ(c.u_u, Q14(cb.u));
      EXPECT_EQ(c.u_v, Q14(cr.u));
      EXPECT_EQ(c.v_u, Q14(cb.v));
      EXPECT_EQ(c.v_v, Q14(cr.v));
    }
  }
  const YuvToPortableCameraCoefficients identity =
      PortableCameraCoefficients(kPortableCameraEncoding);
  EXPECT_EQ(identity.y_y, YuvToPortableCameraCoefficients::kOne);
  EXPECT_EQ(identity.u_u, YuvToPortableCameraCoefficients::kOne);
  EXPECT_EQ(identity.v_v, YuvToPortableCameraCoefficients::kOne);
  EXPECT_EQ(identity.y_u | identity.y_v | identity.u_v | identity.v_u, 0);
}

// A 4:2:0 frame's planes.
struct Planes {
  int width;
  int height;
  size_t y_stride;
  size_t c_stride;
  std::vector<uint8_t> y;
  std::vector<uint8_t> u;
  std::vector<uint8_t> v;

  Planes(int w, int h, size_t padding = 0)
      : width(w),
        height(h),
        y_stride(static_cast<size_t>(w) + padding),
        c_stride(static_cast<size_t>((w + 1) / 2) + padding),
        y(y_stride * static_cast<size_t>(h), 0),
        u(c_stride * static_cast<size_t>((h + 1) / 2), 128),
        v(c_stride * static_cast<size_t>((h + 1) / 2), 128) {}

  bool Convert(YuvEncoding from) {
    return ConvertI420ToPortableCameraEncoding(y, y_stride, u, c_stride, v,
                                               c_stride, width, height, from);
  }
};

uint8_t ToSample(double value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

// The same frame in BT.601 at limited range is unchanged.
TEST(MoltPortableColorTest, PortableEncodingIsUnchanged) {
  Planes frame(6, 4);
  for (size_t i = 0; i < frame.y.size(); ++i) {
    frame.y[i] = static_cast<uint8_t>(i * 37);
  }
  for (size_t i = 0; i < frame.u.size(); ++i) {
    frame.u[i] = static_cast<uint8_t>(i * 91);
    frame.v[i] = static_cast<uint8_t>(255 - i * 53);
  }
  const Planes before = frame;
  ASSERT_TRUE(frame.Convert(kPortableCameraEncoding));
  EXPECT_EQ(frame.y, before.y);
  EXPECT_EQ(frame.u, before.u);
  EXPECT_EQ(frame.v, before.v);
}

// Greys stay grey, and full range maps onto limited range's 16 to 235.
TEST(MoltPortableColorTest, GreysStayGrey) {
  for (YuvMatrix matrix : kMatrices) {
    for (YuvRange range : kRanges) {
      Planes frame(2, 2);
      for (int luma = 0; luma <= 255; ++luma) {
        std::fill(frame.y.begin(), frame.y.end(), static_cast<uint8_t>(luma));
        std::fill(frame.u.begin(), frame.u.end(), uint8_t{128});
        std::fill(frame.v.begin(), frame.v.end(), uint8_t{128});
        ASSERT_TRUE(frame.Convert({matrix, range}));
        const RangeScale s = Scale(range);
        const uint8_t want =
            ToSample(16 + 219 * ((luma - s.y_offset) / s.y_steps));
        EXPECT_EQ(frame.y[0], want) << luma;
        EXPECT_EQ(frame.u[0], 128);
        EXPECT_EQ(frame.v[0], 128);
      }
    }
  }
  Planes black(2, 2);
  ASSERT_TRUE(black.Convert({YuvMatrix::kBt601, YuvRange::kFull}));
  EXPECT_EQ(black.y[0], 16);
  Planes white(2, 2);
  std::fill(white.y.begin(), white.y.end(), uint8_t{255});
  ASSERT_TRUE(white.Convert({YuvMatrix::kBt601, YuvRange::kFull}));
  EXPECT_EQ(white.y[0], 235);
}

// A colour encoded in any encoding and converted is, to a sample, the same
// colour encoded in BT.601 at limited range.
TEST(MoltPortableColorTest, ColoursConvert) {
  uint32_t seed = 2026;
  auto next = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0;
  };
  for (YuvMatrix matrix : kMatrices) {
    for (YuvRange range : kRanges) {
      const YuvEncoding from = {matrix, range};
      for (int i = 0; i < 2000; ++i) {
        const double r = next();
        const double g = next();
        const double b = next();
        const Yuv src = Encode(r, g, b, from);
        const Yuv want = Encode(r, g, b, kPortableCameraEncoding);
        Planes frame(2, 2);
        std::fill(frame.y.begin(), frame.y.end(), ToSample(src.y));
        std::fill(frame.u.begin(), frame.u.end(), ToSample(src.u));
        std::fill(frame.v.begin(), frame.v.end(), ToSample(src.v));
        ASSERT_TRUE(frame.Convert(from));
        // The source's rounding to samples moves the result by up to about a
        // sample and a half; the conversion itself adds half a sample.
        EXPECT_NEAR(frame.y[3], want.y, 2.0);
        EXPECT_NEAR(frame.u[0], want.u, 2.0);
        EXPECT_NEAR(frame.v[0], want.v, 2.0);
      }
    }
  }
}

// Each luma sample takes its 2x2 block's chroma; odd sizes have a last
// column and row of blocks with fewer luma samples.
TEST(MoltPortableColorTest, BlocksTakeTheirChroma) {
  Planes frame(3, 3);
  std::fill(frame.y.begin(), frame.y.end(), uint8_t{100});
  // Blocks: (0,0) neutral, (1,0) blue-ish, (0,1) red-ish, (1,1) both.
  frame.u = {128, 200, 128, 200};
  frame.v = {128, 128, 200, 200};
  ASSERT_TRUE(frame.Convert({YuvMatrix::kBt709, YuvRange::kLimited}));
  const YuvToPortableCameraCoefficients c =
      PortableCameraCoefficients({YuvMatrix::kBt709, YuvRange::kLimited});
  auto luma = [&c](int u, int v) {
    return 16 + ((c.y_y * 84 + c.y_u * (u - 128) + c.y_v * (v - 128) + 8192) >>
                 14);
  };
  const int want[3][3] = {
      {luma(128, 128), luma(128, 128), luma(200, 128)},
      {luma(128, 128), luma(128, 128), luma(200, 128)},
      {luma(128, 200), luma(128, 200), luma(200, 200)},
  };
  for (int row = 0; row < 3; ++row) {
    for (int x = 0; x < 3; ++x) {
      EXPECT_EQ(frame.y[static_cast<size_t>(row * 3 + x)], want[row][x])
          << row << "," << x;
    }
  }
  EXPECT_EQ(frame.u[0], 128);
  EXPECT_EQ(frame.v[0], 128);
}

// Strides longer than the rows: the padding is left alone.
TEST(MoltPortableColorTest, StridesAndPadding) {
  Planes frame(4, 2, /*padding=*/3);
  std::fill(frame.y.begin(), frame.y.end(), uint8_t{7});
  std::fill(frame.u.begin(), frame.u.end(), uint8_t{9});
  std::fill(frame.v.begin(), frame.v.end(), uint8_t{11});
  ASSERT_TRUE(frame.Convert({YuvMatrix::kBt601, YuvRange::kFull}));
  // 16 + 0.8588 * 7 = 22.01.
  for (size_t row = 0; row < 2; ++row) {
    for (size_t x = 0; x < frame.y_stride; ++x) {
      const uint8_t value = frame.y[row * frame.y_stride + x];
      EXPECT_EQ(value, x < 4 ? 22 : 7) << row << "," << x;
    }
  }
  // 128 + 0.8784 * (9 - 128) = 23.47, 128 + 0.8784 * (11 - 128) = 25.22.
  for (size_t x = 0; x < frame.c_stride; ++x) {
    EXPECT_EQ(frame.u[x], x < 2 ? 23 : 9) << x;
    EXPECT_EQ(frame.v[x], x < 2 ? 25 : 11) << x;
  }
}

// Bad geometry: nothing written.
TEST(MoltPortableColorTest, BadGeometry) {
  const YuvEncoding jpeg = {YuvMatrix::kBt601, YuvRange::kFull};
  Planes frame(4, 4);
  std::fill(frame.y.begin(), frame.y.end(), uint8_t{0});
  const Planes before = frame;
  auto unchanged = [&] {
    return frame.y == before.y && frame.u == before.u && frame.v == before.v;
  };
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 4, frame.u, 2, frame.v, 2, 0, 4, jpeg));
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 4, frame.u, 2, frame.v, 2, 4, -1, jpeg));
  // A stride shorter than the row.
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 3, frame.u, 2, frame.v, 2, 4, 4, jpeg));
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 4, frame.u, 1, frame.v, 2, 4, 4, jpeg));
  // Spans too short for their rows.
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      std::span<uint8_t>(frame.y).first(15), 4, frame.u, 2, frame.v, 2, 4, 4,
      jpeg));
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 4, frame.u, 2, std::span<uint8_t>(frame.v).first(3), 2, 4, 4,
      jpeg));
  // Strides so large that the rows overflow.
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, SIZE_MAX / 2, frame.u, 2, frame.v, 2, 4, 4, jpeg));
  EXPECT_FALSE(ConvertI420ToPortableCameraEncoding(
      frame.y, 4, frame.u, 2, frame.v, 2, 1 << 17, 4, jpeg));
  EXPECT_TRUE(unchanged());
}

// The bytes of a frame converted from BT.709 at limited range and from
// full-range BT.601 (JPEG): the same on every CPU.
TEST(MoltPortableColorTest, GoldenDigests) {
  auto digest = [](YuvEncoding from) {
    Planes frame(64, 48);
    uint32_t seed = 7;
    auto next = [&seed] {
      seed = seed * 1664525u + 1013904223u;
      return static_cast<uint8_t>(seed >> 24);
    };
    for (uint8_t& s : frame.y) {
      s = next();
    }
    for (size_t i = 0; i < frame.u.size(); ++i) {
      frame.u[i] = next();
      frame.v[i] = next();
    }
    EXPECT_TRUE(frame.Convert(from));
    uint64_t hash = 14695981039346656037ull;
    for (const std::vector<uint8_t>* plane : {&frame.y, &frame.u, &frame.v}) {
      for (uint8_t s : *plane) {
        hash = (hash ^ s) * 1099511628211ull;
      }
    }
    return hash;
  };
  EXPECT_EQ(digest({YuvMatrix::kBt709, YuvRange::kLimited}),
            0xe15b00f11b1bc889ull);
  EXPECT_EQ(digest({YuvMatrix::kBt601, YuvRange::kFull}),
            0x77af15d6aa5bf344ull);
}

}  // namespace
}  // namespace media::molt
