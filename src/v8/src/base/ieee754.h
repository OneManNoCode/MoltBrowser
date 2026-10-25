// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_BASE_IEEE754_H_
#define V8_BASE_IEEE754_H_

#include <cmath>

#include "src/base/base-export.h"

namespace v8 {
namespace base {
namespace ieee754 {

// Returns the arc cosine of |x|; that is the value whose cosine is |x|.
V8_BASE_EXPORT double acos(double x);

// Returns the inverse hyperbolic cosine of |x|; that is the value whose
// hyperbolic cosine is |x|.
V8_BASE_EXPORT double acosh(double x);

// Returns the arc sine of |x|; that is the value whose sine is |x|.
V8_BASE_EXPORT double asin(double x);

// Returns the inverse hyperbolic sine of |x|; that is the value whose
// hyperbolic sine is |x|.
V8_BASE_EXPORT double asinh(double x);

// Returns the principal value of the arc tangent of |x|; that is the value
// whose tangent is |x|.
V8_BASE_EXPORT double atan(double x);

// Returns the principal value of the arc tangent of |y/x|, using the signs of
// the two arguments to determine the quadrant of the result.
V8_BASE_EXPORT double atan2(double y, double x);

V8_BASE_EXPORT double cos(double x);
V8_BASE_EXPORT double sin(double x);

// Returns the base-e exponential of |x|.
V8_BASE_EXPORT double exp(double x);

V8_BASE_EXPORT double atanh(double x);

// Returns the natural logarithm of |x|.
V8_BASE_EXPORT double log(double x);

// Returns a value equivalent to |log(1+x)|, but computed in a way that is
// accurate even if the value of |x| is near zero.
V8_BASE_EXPORT double log1p(double x);

// Returns the base 2 logarithm of |x|.
V8_BASE_EXPORT double log2(double x);

// Returns the base 10 logarithm of |x|.
V8_BASE_EXPORT double log10(double x);

// Returns the cube root of |x|.
V8_BASE_EXPORT double cbrt(double x);

// Returns exp(x)-1, the exponential of |x| minus 1.
V8_BASE_EXPORT double expm1(double x);

namespace legacy {

// This function should not be used directly. Instead, use
// v8::internal::math::pow.

// Returns |x| to the power of |y|.
// The result of base ** exponent when base is 1 or -1 and exponent is
// +Infinity or -Infinity differs from IEEE 754-2008. The first edition
// of ECMAScript specified a result of NaN for this operation, whereas
// later versions of IEEE 754-2008 specified 1. The historical ECMAScript
// behaviour is preserved for compatibility reasons.
V8_BASE_EXPORT double pow(double x, double y);

}  // namespace legacy

// Returns the tangent of |x|, where |x| is given in radians.
V8_BASE_EXPORT double tan(double x);

// Returns the hyperbolic cosine of |x|, where |x| is given radians.
V8_BASE_EXPORT double cosh(double x);

// Returns the hyperbolic sine of |x|, where |x| is given radians.
V8_BASE_EXPORT double sinh(double x);

// Returns the hyperbolic tangent of |x|, where |x| is given radians.
V8_BASE_EXPORT double tanh(double x);

// MoltBrowser: tanh() calls std::tanh, whose C library can pick its code by
// CPU feature (glibc's FMA variants), unless this is called with false
// (V8::Initialize does, for --no-use-std-math-tanh): then it computes
// fdlibm's tanh over expm1() above, the same way on every CPU.
V8_BASE_EXPORT void SetUseStdTanh(bool use_std_tanh);

// MoltBrowser: the functions above that V8 computes with llvm-libc (acos,
// asin, atan, atan2, cos, exp, log, log1p, log2, log10, expm1, cbrt, sin,
// tan, legacy::pow, and through them the fdlibm ones) use llvm-libc's build
// of the target's features (fused multiply-add on arm64) unless this is
// called with true (V8::Initialize does, for --portable-math): then they use
// molt_portable_math's build without them, the same results on every
// architecture, the ones x86-64 gives by default.
V8_BASE_EXPORT void SetUsePortableMath(bool use_portable_math);

}  // namespace ieee754
}  // namespace base
}  // namespace v8

#endif  // V8_BASE_IEEE754_H_
