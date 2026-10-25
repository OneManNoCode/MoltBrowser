/*
 * Copyright 2026 GenEye AI Labs Inc.
 * Licensed under GPLv3. See LICENSE file.
 */

// MoltBrowser: see include/private/SkMoltBrowserMath.h.

#include "include/private/SkMoltBrowserMath.h"

#include <atomic>
#include <cmath>

#include "third_party/llvm-libc/molt_portable_math.h"

namespace SkMoltBrowserMath {

namespace {

// Set before the process draws or measures anything, so relaxed loads see it
// on every thread.
constinit std::atomic<bool> gPortable{false};

bool portable() {
    return gPortable.load(std::memory_order_relaxed);
}

}  // namespace

void SetPortable(bool portable) {
    gPortable.store(portable, std::memory_order_relaxed);
}

bool IsPortable() {
    return portable();
}

#define SK_MOLTBROWSER_MATH_1(T, Name, std_name)                               \
    T Name(T x) {                                                              \
        return portable() ? molt_portable_math::Name(x) : std::std_name(x);   \
    }
#define SK_MOLTBROWSER_MATH_2(T, Name, std_name)                                   \
    T Name(T x, T y) {                                                             \
        return portable() ? molt_portable_math::Name(x, y) : std::std_name(x, y); \
    }

SK_MOLTBROWSER_MATH_1(float, Sin, sin)
SK_MOLTBROWSER_MATH_1(float, Cos, cos)
SK_MOLTBROWSER_MATH_1(float, Tan, tan)
SK_MOLTBROWSER_MATH_1(float, Asin, asin)
SK_MOLTBROWSER_MATH_1(float, Acos, acos)
SK_MOLTBROWSER_MATH_1(float, Atan, atan)
SK_MOLTBROWSER_MATH_2(float, Atan2, atan2)
SK_MOLTBROWSER_MATH_1(float, Exp, exp)
SK_MOLTBROWSER_MATH_1(float, Exp2, exp2)
SK_MOLTBROWSER_MATH_1(float, Log, log)
SK_MOLTBROWSER_MATH_1(float, Log2, log2)
SK_MOLTBROWSER_MATH_2(float, Pow, pow)

// molt_portable_math has no float cube root: the double one, rounded to
// float, is the same on every CPU too.
float Cbrt(float x) {
    return portable()
            ? static_cast<float>(molt_portable_math::Cbrt(static_cast<double>(x)))
            : std::cbrt(x);
}

SK_MOLTBROWSER_MATH_1(double, Sin, sin)
SK_MOLTBROWSER_MATH_1(double, Cos, cos)
SK_MOLTBROWSER_MATH_1(double, Tan, tan)
SK_MOLTBROWSER_MATH_1(double, Asin, asin)
SK_MOLTBROWSER_MATH_1(double, Acos, acos)
SK_MOLTBROWSER_MATH_1(double, Atan, atan)
SK_MOLTBROWSER_MATH_2(double, Atan2, atan2)
SK_MOLTBROWSER_MATH_1(double, Exp, exp)
SK_MOLTBROWSER_MATH_1(double, Exp2, exp2)
SK_MOLTBROWSER_MATH_1(double, Log, log)
SK_MOLTBROWSER_MATH_1(double, Log2, log2)
SK_MOLTBROWSER_MATH_2(double, Pow, pow)
SK_MOLTBROWSER_MATH_1(double, Cbrt, cbrt)

#undef SK_MOLTBROWSER_MATH_1
#undef SK_MOLTBROWSER_MATH_2

}  // namespace SkMoltBrowserMath
