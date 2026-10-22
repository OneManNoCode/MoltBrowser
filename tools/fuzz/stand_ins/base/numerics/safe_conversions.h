// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/numerics/safe_conversions.h
// (the subset MoltBrowser uses), for tools/fuzz/molt_fuzz.py only. Same
// semantics: saturated_cast clamps to the destination's range and maps NaN
// to 0; checked_cast aborts when the value doesn't fit; ClampFloor / Ceil /
// Round round, then saturate.

#ifndef MOLT_FUZZ_STAND_INS_BASE_NUMERICS_SAFE_CONVERSIONS_H_
#define MOLT_FUZZ_STAND_INS_BASE_NUMERICS_SAFE_CONVERSIONS_H_

#include <cmath>
#include <limits>
#include <type_traits>

#include "base/check.h"

namespace base {

template <typename Dst, typename Src>
constexpr bool IsValueInRangeForNumericType(Src value) {
  using DstLimits = std::numeric_limits<Dst>;
  if constexpr (std::is_floating_point_v<Src>) {
    if (std::isnan(value)) return std::is_floating_point_v<Dst>;
    if constexpr (std::is_floating_point_v<Dst>) {
      return value >= -static_cast<Src>(DstLimits::max()) &&
             value <= static_cast<Src>(DstLimits::max());
    } else {
      // [lowest, max + 1) without rounding max up into range.
      return value >= static_cast<Src>(DstLimits::lowest()) &&
             value < static_cast<Src>(DstLimits::max()) + 1;
    }
  } else if constexpr (std::is_floating_point_v<Dst>) {
    return true;
  } else if constexpr (std::is_signed_v<Src> && !std::is_signed_v<Dst>) {
    return value >= 0 &&
           static_cast<std::make_unsigned_t<Src>>(value) <= DstLimits::max();
  } else if constexpr (!std::is_signed_v<Src> && std::is_signed_v<Dst>) {
    return value <= static_cast<std::make_unsigned_t<Dst>>(DstLimits::max());
  } else {
    return value >= DstLimits::lowest() && value <= DstLimits::max();
  }
}

template <typename Dst, typename Src>
constexpr Dst saturated_cast(Src value) {
  using DstLimits = std::numeric_limits<Dst>;
  if (IsValueInRangeForNumericType<Dst>(value)) return static_cast<Dst>(value);
  if constexpr (std::is_floating_point_v<Src>) {
    if (std::isnan(value)) return Dst(0);
  }
  return value > Src(0) ? DstLimits::max() : DstLimits::lowest();
}

template <typename Dst, typename Src>
constexpr Dst checked_cast(Src value) {
  CHECK(IsValueInRangeForNumericType<Dst>(value));
  return static_cast<Dst>(value);
}

template <typename Dst = int, typename Src>
Dst ClampFloor(Src value) {
  return saturated_cast<Dst>(std::floor(value));
}
template <typename Dst = int, typename Src>
Dst ClampCeil(Src value) {
  return saturated_cast<Dst>(std::ceil(value));
}
template <typename Dst = int, typename Src>
Dst ClampRound(Src value) {
  return saturated_cast<Dst>(std::round(value));
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_NUMERICS_SAFE_CONVERSIONS_H_
