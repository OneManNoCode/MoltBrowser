/*
 *  Copyright (c) 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef RTC_BASE_CPU_INFO_H_
#define RTC_BASE_CPU_INFO_H_

#include <cmath>
#include <cstdint>

namespace webrtc {

namespace cpu_info {

// Returned number of cores is always >= 1.
uint32_t DetectNumberOfCores();

enum class ISA { kSSE2 = 0, kSSE3, kAVX2, kFMA3, kNeon };

// Returns true if the CPU supports the given instruction set.
// MoltBrowser: always false on a thread inside a ScopedCpuIndependent.
bool Supports(ISA instruction_set_architecture);

// MoltBrowser: CPU-independent processing for one audio processing instance.
//
// While a ScopedCpuIndependent(true) is alive on a thread, the code that
// WebRTC picks by the CPU's vector extensions takes its plain C++ path on
// that thread: Supports() answers false for every instruction set (SSE2,
// AVX2, FMA3 and NEON), and the places that choose NEON at compile time ask
// IsCpuIndependent() first (aec3_common.cc, agc2/cpu_features.cc,
// fft_size_128/ooura_fft.cc, resampler/sinc_resampler.cc). The math
// functions of rtc_base/molt_math.h switch to MoltBrowser's portable library.
// The plain paths are compiled without fused multiply-adds (Chromium builds
// with -ffp-contract=off), so they compute the same IEEE operations on every
// x86-64 and arm64 CPU, and the same input gives the same output samples.
//
// AudioProcessingImpl opens a scope in every public method that builds or
// runs its submodules when its Config asks for it
// (moltbrowser_cpu_independent), so the choice holds per instance: the audio
// service runs MoltNet's streams and other windows' streams side by side, on
// threads of their own or not. Scopes nest; the innermost one decides.
//
// Subnormal floats: the CPU's flush-to-zero modes don't agree between
// architectures (x86's FTZ decides a result is tiny after rounding and its
// DAZ reads subnormal operands as zero; Arm's FZ decides before rounding), so
// a ScopedCpuIndependent(true) also turns them off on the thread while it
// lives (MXCSR's FTZ and DAZ, FPCR's or FPSCR's FZ) and puts back what it
// found when it ends; AudioProcessingImpl then doesn't turn them on (its
// DenormalDisabler). IEEE 754 subnormal arithmetic gives the same results on
// every CPU; to keep its cost out of the loops, the instance sets the samples
// of its input below kCpuIndependentFloor to zero (FlushTinySample()) and
// adds a deterministic noise floor of kCpuIndependentDither to them, so that
// its filters, adaptive filters and level estimates settle at that floor in
// silence instead of decaying through the subnormal range, where x86 takes
// microcode assists.
class ScopedCpuIndependent {
 public:
  explicit ScopedCpuIndependent(bool cpu_independent);
  ~ScopedCpuIndependent();
  ScopedCpuIndependent(const ScopedCpuIndependent&) = delete;
  ScopedCpuIndependent& operator=(const ScopedCpuIndependent&) = delete;

 private:
  const bool previous_;
  uint64_t saved_fp_control_ = 0;
  bool restore_fp_control_ = false;
};

// MoltBrowser: the smallest input magnitude CPU-independent processing keeps
// (2^-63, about 1.1e-19 or -379 dBFS; its square is FLT_MIN, so the product
// of two values at or above it is a normal float). The same floor as
// Chromium's media::kPortableAudioFloor.
inline constexpr float kCpuIndependentFloor = 0x1p-63f;

// `sample`, or a zero of its sign when its magnitude is below
// kCpuIndependentFloor. Infinities and NaNs are unchanged.
inline float FlushTinySample(float sample) {
  return std::fabs(sample) < kCpuIndependentFloor
             ? std::copysign(0.0f, sample)
             : sample;
}

// MoltBrowser: the magnitude of the noise floor CPU-independent processing
// adds to its input (2^-32, about 2.3e-10 or -193 dBFS: far below any
// microphone's or converter's own noise, 24-bit audio's included), with a
// pseudo-random sign per sample from a generator each instance seeds the
// same way.
inline constexpr float kCpuIndependentDither = 0x1p-32f;

// MoltBrowser: whether a flush-to-zero mode is on on the calling thread.
bool FlushModeIsOn();

// Whether the calling thread is inside a ScopedCpuIndependent(true).
bool IsCpuIndependent();

}  // namespace cpu_info

}  // namespace webrtc

#endif  // RTC_BASE_CPU_INFO_H_
