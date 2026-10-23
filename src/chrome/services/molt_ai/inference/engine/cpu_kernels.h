// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which of llama.cpp's x86-64 CPU kernel sets this CPU runs
// (docs/design/SANDBOXED_INFERENCE.md section 12). //third_party/llama_cpp
// builds ggml's two instruction-set-specific files once per level; the
// inference service picks the best level the CPU and its OS support before
// any ggml computation. Std-only.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CPU_KERNELS_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CPU_KERNELS_H_

#include <cstdint>
#include <optional>

namespace molt_ai {

// The levels, lowest first. Mirrors molt_llama::X86CpuKernels
// (third_party/llama_cpp/chromium/ggml-cpu-x86-dispatch.h).
enum class X86CpuKernels {
  // x86-64-v2: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT. The minimum.
  kX86_64_V2,
  // The above and AVX.
  kAvx,
  // The above and AVX2, FMA, F16C.
  kAvx2,
};

// The CPUID and XCR0 bits the choice reads, as the CPU reports them.
struct X86CpuInfo {
  // CPUID leaf 0, EAX: the highest standard leaf.
  uint32_t max_leaf = 0;
  // CPUID leaf 1, ECX.
  uint32_t leaf1_ecx = 0;
  // CPUID leaf 7 subleaf 0, EBX (0 when max_leaf < 7).
  uint32_t leaf7_ebx = 0;
  // XGETBV(0): the register state the OS saves (0 unless leaf 1 reports
  // OSXSAVE).
  uint64_t xcr0 = 0;
};

// The best level |info| supports, scored like ggml's
// arch/x86/cpu-feats.cpp. Unlike it, AVX, AVX2, FMA and F16C count only when
// the OS saves the YMM registers (OSXSAVE, XCR0 bits 1 and 2), as base::CPU
// checks: otherwise their first instruction faults. nullopt below
// x86-64-v2.
std::optional<X86CpuKernels> ChooseX86CpuKernels(const X86CpuInfo& info);

// This CPU's bits. All zero when not built for x86-64.
X86CpuInfo ReadX86CpuInfo();

// ChooseX86CpuKernels(ReadX86CpuInfo()), read once per process. nullopt
// when not built for x86-64.
std::optional<X86CpuKernels> HostX86CpuKernels();

// "x86-64-v2", "avx" or "avx2".
const char* X86CpuKernelsName(X86CpuKernels kernels);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CPU_KERNELS_H_
