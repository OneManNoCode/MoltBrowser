// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: see molt_portable_math.h.
//
// How the build is made the same everywhere: llvm-libc's math code chooses
// its algorithms from the target's features at compile time (its
// macros/properties/cpu_features.h). This file turns off, before any
// llvm-libc header, every feature a math function branches on that some of
// Chromium's targets have and others don't:
//
//   fused multiply-add   __ARM_FEATURE_FMA (every arm64 target), __FMA__
//                        (x86-64 built with FMA3) -> fputil::multiply_add()
//                        is a separate multiplication and addition, and the
//                        functions take their non-FMA range reductions and
//                        polynomial evaluations, as on Chromium's x86-64
//                        baseline;
//   rounding instruction LIBC_TARGET_CPU_HAS_NEAREST_INT (arm64, x86-64
//                        with SSE4.2) -> nearest_integer() is computed in
//                        software, and the functions that compute their range
//                        reduction differently with it (tanhf) take the same
//                        branch on every target.
//
// What is left is the same sequence of IEEE-754 operations on every target:
// additions, multiplications, divisions, hardware square roots (correctly
// rounded on every CPU Chromium runs on) and integer arithmetic. Chromium
// compiles with -ffp-contract=off, and the pragma below keeps the compiler
// from fusing anything in this file whatever the flags. It gets its own
// LIBC_NAMESPACE, so its functions never merge with the target-tuned copies
// V8 and libc++ build from the same headers (an ODR clash otherwise: the same
// inline functions compiled two ways).
//
// Errors and floating-point exceptions aren't reported (LIBC_MATH_NO_ERRNO,
// LIBC_MATH_NO_EXCEPT): none of the callers reads errno or the exception
// flags. That changes no result.

// Standard headers llvm-libc pulls in, before the feature macros change.
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdint.h>

#undef LIBC_NAMESPACE
#define LIBC_NAMESPACE __llvm_libc_molt_portable

#undef LIBC_MATH
#define LIBC_MATH (LIBC_MATH_NO_ERRNO | LIBC_MATH_NO_EXCEPT)

#undef __ARM_FEATURE_FMA
#undef __FMA__

// The macros cpu_features.h derives; it is included once (header guard), so
// they stay undefined for every header below.
#include "src/__support/macros/properties/cpu_features.h"
#undef LIBC_TARGET_CPU_HAS_FMA
#undef LIBC_TARGET_CPU_HAS_FMA_HALF
#undef LIBC_TARGET_CPU_HAS_FMA_FLOAT
#undef LIBC_TARGET_CPU_HAS_FMA_DOUBLE
#undef LIBC_TARGET_CPU_HAS_NEAREST_INT

#pragma clang fp contract(off)

#include "shared/math/acos.h"
#include "shared/math/acosf.h"
#include "shared/math/asin.h"
#include "shared/math/asinf.h"
#include "shared/math/atan.h"
#include "shared/math/atan2.h"
#include "shared/math/atan2f.h"
#include "shared/math/atanf.h"
#include "shared/math/cbrt.h"
#include "shared/math/cos.h"
#include "shared/math/cosf.h"
#include "shared/math/exp.h"
#include "shared/math/exp2.h"
#include "shared/math/exp2f.h"
#include "shared/math/expf.h"
#include "shared/math/expm1.h"
#include "shared/math/hypot.h"
#include "shared/math/hypotf.h"
#include "shared/math/log.h"
#include "shared/math/log10.h"
#include "shared/math/log10f.h"
#include "shared/math/log1p.h"
#include "shared/math/log2.h"
#include "shared/math/log2f.h"
#include "shared/math/logf.h"
#include "shared/math/pow.h"
#include "shared/math/powf.h"
#include "shared/math/sin.h"
#include "shared/math/sinf.h"
#include "shared/math/tan.h"
#include "shared/math/tanf.h"
#include "shared/math/tanhf.h"

#if defined(LIBC_TARGET_CPU_HAS_FMA) || defined(LIBC_TARGET_CPU_HAS_FMA_DOUBLE) || \
    defined(LIBC_TARGET_CPU_HAS_FMA_FLOAT) ||                                     \
    defined(LIBC_TARGET_CPU_HAS_NEAREST_INT)
#error "molt_portable_math must be built without target-dependent math paths"
#endif

#include "third_party/llvm-libc/molt_portable_math.h"

namespace molt_portable_math {

namespace libc = LIBC_NAMESPACE::shared;

double Sin(double x) {
  return libc::sin(x);
}
double Cos(double x) {
  return libc::cos(x);
}
double Tan(double x) {
  return libc::tan(x);
}
double Asin(double x) {
  return libc::asin(x);
}
double Acos(double x) {
  return libc::acos(x);
}
double Atan(double x) {
  return libc::atan(x);
}
double Atan2(double y, double x) {
  return libc::atan2(y, x);
}
double Exp(double x) {
  return libc::exp(x);
}
double Exp2(double x) {
  return libc::exp2(x);
}
double Expm1(double x) {
  return libc::expm1(x);
}
double Log(double x) {
  return libc::log(x);
}
double Log2(double x) {
  return libc::log2(x);
}
double Log10(double x) {
  return libc::log10(x);
}
double Log1p(double x) {
  return libc::log1p(x);
}
double Pow(double x, double y) {
  return libc::pow(x, y);
}
double Hypot(double x, double y) {
  return libc::hypot(x, y);
}
double Cbrt(double x) {
  return libc::cbrt(x);
}

float Sin(float x) {
  return libc::sinf(x);
}
float Cos(float x) {
  return libc::cosf(x);
}
float Tan(float x) {
  return libc::tanf(x);
}
float Asin(float x) {
  return libc::asinf(x);
}
float Acos(float x) {
  return libc::acosf(x);
}
float Atan(float x) {
  return libc::atanf(x);
}
float Atan2(float y, float x) {
  return libc::atan2f(y, x);
}
float Tanh(float x) {
  return libc::tanhf(x);
}
float Exp(float x) {
  return libc::expf(x);
}
float Exp2(float x) {
  return libc::exp2f(x);
}
float Log(float x) {
  return libc::logf(x);
}
float Log2(float x) {
  return libc::log2f(x);
}
float Log10(float x) {
  return libc::log10f(x);
}
float Pow(float x, float y) {
  return libc::powf(x, y);
}
float Hypot(float x, float y) {
  return libc::hypotf(x, y);
}

}  // namespace molt_portable_math
