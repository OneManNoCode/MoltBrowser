// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/llvm-libc/molt_portable_math.h"
#include "third_party/skia/include/core/SkContourMeasure.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/skia/include/core/SkMatrix.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkPathEffect.h"
#include "third_party/skia/include/core/SkPathUtils.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "third_party/skia/include/core/SkScalar.h"
#include "third_party/skia/include/effects/SkDashPathEffect.h"
#include "third_party/skia/include/pathops/SkPathOps.h"
#include "third_party/skia/include/private/SkMoltBrowserMath.h"

namespace {

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

class SkMoltBrowserMathTest : public testing::Test {
 protected:
  void TearDown() override { SkMoltBrowserMath::SetPortable(false); }
};

constexpr double kArgs[] = {0.0,   -0.0, 1e-8,  0.1,    0.5235987755982988,
                            0.75,  1.0,  2.0,   3.0,    6.28318,
                            9.628, 0.3,  -0.37, -1.0,   123.456,
                            1e30,  -7.7, 44100, 0.0001, 0.999};

// Off (the default), every function and every SkScalar macro is the C
// library's, bit for bit: normal windows draw and measure what they did.
TEST_F(SkMoltBrowserMathTest, OffIsTheCLibrary) {
  EXPECT_FALSE(SkMoltBrowserMath::IsPortable());
  for (double x : kArgs) {
    const double a = std::fabs(x);
    EXPECT_EQ(Bits(SkMoltBrowserMath::Sin(x)), Bits(std::sin(x))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Cos(x)), Bits(std::cos(x))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Tan(x)), Bits(std::tan(x))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Acos(x / 1e3)), Bits(std::acos(x / 1e3)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Atan2(x, 3.0)),
              Bits(std::atan2(x, 3.0)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Exp(-a)), Bits(std::exp(-a))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Exp2(-a)), Bits(std::exp2(-a))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Log(a)), Bits(std::log(a))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Log2(a)), Bits(std::log2(a))) << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Pow(a, 2.4)), Bits(std::pow(a, 2.4)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Cbrt(x)), Bits(std::cbrt(x))) << x;
    const float f = static_cast<float>(x);
    const float af = std::fabs(f);
    EXPECT_EQ(Bits(SkScalarSin(f)), Bits(static_cast<float>(std::sin(f)))) << x;
    EXPECT_EQ(Bits(SkScalarCos(f)), Bits(static_cast<float>(std::cos(f)))) << x;
    EXPECT_EQ(Bits(SkScalarTan(f)), Bits(static_cast<float>(std::tan(f)))) << x;
    EXPECT_EQ(Bits(SkScalarATan2(f, 3.f)),
              Bits(static_cast<float>(std::atan2(f, 3.f))))
        << x;
    EXPECT_EQ(Bits(SkScalarExp(-af)), Bits(static_cast<float>(std::exp(-af))))
        << x;
    EXPECT_EQ(Bits(SkScalarLog2(af)), Bits(static_cast<float>(std::log2(af))))
        << x;
    EXPECT_EQ(Bits(SkScalarPow(af, 0.3333333f)),
              Bits(std::pow(af, 0.3333333f)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Cbrt(f)), Bits(std::cbrt(f))) << x;
  }
  // The C++ library's promotions: integers, and float with double, compute
  // in double.
  EXPECT_EQ(Bits(SkMoltBrowserMath::Sin(1)), Bits(std::sin(1)));
  EXPECT_EQ(Bits(SkMoltBrowserMath::Pow(2.0f, 3)), Bits(std::pow(2.0f, 3)));
  EXPECT_EQ(Bits(SkMoltBrowserMath::Pow(2, 0.5f)), Bits(std::pow(2, 0.5f)));
  EXPECT_EQ(Bits(SkScalarSin(1)), Bits(static_cast<float>(std::sin(1))));
}

// SkGraphics::MoltBrowserUsePortableMath() turns it on, and then every
// function is molt_portable_math's.
TEST_F(SkMoltBrowserMathTest, OnIsPortable) {
  SkGraphics::MoltBrowserUsePortableMath();
  EXPECT_TRUE(SkMoltBrowserMath::IsPortable());
  for (double x : kArgs) {
    const double a = std::fabs(x);
    EXPECT_EQ(Bits(SkMoltBrowserMath::Sin(x)),
              Bits(molt_portable_math::Sin(x)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Acos(x / 1e3)),
              Bits(molt_portable_math::Acos(x / 1e3)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Atan2(x, 3.0)),
              Bits(molt_portable_math::Atan2(x, 3.0)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Exp2(-a)),
              Bits(molt_portable_math::Exp2(-a)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Pow(a, 2.4)),
              Bits(molt_portable_math::Pow(a, 2.4)))
        << x;
    EXPECT_EQ(Bits(SkMoltBrowserMath::Cbrt(x)),
              Bits(molt_portable_math::Cbrt(x)))
        << x;
    const float f = static_cast<float>(x);
    const float af = std::fabs(f);
    EXPECT_EQ(Bits(SkScalarSin(f)), Bits(molt_portable_math::Sin(f))) << x;
    EXPECT_EQ(Bits(SkScalarCos(f)), Bits(molt_portable_math::Cos(f))) << x;
    EXPECT_EQ(Bits(SkScalarTan(f)), Bits(molt_portable_math::Tan(f))) << x;
    EXPECT_EQ(Bits(SkScalarACos(f / 1e3f)),
              Bits(molt_portable_math::Acos(f / 1e3f)))
        << x;
    EXPECT_EQ(Bits(SkScalarATan2(f, 3.f)),
              Bits(molt_portable_math::Atan2(f, 3.f)))
        << x;
    EXPECT_EQ(Bits(SkScalarExp(-af)), Bits(molt_portable_math::Exp(-af)))
        << x;
    EXPECT_EQ(Bits(SkScalarLog(af)), Bits(molt_portable_math::Log(af))) << x;
    EXPECT_EQ(Bits(SkScalarPow(af, 0.3333333f)),
              Bits(molt_portable_math::Pow(af, 0.3333333f)))
        << x;
    // No float cube root in the library: the double one, rounded.
    EXPECT_EQ(Bits(SkMoltBrowserMath::Cbrt(f)),
              Bits(static_cast<float>(
                  molt_portable_math::Cbrt(static_cast<double>(f)))))
        << x;
  }
}

// FNV-1a over what Skia computed.
struct Hash {
  uint64_t h = 0xcbf29ce484222325ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 0x100000001b3ull;
    }
  }
  void F(float x) { Bytes(&x, sizeof(x)); }
  void I(int x) { Bytes(&x, sizeof(x)); }
  void Path(const SkPath& path) {
    SkPath::RawIter iter(path);
    SkPoint pts[4];
    SkPath::Verb verb;
    while ((verb = iter.next(pts)) != SkPath::kDone_Verb) {
      I(verb);
      int n = 0;
      switch (verb) {
        case SkPath::kMove_Verb:
          n = 1;
          break;
        case SkPath::kLine_Verb:
          n = 2;
          break;
        case SkPath::kQuad_Verb:
          n = 3;
          break;
        case SkPath::kConic_Verb:
          n = 3;
          F(iter.conicWeight());
          break;
        case SkPath::kCubic_Verb:
          n = 4;
          break;
        default:
          break;
      }
      for (int k = 0; k < n; ++k) {
        F(pts[k].fX);
        F(pts[k].fY);
      }
    }
  }
};

SkPath Arcs() {
  SkPathBuilder b;
  for (int i = 0; i < 120; ++i) {
    const float start = -720.f + i * 3.712345f;
    const float sweep = -359.f + i * 1.79123f;
    const SkRect oval = SkRect::MakeXYWH(i * 0.37f, i * 0.11f, 30 + i * 0.7f,
                                         20 + i * 1.3f);
    b.arcTo(oval, start, sweep, i % 3 == 0);
  }
  return b.detach();
}

SkPath SvgArcs() {
  SkPathBuilder b;
  b.moveTo(10, 10);
  for (int i = 0; i < 100; ++i) {
    b.arcTo({17.f + i * 0.31f, 9.f + i * 0.17f}, i * 7.3f - 300.f,
            i % 2 ? SkPathBuilder::kLarge_ArcSize
                  : SkPathBuilder::kSmall_ArcSize,
            i % 3 ? SkPathDirection::kCW : SkPathDirection::kCCW,
            {10.f + std::fmod(i * 13.37f, 200.f),
             10.f + std::fmod(i * 7.77f, 150.f)});
  }
  return b.detach();
}

SkPath Curvy() {
  SkPathBuilder b;
  b.moveTo(5, 80);
  for (int i = 0; i < 60; ++i) {
    const float x = 5 + i * 4.1f;
    b.cubicTo(x + 13.7f, -40 + i * 1.3f, x - 21.1f, 170 - i * 0.7f, x + 3.9f,
              80 + std::fmod(i * 17.3f, 40.f) - 20);
    b.conicTo(x + 9.f, 30.f + i, x + 4.f, 100.f - i * 0.5f, 0.3f + i * 0.04f);
  }
  b.close();
  b.addOval(SkRect::MakeXYWH(40, 30, 77.7f, 41.3f));
  b.addRRect(
      SkRRect::MakeRectXY(SkRect::MakeXYWH(120, 20, 60, 50), 13.3f, 7.1f));
  return b.detach();
}

// With the portable functions, the arcs, strokes, lengths and path
// operations behind isPointInPath(), isPointInStroke(), getTotalLength() and
// hit testing come out the same on every CPU and OS: these hashes were
// computed on x86-64 CPUs from Core 2 to Zen 2 and on arm64, and must match
// on every platform MoltBrowser builds for.
TEST_F(SkMoltBrowserMathTest, PortableGeometryIsTheSameEverywhere) {
  SkMoltBrowserMath::SetPortable(true);
  const SkPath arcs = Arcs();
  const SkPath svg = SvgArcs();
  const SkPath curvy = Curvy();
  Hash h_arcs;
  h_arcs.Path(arcs);
  EXPECT_EQ(h_arcs.h, 0xd8fc36bcadb2efdbull);
  Hash h_svg;
  h_svg.Path(svg);
  EXPECT_EQ(h_svg.h, 0x9dc29919797de779ull);

  Hash h_rotate;
  for (int i = 0; i < 2000; ++i) {
    SkMatrix m;
    m.setRotate(i * 0.4567f - 400.f, 3.f, 7.f);
    float v[9];
    m.get9(v);
    h_rotate.Bytes(v, sizeof(v));
  }
  EXPECT_EQ(h_rotate.h, 0x6d1c3ff2a0d4ff01ull);

  Hash h_contains;
  for (const SkPath* p : {&arcs, &svg, &curvy}) {
    const SkRect r = p->computeTightBounds();
    h_contains.Bytes(&r, sizeof(r));
    for (float y = r.fTop; y < r.fBottom; y += r.height() / 41.f) {
      for (float x = r.fLeft; x < r.fRight; x += r.width() / 37.f) {
        h_contains.I(p->contains(x, y));
      }
    }
  }
  EXPECT_EQ(h_contains.h, 0x28b119f6211e6535ull);

  Hash h_stroke;
  Hash h_stroke_contains;
  int k = 0;
  for (SkPaint::Join join :
       {SkPaint::kMiter_Join, SkPaint::kRound_Join, SkPaint::kBevel_Join}) {
    for (SkPaint::Cap cap :
         {SkPaint::kButt_Cap, SkPaint::kRound_Cap, SkPaint::kSquare_Cap}) {
      for (bool dash : {false, true}) {
        SkPaint paint;
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(3.3f + k * 0.7f);
        paint.setStrokeJoin(join);
        paint.setStrokeCap(cap);
        paint.setStrokeMiter(4.f + k);
        if (dash) {
          const float intervals[] = {7.7f, 3.1f, 1.3f, 5.5f};
          paint.setPathEffect(SkDashPathEffect::Make(intervals, 1.7f));
        }
        for (const SkPath* p : {&svg, &curvy}) {
          SkPathBuilder out;
          skpathutils::FillPathWithPaint(*p, paint, &out, nullptr,
                                         SkMatrix::Scale(1.3f, 1.3f));
          const SkPath s = out.detach();
          h_stroke.Path(s);
          const SkRect r = s.computeTightBounds();
          h_stroke_contains.Bytes(&r, sizeof(r));
          for (float y = r.fTop; y < r.fBottom; y += r.height() / 17.f) {
            for (float x = r.fLeft; x < r.fRight; x += r.width() / 19.f) {
              h_stroke_contains.I(s.contains(x, y));
            }
          }
        }
        ++k;
      }
    }
  }
  EXPECT_EQ(h_stroke.h, 0x88723727233f361eull);
  EXPECT_EQ(h_stroke_contains.h, 0x1fe32bf943b96cf8ull);

  Hash h_measure;
  for (const SkPath* p : {&arcs, &svg, &curvy}) {
    SkContourMeasureIter iter(*p, false, 1.f);
    while (sk_sp<SkContourMeasure> m = iter.next()) {
      h_measure.F(m->length());
      for (int i = 0; i <= 16; ++i) {
        SkPoint pos;
        SkVector tan;
        if (m->getPosTan(m->length() * i / 16.f, &pos, &tan)) {
          h_measure.F(pos.fX);
          h_measure.F(pos.fY);
          h_measure.F(tan.fX);
          h_measure.F(tan.fY);
        }
      }
    }
  }
  EXPECT_EQ(h_measure.h, 0x6bced3981b01a26aull);

  Hash h_ops;
  SkPathBuilder quad;
  quad.moveTo(30, 20)
      .lineTo(170.3f, 35.1f)
      .lineTo(150.7f, 130.9f)
      .lineTo(20.2f, 110.f)
      .close();
  const SkPath q = quad.detach();
  for (SkPathOp op : {kIntersect_SkPathOp, kDifference_SkPathOp,
                      kUnion_SkPathOp, kXOR_SkPathOp}) {
    SkPath result;
    if (Op(curvy, q, op, &result)) {
      h_ops.Path(result);
    } else {
      h_ops.I(-1);
    }
    SkPath result2;
    if (Op(svg, curvy, op, &result2)) {
      h_ops.Path(result2);
    } else {
      h_ops.I(-2);
    }
  }
  EXPECT_EQ(h_ops.h, 0x435c6eaec356bb45ull);
}

}  // namespace
