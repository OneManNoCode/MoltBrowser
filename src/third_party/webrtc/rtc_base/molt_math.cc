// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "rtc_base/molt_math.h"

#include <cmath>

#include "rtc_base/cpu_info.h"
#include "third_party/llvm-libc/molt_portable_math.h"

namespace webrtc {
namespace molt_math {

namespace {

bool Portable() {
  return cpu_info::IsCpuIndependent();
}

}  // namespace

float Pow(float x, float y) {
  return Portable() ? molt_portable_math::Pow(x, y) : std::pow(x, y);
}

double Pow(double x, double y) {
  return Portable() ? molt_portable_math::Pow(x, y) : std::pow(x, y);
}

float Log10(float x) {
  return Portable() ? molt_portable_math::Log10(x) : std::log10(x);
}

double Log10(double x) {
  return Portable() ? molt_portable_math::Log10(x) : std::log10(x);
}

float Tanh(float x) {
  return Portable() ? molt_portable_math::Tanh(x) : std::tanh(x);
}

float Sin(float x) {
  return Portable() ? molt_portable_math::Sin(x) : std::sin(x);
}

double Sin(double x) {
  return Portable() ? molt_portable_math::Sin(x) : std::sin(x);
}

float Cos(float x) {
  return Portable() ? molt_portable_math::Cos(x) : std::cos(x);
}

double Cos(double x) {
  return Portable() ? molt_portable_math::Cos(x) : std::cos(x);
}

float Atan(float x) {
  return Portable() ? molt_portable_math::Atan(x) : std::atan(x);
}

}  // namespace molt_math
}  // namespace webrtc
