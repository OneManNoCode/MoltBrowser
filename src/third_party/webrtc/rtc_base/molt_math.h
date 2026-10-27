// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the math functions of WebRTC's audio processing whose results
// reach the processed samples (noise suppression's gains, AGC2's levels and
// voice detector features, AEC3's comfort noise and reverb decay, the
// resampler's kernel, the noise suppressor's FFT tables).
//
// Outside a webrtc::cpu_info::ScopedCpuIndependent each function is the C
// library's (std::pow and so on), exactly what the call sites called before.
// Inside one (CPU-independent processing, rtc_base/cpu_info.h) it is
// molt_portable_math's (third_party/llvm-libc/molt_portable_math.h), which
// gives the same result for the same argument on every CPU and OS: glibc and
// the Windows C runtime pick fused multiply-add variants on CPUs with FMA3,
// and macOS and the three of them differ from each other in the last bits.
//
// Float arguments call the float functions (powf, tanhf, ...) and double
// arguments the double ones, as std:: does; there are deliberately no mixed
// or integer overloads, so a call site keeps its argument types.

#ifndef RTC_BASE_MOLT_MATH_H_
#define RTC_BASE_MOLT_MATH_H_

namespace webrtc {
namespace molt_math {

float Pow(float x, float y);
double Pow(double x, double y);
float Log10(float x);
double Log10(double x);
float Tanh(float x);
float Sin(float x);
double Sin(double x);
float Cos(float x);
double Cos(double x);
float Atan(float x);

}  // namespace molt_math
}  // namespace webrtc

#endif  // RTC_BASE_MOLT_MATH_H_
