// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "ui/gfx/geometry/molt_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/llvm-libc/molt_portable_math.h"
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
