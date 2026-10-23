// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/cpu_kernels.h"

#include <array>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64)
#define MOLT_CPU_KERNELS_X86_64 1
#if defined(_MSC_VER)
#include <immintrin.h>  // _xgetbv()
#include <intrin.h>     // __cpuidex()
#endif
#endif

namespace molt_ai {

namespace {

// CPUID leaf 1, ECX.
constexpr uint32_t kEcxSse3 = 1u << 0;
constexpr uint32_t kEcxSsse3 = 1u << 9;
constexpr uint32_t kEcxFma = 1u << 12;
constexpr uint32_t kEcxSse41 = 1u << 19;
constexpr uint32_t kEcxSse42 = 1u << 20;
constexpr uint32_t kEcxPopcnt = 1u << 23;
constexpr uint32_t kEcxXsave = 1u << 26;
constexpr uint32_t kEcxOsxsave = 1u << 27;
constexpr uint32_t kEcxAvx = 1u << 28;
constexpr uint32_t kEcxF16c = 1u << 29;
// CPUID leaf 7 subleaf 0, EBX.
constexpr uint32_t kEbx7Avx2 = 1u << 5;
// XCR0: SSE (XMM) and AVX (upper YMM) state.
constexpr uint64_t kXcr0SseAvx = 0x6;

// What a level needs.
enum Feature : uint32_t {
  kSse3 = 1u << 0,
  kSsse3 = 1u << 1,
  kSse41 = 1u << 2,
  kSse42 = 1u << 3,
  kPopcnt = 1u << 4,
  kAvx = 1u << 5,
  kF16c = 1u << 6,
  kFma = 1u << 7,
  kAvx2 = 1u << 8,
};

constexpr uint32_t kX86_64_V2Features = kSse3 | kSsse3 | kSse41 | kSse42 |
                                        kPopcnt;

struct Level {
  X86CpuKernels kernels;
  uint32_t required;
};

// What //third_party/llama_cpp/BUILD.gn compiles each level with
// (_ggml_x86_levels), as run-time requirements.
constexpr Level kLevels[] = {
    {X86CpuKernels::kX86_64_V2, kX86_64_V2Features},
    {X86CpuKernels::kAvx, kX86_64_V2Features | kAvx},
    {X86CpuKernels::kAvx2, kX86_64_V2Features | kAvx | kF16c | kFma | kAvx2},
};

// ggml's score (arch/x86/cpu-feats.cpp, ggml_backend_cpu_x86_score): 1, plus
// a weight per required feature. The features below SSE4.2 weigh nothing
// there (no upstream level requires them separately).
int Score(uint32_t required) {
  int score = 1;
  if (required & kFma) score += 1;
  if (required & kF16c) score += 1 << 1;
  if (required & kSse42) score += 1 << 2;
  if (required & kAvx) score += 1 << 4;
  if (required & kAvx2) score += 1 << 5;
  return score;
}

uint32_t Features(const X86CpuInfo& info) {
  if (info.max_leaf < 1) return 0;
  const uint32_t ecx = info.leaf1_ecx;
  uint32_t features = 0;
  if (ecx & kEcxSse3) features |= kSse3;
  if (ecx & kEcxSsse3) features |= kSsse3;
  if (ecx & kEcxSse41) features |= kSse41;
  if (ecx & kEcxSse42) features |= kSse42;
  if (ecx & kEcxPopcnt) features |= kPopcnt;
  // AVX, AVX2, FMA and F16C are VEX-encoded: they fault unless the OS has
  // turned on and saves the YMM state.
  const bool os_saves_ymm = (ecx & kEcxXsave) && (ecx & kEcxOsxsave) &&
                            (info.xcr0 & kXcr0SseAvx) == kXcr0SseAvx;
  if (os_saves_ymm) {
    if (ecx & kEcxAvx) features |= kAvx;
    if (ecx & kEcxF16c) features |= kF16c;
    if (ecx & kEcxFma) features |= kFma;
    if (info.max_leaf >= 7 && (info.leaf7_ebx & kEbx7Avx2)) {
      features |= kAvx2;
    }
  }
  return features;
}

#if defined(MOLT_CPU_KERNELS_X86_64)
// EAX, EBX, ECX, EDX.
std::array<uint32_t, 4> Cpuid(uint32_t leaf, uint32_t subleaf) {
  std::array<uint32_t, 4> regs = {};
#if defined(_MSC_VER)
  std::array<int, 4> r = {};
  __cpuidex(r.data(), static_cast<int>(leaf), static_cast<int>(subleaf));
  for (size_t i = 0; i < regs.size(); ++i) {
    regs[i] = static_cast<uint32_t>(r[i]);
  }
#else
  __asm__ volatile("cpuid"
                   : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]),
                     "=d"(regs[3])
                   : "a"(leaf), "c"(subleaf));
#endif
  return regs;
}

uint64_t Xgetbv0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  uint32_t eax, edx;
  __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
  return (static_cast<uint64_t>(edx) << 32) | eax;
#endif
}
#endif  // defined(MOLT_CPU_KERNELS_X86_64)

}  // namespace

std::optional<X86CpuKernels> ChooseX86CpuKernels(const X86CpuInfo& info) {
  const uint32_t features = Features(info);
  std::optional<X86CpuKernels> best;
  int best_score = 0;
  for (const Level& level : kLevels) {
    if ((features & level.required) != level.required) continue;
    const int score = Score(level.required);
    if (score > best_score) {
      best_score = score;
      best = level.kernels;
    }
  }
  return best;
}

X86CpuInfo ReadX86CpuInfo() {
  X86CpuInfo info;
#if defined(MOLT_CPU_KERNELS_X86_64)
  info.max_leaf = Cpuid(0, 0)[0];
  if (info.max_leaf >= 1) info.leaf1_ecx = Cpuid(1, 0)[2];
  if (info.max_leaf >= 7) info.leaf7_ebx = Cpuid(7, 0)[1];
  // XGETBV itself faults without OSXSAVE; XSAVE too, as base::CPU checks
  // (crbug.com/375968).
  if ((info.leaf1_ecx & kEcxXsave) && (info.leaf1_ecx & kEcxOsxsave)) {
    info.xcr0 = Xgetbv0();
  }
#endif
  return info;
}

std::optional<X86CpuKernels> HostX86CpuKernels() {
#if defined(MOLT_CPU_KERNELS_X86_64)
  static const std::optional<X86CpuKernels> kernels =
      ChooseX86CpuKernels(ReadX86CpuInfo());
  return kernels;
#else
  return std::nullopt;
#endif
}

const char* X86CpuKernelsName(X86CpuKernels kernels) {
  switch (kernels) {
    case X86CpuKernels::kX86_64_V2:
      return "x86-64-v2";
    case X86CpuKernels::kAvx:
      return "avx";
    case X86CpuKernels::kAvx2:
      return "avx2";
  }
  return "x86-64-v2";
}

}  // namespace molt_ai
