// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the math functions of the computations a page can read back
// (DOMMatrix, CSS math functions, transforms and their interpolation, canvas
// transforms, Web Audio, ...), with one process-wide choice of library.
//
// By default each function is the C library's (std::sin and so on), exactly
// what the call sites called before. In a process that called
// SetPortable(true) (MoltNet renderers, before Blink starts:
// third_party/blink/renderer/core/molt_fingerprinting/moltnet_cpu.h) they are
// molt_portable_math's (third_party/llvm-libc/molt_portable_math.h), which give
// the same result for the same argument on every CPU and OS. The C libraries
// don't: glibc and the Windows C runtime pick FMA variants on CPUs that have
// FMA3, and macOS and the three of them differ from each other.
//
// Any thread. The choice is made once, before the process starts any of this
// work, and never changes after.

#ifndef UI_GFX_GEOMETRY_MOLT_MATH_H_
#define UI_GFX_GEOMETRY_MOLT_MATH_H_

#include <concepts>
#include <type_traits>

#include "base/component_export.h"

namespace gfx::molt_math {

// Whether this process uses the portable functions.
COMPONENT_EXPORT(GEOMETRY) void SetPortable(bool portable);
COMPONENT_EXPORT(GEOMETRY) bool IsPortable();

// double
COMPONENT_EXPORT(GEOMETRY) double Sin(double x);
COMPONENT_EXPORT(GEOMETRY) double Cos(double x);
COMPONENT_EXPORT(GEOMETRY) double Tan(double x);
COMPONENT_EXPORT(GEOMETRY) double Asin(double x);
COMPONENT_EXPORT(GEOMETRY) double Acos(double x);
COMPONENT_EXPORT(GEOMETRY) double Atan(double x);
COMPONENT_EXPORT(GEOMETRY) double Atan2(double y, double x);
COMPONENT_EXPORT(GEOMETRY) double Exp(double x);
COMPONENT_EXPORT(GEOMETRY) double Exp2(double x);
COMPONENT_EXPORT(GEOMETRY) double Expm1(double x);
COMPONENT_EXPORT(GEOMETRY) double Log(double x);
COMPONENT_EXPORT(GEOMETRY) double Log2(double x);
COMPONENT_EXPORT(GEOMETRY) double Log10(double x);
COMPONENT_EXPORT(GEOMETRY) double Log1p(double x);
COMPONENT_EXPORT(GEOMETRY) double Pow(double x, double y);
COMPONENT_EXPORT(GEOMETRY) double Hypot(double x, double y);
COMPONENT_EXPORT(GEOMETRY) double Cbrt(double x);

// float (std::sin(float) is sinf(), and so on)
COMPONENT_EXPORT(GEOMETRY) float Sin(float x);
COMPONENT_EXPORT(GEOMETRY) float Cos(float x);
COMPONENT_EXPORT(GEOMETRY) float Tan(float x);
COMPONENT_EXPORT(GEOMETRY) float Asin(float x);
COMPONENT_EXPORT(GEOMETRY) float Acos(float x);
COMPONENT_EXPORT(GEOMETRY) float Atan(float x);
COMPONENT_EXPORT(GEOMETRY) float Atan2(float y, float x);
COMPONENT_EXPORT(GEOMETRY) float Tanh(float x);
COMPONENT_EXPORT(GEOMETRY) float Exp(float x);
COMPONENT_EXPORT(GEOMETRY) float Exp2(float x);
COMPONENT_EXPORT(GEOMETRY) float Log(float x);
COMPONENT_EXPORT(GEOMETRY) float Log2(float x);
COMPONENT_EXPORT(GEOMETRY) float Log10(float x);
COMPONENT_EXPORT(GEOMETRY) float Pow(float x, float y);
COMPONENT_EXPORT(GEOMETRY) float Hypot(float x, float y);

// The C++ library's promotions for the other argument types, so that a call
// site can swap std::f for molt_math::F without changing what it computes:
// an integer argument, or a float and a double, compute in double
// (std::sin(1), std::pow(2.0f, 0.5)).
#define MOLT_MATH_PROMOTE_1(Name)              \
  template <typename T>                        \
    requires std::integral<T>                  \
  double Name(T x) {                           \
    return Name(static_cast<double>(x));       \
  }
#define MOLT_MATH_PROMOTE_2(Name)                                        \
  template <typename A, typename B>                                      \
    requires std::is_arithmetic_v<A> && std::is_arithmetic_v<B> &&       \
             (!std::is_same_v<A, B> || std::integral<A>)                 \
  double Name(A x, B y) {                                                \
    return Name(static_cast<double>(x), static_cast<double>(y));         \
  }
MOLT_MATH_PROMOTE_1(Sin)
MOLT_MATH_PROMOTE_1(Cos)
MOLT_MATH_PROMOTE_1(Tan)
MOLT_MATH_PROMOTE_1(Asin)
MOLT_MATH_PROMOTE_1(Acos)
MOLT_MATH_PROMOTE_1(Atan)
MOLT_MATH_PROMOTE_1(Exp)
MOLT_MATH_PROMOTE_1(Exp2)
MOLT_MATH_PROMOTE_1(Expm1)
MOLT_MATH_PROMOTE_1(Log)
MOLT_MATH_PROMOTE_1(Log2)
MOLT_MATH_PROMOTE_1(Log10)
MOLT_MATH_PROMOTE_1(Log1p)
MOLT_MATH_PROMOTE_1(Cbrt)
MOLT_MATH_PROMOTE_2(Atan2)
MOLT_MATH_PROMOTE_2(Pow)
MOLT_MATH_PROMOTE_2(Hypot)
#undef MOLT_MATH_PROMOTE_1
#undef MOLT_MATH_PROMOTE_2

}  // namespace gfx::molt_math

#endif  // UI_GFX_GEOMETRY_MOLT_MATH_H_
