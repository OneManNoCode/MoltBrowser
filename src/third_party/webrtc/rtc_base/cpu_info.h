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
class ScopedCpuIndependent {
 public:
  explicit ScopedCpuIndependent(bool cpu_independent);
  ~ScopedCpuIndependent();
  ScopedCpuIndependent(const ScopedCpuIndependent&) = delete;
  ScopedCpuIndependent& operator=(const ScopedCpuIndependent&) = delete;

 private:
  const bool previous_;
};

// Whether the calling thread is inside a ScopedCpuIndependent(true).
bool IsCpuIndependent();

}  // namespace cpu_info

}  // namespace webrtc

#endif  // RTC_BASE_CPU_INFO_H_
