/*
 * Copyright 2026 GenEye AI Labs Inc.
 * Licensed under GPLv3. See LICENSE file.
 */

// MoltBrowser: the C library's math functions as Skia calls them (the
// SkScalarSin()-style macros of SkScalar.h, and the code that calls std::
// directly where a page can see the result: cubic roots, path operations,
// blur kernels, gradient interpolation, gain maps and HDR tone mapping), with
// one process-wide choice of library.
//
// By default each function is the C library's (std::sin and so on): what
// Skia called before, with the same overloads and the same promotions of
// integer and mixed arguments. In a process that called SetPortable(true)
// (MoltNet's renderers, through SkGraphics::MoltBrowserUsePortableMath(),
// before anything is drawn) they are molt_portable_math's
// (third_party/llvm-libc/molt_portable_math.h): the same result for the same
// argument on every CPU and operating system. The C libraries don't give
// that: glibc and the Windows C runtime pick fused multiply-add versions on
// CPUs that have them, and macOS's library and the three of them differ from
// each other in the last bits, which a page could see in the paths, strokes
// and hit tests it measures and in the pixels it lays text out around
// (shape-outside).
//
// Any thread. The choice is made once, before the process draws or measures
// anything, and never changes after.

#ifndef SkMoltBrowserMath_DEFINED
#define SkMoltBrowserMath_DEFINED

#include "include/private/SkAPI.h"

#include <type_traits>

namespace SkMoltBrowserMath {

// Whether this process uses the portable functions.
SK_API void SetPortable(bool portable);
SK_API bool IsPortable();

// float (std::sin(float) is sinf(), and so on)
SK_API float Sin(float x);
SK_API float Cos(float x);
SK_API float Tan(float x);
SK_API float Asin(float x);
SK_API float Acos(float x);
SK_API float Atan(float x);
SK_API float Atan2(float y, float x);
SK_API float Exp(float x);
SK_API float Exp2(float x);
SK_API float Log(float x);
SK_API float Log2(float x);
SK_API float Pow(float x, float y);
SK_API float Cbrt(float x);

// double
SK_API double Sin(double x);
SK_API double Cos(double x);
SK_API double Tan(double x);
SK_API double Asin(double x);
SK_API double Acos(double x);
SK_API double Atan(double x);
SK_API double Atan2(double y, double x);
SK_API double Exp(double x);
SK_API double Exp2(double x);
SK_API double Log(double x);
SK_API double Log2(double x);
SK_API double Pow(double x, double y);
SK_API double Cbrt(double x);

// The C++ library's promotions for the other argument types, so that a call
// site can swap std::f for SkMoltBrowserMath::F without changing what it
// computes: an integer argument, or a float and a double, compute in double
// (std::sin(1), std::pow(2.0f, 3)).
#define SK_MOLTBROWSER_MATH_PROMOTE_1(Name)                                \
    template <typename T, std::enable_if_t<std::is_integral_v<T>, bool> = true> \
    inline double Name(T x) {                                              \
        return Name(static_cast<double>(x));                               \
    }
#define SK_MOLTBROWSER_MATH_PROMOTE_2(Name)                                     \
    template <typename A, typename B,                                           \
              std::enable_if_t<std::is_arithmetic_v<A> && std::is_arithmetic_v<B> && \
                                       (!std::is_same_v<A, B> || std::is_integral_v<A>), \
                               bool> = true>                                    \
    inline double Name(A x, B y) {                                              \
        return Name(static_cast<double>(x), static_cast<double>(y));            \
    }
SK_MOLTBROWSER_MATH_PROMOTE_1(Sin)
SK_MOLTBROWSER_MATH_PROMOTE_1(Cos)
SK_MOLTBROWSER_MATH_PROMOTE_1(Tan)
SK_MOLTBROWSER_MATH_PROMOTE_1(Asin)
SK_MOLTBROWSER_MATH_PROMOTE_1(Acos)
SK_MOLTBROWSER_MATH_PROMOTE_1(Atan)
SK_MOLTBROWSER_MATH_PROMOTE_1(Exp)
SK_MOLTBROWSER_MATH_PROMOTE_1(Exp2)
SK_MOLTBROWSER_MATH_PROMOTE_1(Log)
SK_MOLTBROWSER_MATH_PROMOTE_1(Log2)
SK_MOLTBROWSER_MATH_PROMOTE_1(Cbrt)
SK_MOLTBROWSER_MATH_PROMOTE_2(Atan2)
SK_MOLTBROWSER_MATH_PROMOTE_2(Pow)
#undef SK_MOLTBROWSER_MATH_PROMOTE_1
#undef SK_MOLTBROWSER_MATH_PROMOTE_2

}  // namespace SkMoltBrowserMath

#endif  // SkMoltBrowserMath_DEFINED
