// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "ui/gfx/geometry/molt_math.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/llvm-libc/molt_portable_math.h"
#include "ui/gfx/geometry/molt_complex_math.h"
#include "ui/gfx/geometry/sin_cos_degrees.h"

namespace gfx::molt_math {
namespace {

uint64_t Bits(double d) {
  uint64_t u;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

class MoltMathTest : public testing::Test {
 protected:
  void TearDown() override { SetPortable(false); }
};

// Arguments around the ones pages use: angles of a few degrees to a full
// turn in radians, CSS exponents and logarithms, Web Audio's frequencies and
// gains, and values on either side of 0 and 1.
constexpr double kArgs[] = {0.0,     -0.0,      1e-8,    0.1,   0.5235987755982988,
                            0.75,    1.0,       1.0471975511965976,  2.0,   3.0,
                            6.28318, 9.628,     -4.64,   44100, 0.001,
                            -0.3,    -1.0,      1e300,   -7.77, 123.456};

// Off (the default), every function is the C library's, bit for bit: normal
// windows compute what they computed before.
TEST_F(MoltMathTest, OffIsTheCLibrary) {
  EXPECT_FALSE(IsPortable());
  for (double x : kArgs) {
    EXPECT_EQ(Bits(Sin(x)), Bits(std::sin(x))) << x;
    EXPECT_EQ(Bits(Cos(x)), Bits(std::cos(x))) << x;
    EXPECT_EQ(Bits(Tan(x)), Bits(std::tan(x))) << x;
    EXPECT_EQ(Bits(Atan(x)), Bits(std::atan(x))) << x;
    EXPECT_EQ(Bits(Exp(x)), Bits(std::exp(x))) << x;
    EXPECT_EQ(Bits(Log(std::fabs(x))), Bits(std::log(std::fabs(x)))) << x;
    EXPECT_EQ(Bits(Log10(std::fabs(x))), Bits(std::log10(std::fabs(x))))
        << x;
    EXPECT_EQ(Bits(Pow(std::fabs(x), -4.64)),
              Bits(std::pow(std::fabs(x), -4.64)))
        << x;
    EXPECT_EQ(Bits(Atan2(x, 3.0)), Bits(std::atan2(x, 3.0))) << x;
    EXPECT_EQ(Bits(Hypot(x, 4.5)), Bits(std::hypot(x, 4.5))) << x;
    const float f = static_cast<float>(x);
    EXPECT_EQ(Bits(Sin(f)), Bits(std::sin(f))) << x;
    EXPECT_EQ(Bits(Pow(std::fabs(f), 0.7f)), Bits(std::pow(std::fabs(f), 0.7f)))
        << x;
    EXPECT_EQ(Bits(Hypot(f, 2.5f)), Bits(std::hypot(f, 2.5f))) << x;
  }
  // The C++ library's promotions: integers and mixed float and double
  // arguments compute in double.
  EXPECT_EQ(Bits(Sin(1)), Bits(std::sin(1)));
  EXPECT_EQ(Bits(Pow(2.0f, 0.3)), Bits(std::pow(2.0f, 0.3)));
  EXPECT_EQ(Bits(Pow(10, -5)), Bits(std::pow(10, -5)));
  EXPECT_EQ(Bits(Atan2(1.0f, 3.0)), Bits(std::atan2(1.0f, 3.0)));
}

// On, every function is molt_portable_math's.
TEST_F(MoltMathTest, OnIsPortable) {
  SetPortable(true);
  EXPECT_TRUE(IsPortable());
  for (double x : kArgs) {
    EXPECT_EQ(Bits(Sin(x)), Bits(molt_portable_math::Sin(x))) << x;
    EXPECT_EQ(Bits(Cos(x)), Bits(molt_portable_math::Cos(x))) << x;
    EXPECT_EQ(Bits(Tan(x)), Bits(molt_portable_math::Tan(x))) << x;
    EXPECT_EQ(Bits(Asin(x)), Bits(molt_portable_math::Asin(x))) << x;
    EXPECT_EQ(Bits(Acos(x)), Bits(molt_portable_math::Acos(x))) << x;
    EXPECT_EQ(Bits(Atan(x)), Bits(molt_portable_math::Atan(x))) << x;
    EXPECT_EQ(Bits(Exp(x)), Bits(molt_portable_math::Exp(x))) << x;
    EXPECT_EQ(Bits(Exp2(x)), Bits(molt_portable_math::Exp2(x))) << x;
    EXPECT_EQ(Bits(Expm1(x)), Bits(molt_portable_math::Expm1(x))) << x;
    EXPECT_EQ(Bits(Log(x)), Bits(molt_portable_math::Log(x))) << x;
    EXPECT_EQ(Bits(Log2(x)), Bits(molt_portable_math::Log2(x))) << x;
    EXPECT_EQ(Bits(Log10(x)), Bits(molt_portable_math::Log10(x))) << x;
    EXPECT_EQ(Bits(Log1p(x)), Bits(molt_portable_math::Log1p(x))) << x;
    EXPECT_EQ(Bits(Cbrt(x)), Bits(molt_portable_math::Cbrt(x))) << x;
    EXPECT_EQ(Bits(Pow(x, -4.64)), Bits(molt_portable_math::Pow(x, -4.64)))
        << x;
    EXPECT_EQ(Bits(Atan2(x, 3.0)), Bits(molt_portable_math::Atan2(x, 3.0)))
        << x;
    EXPECT_EQ(Bits(Hypot(x, 4.5)), Bits(molt_portable_math::Hypot(x, 4.5)))
        << x;
    const float f = static_cast<float>(x);
    EXPECT_EQ(Bits(Sin(f)), Bits(molt_portable_math::Sin(f))) << x;
    EXPECT_EQ(Bits(Cos(f)), Bits(molt_portable_math::Cos(f))) << x;
    EXPECT_EQ(Bits(Tan(f)), Bits(molt_portable_math::Tan(f))) << x;
    EXPECT_EQ(Bits(Asin(f)), Bits(molt_portable_math::Asin(f))) << x;
    EXPECT_EQ(Bits(Acos(f)), Bits(molt_portable_math::Acos(f))) << x;
    EXPECT_EQ(Bits(Atan(f)), Bits(molt_portable_math::Atan(f))) << x;
    EXPECT_EQ(Bits(Tanh(f)), Bits(molt_portable_math::Tanh(f))) << x;
    EXPECT_EQ(Bits(Exp(f)), Bits(molt_portable_math::Exp(f))) << x;
    EXPECT_EQ(Bits(Exp2(f)), Bits(molt_portable_math::Exp2(f))) << x;
    EXPECT_EQ(Bits(Log(f)), Bits(molt_portable_math::Log(f))) << x;
    EXPECT_EQ(Bits(Log2(f)), Bits(molt_portable_math::Log2(f))) << x;
    EXPECT_EQ(Bits(Log10(f)), Bits(molt_portable_math::Log10(f))) << x;
    EXPECT_EQ(Bits(Pow(f, 0.7f)), Bits(molt_portable_math::Pow(f, 0.7f)))
        << x;
    EXPECT_EQ(Bits(Atan2(f, 3.0f)), Bits(molt_portable_math::Atan2(f, 3.0f)))
        << x;
    EXPECT_EQ(Bits(Hypot(f, 2.5f)), Bits(molt_portable_math::Hypot(f, 2.5f)))
        << x;
  }
  // The one-call detector of glibc's FMA variant of pow (0x...cf1 with FMA).
  EXPECT_EQ(Bits(Pow(9.628, -4.64)), 0x3efca42b3c54ccf2ull);
}

// The complex functions: std::'s when off; libc++'s formulas with the
// portable functions when on, NaN and infinity handling included.
TEST_F(MoltMathTest, Complex) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const std::complex<double> values[] = {
      {0.3, -0.7}, {-2.5, 1e-9}, {1e300, 1e300}, {0.0, -0.0}, {-1.0, 0.0}};
  const double polar_args[][2] = {{1.0, 0.5},  {2.5, -3.0}, {0.0, 1.0},
                                  {-1.0, 1.0}, {kInf, 0.0}, {kInf, kNaN},
                                  {1.0, kNaN}, {1.0, kInf}, {kInf, kInf}};
  for (bool portable : {false, true}) {
    SetPortable(portable);
    for (const auto& z : values) {
      const double want_abs =
          portable ? molt_portable_math::Hypot(z.real(), z.imag())
                   : std::abs(z);
      const double want_arg =
          portable ? molt_portable_math::Atan2(z.imag(), z.real())
                   : std::arg(z);
      EXPECT_EQ(Bits(ComplexAbs(z)), Bits(want_abs)) << z;
      EXPECT_EQ(Bits(ComplexArg(z)), Bits(want_arg)) << z;
      const std::complex<float> zf(static_cast<float>(z.real()),
                                   static_cast<float>(z.imag()));
      if (!portable) {
        EXPECT_EQ(Bits(ComplexAbs(zf)), Bits(std::abs(zf))) << z;
      }
    }
    for (const auto& a : polar_args) {
      const std::complex<double> got = ComplexPolar(a[0], a[1]);
      const std::complex<double> want = std::polar(a[0], a[1]);
      if (!portable) {
        // Both NaN, or the same bits.
        EXPECT_EQ(std::isnan(got.real()), std::isnan(want.real()));
        EXPECT_EQ(std::isnan(got.imag()), std::isnan(want.imag()));
        if (!std::isnan(want.real())) {
          EXPECT_EQ(Bits(got.real()), Bits(want.real()));
        }
        if (!std::isnan(want.imag())) {
          EXPECT_EQ(Bits(got.imag()), Bits(want.imag()));
        }
        continue;
      }
      if (std::isfinite(a[0]) && a[0] >= 0 && std::isfinite(a[1])) {
        EXPECT_EQ(Bits(got.real()), Bits(a[0] * molt_portable_math::Cos(a[1])));
        EXPECT_EQ(Bits(got.imag()), Bits(a[0] * molt_portable_math::Sin(a[1])));
        continue;
      }
      // libc++'s answers for NaN, infinite and negative arguments.
      const bool inf_rho = std::isinf(a[0]);
      if (a[0] < 0 || (!inf_rho && !std::isfinite(a[1]))) {
        EXPECT_TRUE(std::isnan(got.real()));
        EXPECT_TRUE(std::isnan(got.imag()));
      } else if (std::isfinite(a[1])) {
        EXPECT_EQ(got.real(), kInf);             // inf * cos(0)
        EXPECT_EQ(Bits(got.imag()), Bits(0.0));  // inf * sin(0) is NaN: 0
      } else {
        EXPECT_EQ(got.real(), kInf);
        EXPECT_TRUE(std::isnan(got.imag()));
      }
    }
  }
}

// SinCosDegrees (DOMMatrix and CSS rotations, CSS sin() and cos()) goes
// through the switch, and keeps its exact multiples of 45 degrees.
TEST_F(MoltMathTest, SinCosDegrees) {
  SetPortable(true);
  for (double degrees : {1.0, 10.0, 30.0, 33.3, 60.0, 89.0, 100.0, 200.0,
                         -30.0, 359.0, 1e9}) {
    const SinCos sc = SinCosDegrees(degrees);
    SetPortable(false);
    const SinCos c_library = SinCosDegrees(degrees);
    SetPortable(true);
    // Either library is within an ulp or two of the other; what matters is
    // that the portable one is what MoltNet gets.
    EXPECT_NEAR(sc.sin, c_library.sin, 1e-15) << degrees;
    EXPECT_NEAR(sc.cos, c_library.cos, 1e-15) << degrees;
  }
  EXPECT_EQ(SinCosDegrees(90).sin, 1.0);
  EXPECT_EQ(SinCosDegrees(180).cos, -1.0);
  const SinCos thirty = SinCosDegrees(30);
  EXPECT_EQ(Bits(thirty.sin), 0x3fdfffffffffffffull);
}

}  // namespace
}  // namespace gfx::molt_math
