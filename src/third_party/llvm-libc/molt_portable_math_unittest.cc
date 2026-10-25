// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt_portable_math gives the same results on every CPU and OS. The golden
// hashes below were computed on x86-64 (Chromium's baseline and an AVX2 + FMA
// build, on CPUs from Core 2 to AVX-512) and arm64 (Cortex-A72, Neoverse N1),
// which all agree; a build machine of another architecture or OS that gets
// another hash has found a difference.

#include "third_party/llvm-libc/molt_portable_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_portable_math {
namespace {

uint64_t Bits(double d) {
  if (std::isnan(d)) {
    return 0x7ff8000000000000ull;  // A page can't see NaN payloads here.
  }
  uint64_t u;
  std::memcpy(&u, &d, sizeof(u));
  return u;
}

uint64_t Bits(float f) {
  if (std::isnan(f)) {
    return 0x7fc00000u;
  }
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

// Arguments: special values, then a fixed pseudo-random mix of ranges (unit,
// degrees-as-radians, large, tiny, raw bit patterns).
class Args {
 public:
  double Next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    double u = static_cast<double>(s_ >> 11) * 0x1p-53;
    switch (i_++ % 7) {
      case 0:
        return u * 2 - 1;
      case 1:
        return (u * 2 - 1) * 10;
      case 2:
        return (u * 2 - 1) * 1e6;
      case 3:
        return u * 710 - 5;
      case 4:
        return u * 0.7853981633974483;  // [0, 45 degrees]
      case 5:
        return (u * 2 - 1) * 1e-3;
      default: {
        double x;
        std::memcpy(&x, &s_, sizeof(x));
        return x;
      }
    }
  }

 private:
  uint64_t s_ = 0x9e3779b97f4a7c15ull;
  unsigned i_ = 0;
};

constexpr double kSpecial[] = {0.0,
                               -0.0,
                               1.0,
                               -1.0,
                               0.5,
                               2.0,
                               1e-300,
                               1e300,
                               3.141592653589793,
                               1.5707963267948966,
                               709.78,
                               -745.1,
                               4.9e-324,
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()};

constexpr int kCount = 20000;

class Hash {
 public:
  void Add(uint64_t bits) { h_ = (h_ ^ bits) * 0x100000001b3ull; }
  uint64_t value() const { return h_; }

 private:
  uint64_t h_ = 0xcbf29ce484222325ull;
};

template <typename T>
uint64_t Hash1(T (*f)(T)) {
  Hash h;
  for (double x : kSpecial) {
    h.Add(Bits(f(static_cast<T>(x))));
  }
  Args args;
  for (int i = 0; i < kCount; ++i) {
    h.Add(Bits(f(static_cast<T>(args.Next()))));
  }
  return h.value();
}

template <typename T>
uint64_t Hash2(T (*f)(T, T)) {
  Hash h;
  for (double x : kSpecial) {
    for (double y : kSpecial) {
      h.Add(Bits(f(static_cast<T>(x), static_cast<T>(y))));
    }
  }
  Args args;
  for (int i = 0; i < kCount; ++i) {
    T x = static_cast<T>(args.Next());
    T y = static_cast<T>(args.Next());
    if (i % 3 == 0) {
      x = std::fabs(x);
      y = static_cast<T>(std::fmod(static_cast<double>(y), 8.0));
    }
    h.Add(Bits(f(x, y)));
  }
  return h.value();
}

struct Golden {
  const char* name;
  uint64_t hash;
};

TEST(MoltPortableMathTest, ExactValues) {
  EXPECT_EQ(Bits(Sin(0.0)), Bits(0.0));
  EXPECT_EQ(Bits(Sin(-0.0)), Bits(-0.0));
  EXPECT_EQ(Cos(0.0), 1.0);
  EXPECT_EQ(Tan(0.0), 0.0);
  EXPECT_EQ(Atan2(0.0, -1.0), 3.141592653589793);
  EXPECT_EQ(Exp(0.0), 1.0);
  EXPECT_EQ(Log(1.0), 0.0);
  EXPECT_EQ(Log2(1024.0), 10.0);
  EXPECT_EQ(Log10(1e-5), -5.0);
  EXPECT_EQ(Pow(2.0, 10.0), 1024.0);
  EXPECT_EQ(Pow(10.0, -5.0), 1e-5);
  EXPECT_EQ(Hypot(3.0, 4.0), 5.0);
  EXPECT_EQ(Cbrt(27.0), 3.0);
  EXPECT_EQ(Sin(0.0f), 0.0f);
  EXPECT_EQ(Hypot(3.0f, 4.0f), 5.0f);
  EXPECT_EQ(Pow(2.0f, 0.5f), std::sqrt(2.0f));
  EXPECT_TRUE(std::isnan(Sin(std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(Log(-1.0)));
  EXPECT_TRUE(std::isnan(Acos(2.0f)));
}

// A value C libraries disagree on: glibc 2.39 on x86-64 gives 0x...cf1 on
// CPUs with FMA, 0x...cf2 (the correctly rounded result) without. And two
// that are easy to get wrong when the angle is 30 or 60 degrees.
TEST(MoltPortableMathTest, KnownValues) {
  EXPECT_EQ(Bits(Pow(9.628, -4.64)), 0x3efca42b3c54ccf2ull);
  EXPECT_EQ(Bits(Sin(0.5235987755982988)), 0x3fdfffffffffffffull);
  EXPECT_EQ(Bits(Cos(1.0471975511965976)), 0x3fe0000000000001ull);
}

TEST(MoltPortableMathTest, SameResultsOnEveryPlatform) {
  const Golden kDouble[] = {
      {"sin", Hash1<double>(Sin)},     {"cos", Hash1<double>(Cos)},
      {"tan", Hash1<double>(Tan)},     {"asin", Hash1<double>(Asin)},
      {"acos", Hash1<double>(Acos)},   {"atan", Hash1<double>(Atan)},
      {"exp", Hash1<double>(Exp)},     {"exp2", Hash1<double>(Exp2)},
      {"expm1", Hash1<double>(Expm1)}, {"log", Hash1<double>(Log)},
      {"log2", Hash1<double>(Log2)},   {"log10", Hash1<double>(Log10)},
      {"log1p", Hash1<double>(Log1p)}, {"cbrt", Hash1<double>(Cbrt)},
      {"atan2", Hash2<double>(Atan2)}, {"pow", Hash2<double>(Pow)},
      {"hypot", Hash2<double>(Hypot)},
  };
  const Golden kFloat[] = {
      {"sinf", Hash1<float>(Sin)},     {"cosf", Hash1<float>(Cos)},
      {"tanf", Hash1<float>(Tan)},     {"asinf", Hash1<float>(Asin)},
      {"acosf", Hash1<float>(Acos)},   {"atanf", Hash1<float>(Atan)},
      {"tanhf", Hash1<float>(Tanh)},   {"expf", Hash1<float>(Exp)},
      {"exp2f", Hash1<float>(Exp2)},   {"logf", Hash1<float>(Log)},
      {"log2f", Hash1<float>(Log2)},   {"log10f", Hash1<float>(Log10)},
      {"atan2f", Hash2<float>(Atan2)}, {"powf", Hash2<float>(Pow)},
      {"hypotf", Hash2<float>(Hypot)},
  };
  const Golden kExpected[] = {
      {"sin", 0xabb898bf4a58df1bull},
      {"cos", 0x5659be144e7b0ecbull},
      {"tan", 0x2b660c60f3f06e9full},
      {"asin", 0xd80394ecb47a0ce7ull},
      {"acos", 0x2ca9ccd529444834ull},
      {"atan", 0xddf6b7a8990d3db2ull},
      {"exp", 0x0fd240fc4d9834b8ull},
      {"exp2", 0xcf817856cf10ba46ull},
      {"expm1", 0x3762cde284c4e197ull},
      {"log", 0xd16161bb648dfb07ull},
      {"log2", 0x289fe51fb2fdaedbull},
      {"log10", 0xdacb3122b32d2ae9ull},
      {"log1p", 0x14a3a0d829c55c14ull},
      {"cbrt", 0xef2893644d4ec16aull},
      {"atan2", 0x256a5428ec4a5309ull},
      {"pow", 0x925e13d883f089ccull},
      {"hypot", 0x486c2bce683d6bc0ull},
      {"sinf", 0x15c0e26623da08e3ull},
      {"cosf", 0xd43b941810e6d73eull},
      {"tanf", 0x5e59b9f9ab3f1a9eull},
      {"asinf", 0x74ec0a6942ca6bf5ull},
      {"acosf", 0xb16f8f4afb3c4f42ull},
      {"atanf", 0xf2b90a77416b6925ull},
      {"tanhf", 0xb9d09780c07f9c5bull},
      {"expf", 0x66bb427981ec5fa3ull},
      {"exp2f", 0x53cdf6b505f82bd2ull},
      {"logf", 0x10fab414d1af7236ull},
      {"log2f", 0x64d2a6bd010ae0d2ull},
      {"log10f", 0xd1006d888d22e946ull},
      {"atan2f", 0x72e80c5292f0e0aaull},
      {"powf", 0xd2b84b48129c97a7ull},
      {"hypotf", 0x3453a4364d642fbbull},
  };
  ASSERT_EQ(std::size(kDouble) + std::size(kFloat), std::size(kExpected));
  size_t i = 0;
  for (const Golden& g : kDouble) {
    EXPECT_STREQ(g.name, kExpected[i].name);
    EXPECT_EQ(g.hash, kExpected[i].hash) << g.name;
    ++i;
  }
  for (const Golden& g : kFloat) {
    EXPECT_STREQ(g.name, kExpected[i].name);
    EXPECT_EQ(g.hash, kExpected[i].hash) << g.name;
    ++i;
  }
}

}  // namespace
}  // namespace molt_portable_math
