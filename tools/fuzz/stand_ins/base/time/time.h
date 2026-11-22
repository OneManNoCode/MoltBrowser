// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// STANDALONE FUZZING STAND-IN for Chromium's base/time/time.h (the subset
// MoltBrowser uses in fuzzed code: TimeDelta and Seconds()), for
// tools/fuzz/molt_fuzz.py only. Same semantics: a TimeDelta counts
// microseconds, and Seconds() of an integer saturates at the range of a
// TimeDelta instead of overflowing.

#ifndef MOLT_FUZZ_STAND_INS_BASE_TIME_TIME_H_
#define MOLT_FUZZ_STAND_INS_BASE_TIME_TIME_H_

#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace base {

class TimeDelta {
 public:
  constexpr TimeDelta() = default;

  static constexpr TimeDelta FromMicroseconds(int64_t us) {
    return TimeDelta(us);
  }

  constexpr int64_t InMicroseconds() const { return delta_; }
  constexpr int64_t InMilliseconds() const { return delta_ / 1000; }
  constexpr int64_t InSeconds() const { return delta_ / 1'000'000; }
  constexpr bool is_zero() const { return delta_ == 0; }

  friend constexpr bool operator==(TimeDelta, TimeDelta) = default;
  friend constexpr auto operator<=>(TimeDelta, TimeDelta) = default;

 private:
  constexpr explicit TimeDelta(int64_t us) : delta_(us) {}

  int64_t delta_ = 0;
};

template <typename T>
  requires std::is_integral_v<T>
constexpr TimeDelta Seconds(T n) {
  constexpr int64_t kPerSecond = 1'000'000;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  if constexpr (std::is_signed_v<T>) {
    if (n < 0) {
      return TimeDelta::FromMicroseconds(
          static_cast<int64_t>(n) < kMin / kPerSecond
              ? kMin
              : static_cast<int64_t>(n) * kPerSecond);
    }
  }
  return TimeDelta::FromMicroseconds(
      static_cast<uint64_t>(n) > static_cast<uint64_t>(kMax / kPerSecond)
          ? kMax
          : static_cast<int64_t>(n) * kPerSecond);
}

}  // namespace base

#endif  // MOLT_FUZZ_STAND_INS_BASE_TIME_TIME_H_
