// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: llvm-libc's math functions, built to give the same result for
// the same argument on every CPU and operating system.
//
// The C library's sin(), pow() and the like differ between platforms and
// between CPUs of one platform: glibc and the Windows C runtime pick fused
// multiply-add (FMA) variants on CPUs that have FMA3, macOS has its own
// library, and even llvm-libc, which V8 builds its Math functions from, takes
// other code paths where the target has FMA (every arm64 CPU) than where it
// doesn't (Chromium's x86-64 baseline). The results differ in the last bit
// for a fraction of the arguments, which is enough for a page to tell the
// machines apart.
//
// These are llvm-libc's implementations built without fused multiply-adds on
// every target (molt_portable_math.cc): each result comes from the same
// sequence of IEEE-754 additions, multiplications, divisions and square roots
// on x86-64 and arm64, whatever the OS and the CPU's extensions. On x86-64 that
// is the code V8 already runs for Math. Most of them (sin, cos, tan, exp, log,
// pow, hypot, ...) are correctly rounded as well; atan, asin and acos are
// within one unit in the last place.
//
// Pure functions, no state, safe on any thread. The process-wide switch that
// decides who calls them lives with the callers: V8's --portable-math
// (v8/src/base/ieee754.h), gfx::molt_math (ui/gfx/geometry/molt_math.h) and
// Skia's SkMoltBrowserMath (third_party/skia/include/private/
// SkMoltBrowserMath.h); WebRTC's webrtc::molt_math (third_party/webrtc/
// rtc_base/molt_math.h) decides per audio processing instance.

#ifndef THIRD_PARTY_LLVM_LIBC_MOLT_PORTABLE_MATH_H_
#define THIRD_PARTY_LLVM_LIBC_MOLT_PORTABLE_MATH_H_

namespace molt_portable_math {

// double
double Sin(double x);
double Cos(double x);
double Tan(double x);
double Asin(double x);
double Acos(double x);
double Atan(double x);
double Atan2(double y, double x);
double Exp(double x);
double Exp2(double x);
double Expm1(double x);
double Log(double x);
double Log2(double x);
double Log10(double x);
double Log1p(double x);
double Pow(double x, double y);
double Hypot(double x, double y);
double Cbrt(double x);

// float
float Sin(float x);
float Cos(float x);
float Tan(float x);
float Asin(float x);
float Acos(float x);
float Atan(float x);
float Atan2(float y, float x);
float Tanh(float x);
float Exp(float x);
float Exp2(float x);
float Log(float x);
float Log2(float x);
float Log10(float x);
float Pow(float x, float y);
float Hypot(float x, float y);

}  // namespace molt_portable_math

#endif  // THIRD_PARTY_LLVM_LIBC_MOLT_PORTABLE_MATH_H_
