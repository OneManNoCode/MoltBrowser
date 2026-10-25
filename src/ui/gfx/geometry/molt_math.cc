// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "ui/gfx/geometry/molt_math.h"

#include <atomic>
#include <cmath>

#include "third_party/fdlibm/ieee754.h"
#include "third_party/llvm-libc/molt_portable_math.h"

namespace gfx::molt_math {

namespace {

// Set before the process does any of the work that calls these, so relaxed
// loads see it everywhere.
constinit std::atomic<bool> g_portable{false};

bool Portable() {
  return g_portable.load(std::memory_order_relaxed);
}

}  // namespace

void SetPortable(bool portable) {
  g_portable.store(portable, std::memory_order_relaxed);
}

bool IsPortable() {
  return Portable();
}

#define MOLT_MATH_1(T, Name, std_name)                                 \
  T Name(T x) {                                                        \
    return Portable() ? molt_portable_math::Name(x) : std::std_name(x); \
  }
#define MOLT_MATH_2(T, Name, std_name)                                       \
  T Name(T x, T y) {                                                         \
    return Portable() ? molt_portable_math::Name(x, y) : std::std_name(x, y); \
  }

MOLT_MATH_1(double, Sin, sin)
MOLT_MATH_1(double, Cos, cos)
MOLT_MATH_1(double, Tan, tan)
MOLT_MATH_1(double, Asin, asin)
MOLT_MATH_1(double, Acos, acos)
MOLT_MATH_1(double, Atan, atan)
MOLT_MATH_2(double, Atan2, atan2)
MOLT_MATH_1(double, Exp, exp)
MOLT_MATH_1(double, Exp2, exp2)
MOLT_MATH_1(double, Expm1, expm1)
MOLT_MATH_1(double, Log, log)
MOLT_MATH_1(double, Log2, log2)
MOLT_MATH_1(double, Log10, log10)
MOLT_MATH_1(double, Log1p, log1p)
MOLT_MATH_2(double, Pow, pow)
MOLT_MATH_2(double, Hypot, hypot)
MOLT_MATH_1(double, Cbrt, cbrt)

double Tanh(double x) {
  return Portable() ? fdlibm::tanh(x) : std::tanh(x);
}

MOLT_MATH_1(float, Sin, sin)
MOLT_MATH_1(float, Cos, cos)
MOLT_MATH_1(float, Tan, tan)
MOLT_MATH_1(float, Asin, asin)
MOLT_MATH_1(float, Acos, acos)
MOLT_MATH_1(float, Atan, atan)
MOLT_MATH_2(float, Atan2, atan2)
MOLT_MATH_1(float, Tanh, tanh)
MOLT_MATH_1(float, Exp, exp)
MOLT_MATH_1(float, Exp2, exp2)
MOLT_MATH_1(float, Log, log)
MOLT_MATH_1(float, Log2, log2)
MOLT_MATH_1(float, Log10, log10)
MOLT_MATH_2(float, Pow, pow)
MOLT_MATH_2(float, Hypot, hypot)

#undef MOLT_MATH_1
#undef MOLT_MATH_2

}  // namespace gfx::molt_math
