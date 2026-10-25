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

}  // namespace gfx::molt_math

#endif  // UI_GFX_GEOMETRY_MOLT_MATH_H_
