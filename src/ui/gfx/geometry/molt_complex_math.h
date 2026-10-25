// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: std::abs, std::arg and std::polar of std::complex values for
// the computations a page can read back (Web Audio's filter frequency
// responses, the analyser's magnitudes, HRTF kernels), with molt_math.h's
// process-wide choice of library.
//
// By default each function is exactly the std:: one. In a process that called
// SetPortable(true) they compute what libc++ computes, with molt_math's
// portable functions in place of the C library's: the magnitude with Hypot(),
// the phase with Atan2(), polar coordinates with Cos() and Sin() (and
// libc++'s handling of NaN and infinite arguments).

#ifndef UI_GFX_GEOMETRY_MOLT_COMPLEX_MATH_H_
#define UI_GFX_GEOMETRY_MOLT_COMPLEX_MATH_H_

#include <cmath>
#include <complex>
#include <concepts>
#include <limits>

#include "ui/gfx/geometry/molt_math.h"

namespace gfx::molt_math {

// std::abs(z).
template <std::floating_point T>
T ComplexAbs(const std::complex<T>& z) {
  return IsPortable() ? Hypot(z.real(), z.imag()) : std::abs(z);
}

// std::arg(z).
template <std::floating_point T>
T ComplexArg(const std::complex<T>& z) {
  return IsPortable() ? Atan2(z.imag(), z.real()) : std::arg(z);
}

// std::polar(rho, theta).
template <std::floating_point T>
std::complex<T> ComplexPolar(T rho, T theta) {
  if (!IsPortable()) {
    return std::polar(rho, theta);
  }
  constexpr T kNaN = std::numeric_limits<T>::quiet_NaN();
  if (std::isnan(rho) || std::signbit(rho)) {
    return std::complex<T>(kNaN, kNaN);
  }
  if (std::isnan(theta)) {
    return std::isinf(rho) ? std::complex<T>(rho, theta)
                           : std::complex<T>(theta, theta);
  }
  if (std::isinf(theta)) {
    return std::isinf(rho) ? std::complex<T>(rho, kNaN)
                           : std::complex<T>(kNaN, kNaN);
  }
  T x = rho * Cos(theta);
  if (std::isnan(x)) {
    x = 0;
  }
  T y = rho * Sin(theta);
  if (std::isnan(y)) {
    y = 0;
  }
  return std::complex<T>(x, y);
}

}  // namespace gfx::molt_math

#endif  // UI_GFX_GEOMETRY_MOLT_COMPLEX_MATH_H_
