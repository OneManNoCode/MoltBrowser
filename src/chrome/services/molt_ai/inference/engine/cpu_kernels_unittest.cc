// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/cpu_kernels.h"

#include <initializer_list>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

// CPUID leaf 1, ECX.
constexpr uint32_t kSse3 = 1u << 0;
constexpr uint32_t kSsse3 = 1u << 9;
constexpr uint32_t kFma = 1u << 12;
constexpr uint32_t kSse41 = 1u << 19;
constexpr uint32_t kSse42 = 1u << 20;
constexpr uint32_t kPopcnt = 1u << 23;
constexpr uint32_t kXsave = 1u << 26;
constexpr uint32_t kOsxsave = 1u << 27;
constexpr uint32_t kAvx = 1u << 28;
constexpr uint32_t kF16c = 1u << 29;
constexpr uint32_t kHypervisor = 1u << 31;
// CPUID leaf 7, EBX.
constexpr uint32_t kAvx2 = 1u << 5;
constexpr uint32_t kBmi2 = 1u << 8;
constexpr uint32_t kAvx512f = 1u << 16;
// XCR0.
constexpr uint64_t kX87 = 1u << 0;
constexpr uint64_t kSseState = 1u << 1;
constexpr uint64_t kYmmState = 1u << 2;
constexpr uint64_t kAvx512State = 0xe0;

constexpr uint32_t kX86_64_V2 = kSse3 | kSsse3 | kSse41 | kSse42 | kPopcnt;
constexpr uint64_t kOsAvx = kX87 | kSseState | kYmmState;

X86CpuInfo Info(uint32_t ecx, uint32_t ebx7 = 0, uint64_t xcr0 = 0,
                uint32_t max_leaf = 0xd) {
  X86CpuInfo info;
  info.max_leaf = max_leaf;
  info.leaf1_ecx = ecx;
  info.leaf7_ebx = ebx7;
  info.xcr0 = xcr0;
  return info;
}

// The CPUs the levels are named after (feature bits as their CPUID reports
// them; the other bits don't matter here).
X86CpuInfo Nehalem() {
  return Info(kX86_64_V2);
}
X86CpuInfo SandyBridge() {
  return Info(kX86_64_V2 | kXsave | kOsxsave | kAvx, 0, kOsAvx);
}
X86CpuInfo IvyBridge() {
  return Info(kX86_64_V2 | kXsave | kOsxsave | kAvx | kF16c, 0, kOsAvx);
}
X86CpuInfo Piledriver() {
  return Info(kX86_64_V2 | kXsave | kOsxsave | kAvx | kF16c | kFma, 0,
              kOsAvx);
}
X86CpuInfo Haswell() {
  return Info(kX86_64_V2 | kXsave | kOsxsave | kAvx | kF16c | kFma,
              kAvx2 | kBmi2, kOsAvx);
}

TEST(CpuKernelsTest, BelowX86_64V2IsRefused) {
  EXPECT_EQ(ChooseX86CpuKernels(X86CpuInfo()), std::nullopt);
  // Core 2 (Penryn): SSE4.1, no SSE4.2, no POPCNT.
  EXPECT_EQ(ChooseX86CpuKernels(Info(kSse3 | kSsse3 | kSse41)), std::nullopt);
  // AMD K10: POPCNT, no SSSE3 / SSE4.1 / SSE4.2.
  EXPECT_EQ(ChooseX86CpuKernels(Info(kSse3 | kPopcnt)), std::nullopt);
  // Each x86-64-v2 feature is needed.
  for (const uint32_t missing : {kSse3, kSsse3, kSse41, kSse42, kPopcnt}) {
    EXPECT_EQ(ChooseX86CpuKernels(Info(kX86_64_V2 & ~missing)), std::nullopt)
        << missing;
  }
  // AVX2 doesn't make up for missing x86-64-v2 bits (a hypervisor that
  // hides POPCNT).
  X86CpuInfo no_popcnt = Haswell();
  no_popcnt.leaf1_ecx &= ~kPopcnt;
  EXPECT_EQ(ChooseX86CpuKernels(no_popcnt), std::nullopt);
}

TEST(CpuKernelsTest, NamedCpus) {
  EXPECT_EQ(ChooseX86CpuKernels(Nehalem()), X86CpuKernels::kX86_64_V2);
  EXPECT_EQ(ChooseX86CpuKernels(SandyBridge()), X86CpuKernels::kAvx);
  EXPECT_EQ(ChooseX86CpuKernels(IvyBridge()), X86CpuKernels::kAvx);
  // FMA and F16C without AVX2 (Piledriver, Steamroller): no FMA-only level.
  EXPECT_EQ(ChooseX86CpuKernels(Piledriver()), X86CpuKernels::kAvx);
  EXPECT_EQ(ChooseX86CpuKernels(Haswell()), X86CpuKernels::kAvx2);
  // AVX-512 and a VM's hypervisor bit change nothing.
  X86CpuInfo skylake_x = Haswell();
  skylake_x.leaf7_ebx |= kAvx512f;
  skylake_x.leaf1_ecx |= kHypervisor;
  skylake_x.xcr0 |= kAvx512State;
  EXPECT_EQ(ChooseX86CpuKernels(skylake_x), X86CpuKernels::kAvx2);
}

TEST(CpuKernelsTest, AvxNeedsTheOsToSaveYmm) {
  // The kernel didn't turn on the AVX state (XCR0 without bit 2): the first
  // AVX instruction would fault, so the SSE level.
  X86CpuInfo no_ymm = Haswell();
  no_ymm.xcr0 = kX87 | kSseState;
  EXPECT_EQ(ChooseX86CpuKernels(no_ymm), X86CpuKernels::kX86_64_V2);
  X86CpuInfo no_sse_state = Haswell();
  no_sse_state.xcr0 = kX87 | kYmmState;
  EXPECT_EQ(ChooseX86CpuKernels(no_sse_state), X86CpuKernels::kX86_64_V2);
  // No OSXSAVE (XCR0 not readable) or no XSAVE: whatever XCR0 says.
  X86CpuInfo no_osxsave = Haswell();
  no_osxsave.leaf1_ecx &= ~kOsxsave;
  EXPECT_EQ(ChooseX86CpuKernels(no_osxsave), X86CpuKernels::kX86_64_V2);
  X86CpuInfo no_xsave = SandyBridge();
  no_xsave.leaf1_ecx &= ~kXsave;
  EXPECT_EQ(ChooseX86CpuKernels(no_xsave), X86CpuKernels::kX86_64_V2);
}

TEST(CpuKernelsTest, Avx2NeedsFmaF16cAndLeaf7) {
  // The AVX2 level is built with -mavx2 -mfma -mf16c: all three.
  X86CpuInfo no_fma = Haswell();
  no_fma.leaf1_ecx &= ~kFma;
  EXPECT_EQ(ChooseX86CpuKernels(no_fma), X86CpuKernels::kAvx);
  X86CpuInfo no_f16c = Haswell();
  no_f16c.leaf1_ecx &= ~kF16c;
  EXPECT_EQ(ChooseX86CpuKernels(no_f16c), X86CpuKernels::kAvx);
  // AVX2 is in leaf 7: a CPU (or hypervisor) reporting fewer leaves has
  // none, whatever the stale register says.
  X86CpuInfo low_leaf = Haswell();
  low_leaf.max_leaf = 6;
  EXPECT_EQ(ChooseX86CpuKernels(low_leaf), X86CpuKernels::kAvx);
  // AVX2 without AVX makes no sense, but isn't trusted.
  X86CpuInfo no_avx = Haswell();
  no_avx.leaf1_ecx &= ~kAvx;
  EXPECT_EQ(ChooseX86CpuKernels(no_avx), X86CpuKernels::kX86_64_V2);
}

TEST(CpuKernelsTest, NoLeafOne) {
  X86CpuInfo info = Haswell();
  info.max_leaf = 0;
  EXPECT_EQ(ChooseX86CpuKernels(info), std::nullopt);
}

TEST(CpuKernelsTest, Names) {
  EXPECT_STREQ(X86CpuKernelsName(X86CpuKernels::kX86_64_V2), "x86-64-v2");
  EXPECT_STREQ(X86CpuKernelsName(X86CpuKernels::kAvx), "avx");
  EXPECT_STREQ(X86CpuKernelsName(X86CpuKernels::kAvx2), "avx2");
}

TEST(CpuKernelsTest, Host) {
  const X86CpuInfo info = ReadX86CpuInfo();
#if defined(__x86_64__) || defined(_M_X64)
  // Every x86-64 CPU has leaf 1 and SSE3 at least; the choice is the pure
  // function's, and stays the same.
  EXPECT_GE(info.max_leaf, 1u);
  EXPECT_NE(info.leaf1_ecx & kSse3, 0u);
  EXPECT_EQ(HostX86CpuKernels(), ChooseX86CpuKernels(info));
  EXPECT_EQ(HostX86CpuKernels(), HostX86CpuKernels());
#else
  EXPECT_EQ(info.max_leaf, 0u);
  EXPECT_EQ(HostX86CpuKernels(), std::nullopt);
#endif
}

}  // namespace
}  // namespace molt_ai
