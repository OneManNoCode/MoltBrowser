// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_cpu_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

MoltNetCpuFeatures X86_64(bool sse3, bool ssse3, bool sse41) {
  MoltNetCpuFeatures cpu;
  cpu.arch = MoltNetCpuArch::kX86_64;
  cpu.sse3 = sse3;
  cpu.ssse3 = ssse3;
  cpu.sse41 = sse41;
  return cpu;
}

MoltNetCpuFeatures Arm64() {
  MoltNetCpuFeatures cpu;
  cpu.arch = MoltNetCpuArch::kArm64;
  return cpu;
}

// Nobody filled the features in: refuse rather than assume.
TEST(MoltNetCpuPolicyTest, DefaultRefuses) {
  EXPECT_EQ(CheckMoltNetCpu(MoltNetCpuFeatures()),
            MoltNetCpuCheck::kUnsupportedArch);
}

// What base::CPU reports on the processors V8's own probe was run on under
// qemu (docs/FINGERPRINTING.md): Core 2 (65 nm), AMD K10 and Bobcat have no
// SSE4.1; Penryn (Core 2, 45 nm) has SSE4.1 but not SSE4.2.
TEST(MoltNetCpuPolicyTest, X86_64NeedsSse41) {
  EXPECT_EQ(CheckMoltNetCpu(X86_64(true, true, true)), MoltNetCpuCheck::kOk);
  // Core 2 (Merom, Conroe), AMD Bobcat.
  EXPECT_EQ(CheckMoltNetCpu(X86_64(true, true, false)),
            MoltNetCpuCheck::kNoSse41);
  // AMD K8 and K10, Pentium 4 with x86-64.
  EXPECT_EQ(CheckMoltNetCpu(X86_64(true, false, false)),
            MoltNetCpuCheck::kNoSse41);
}

// V8 only uses SSE4.1 when SSSE3 and SSE3 are there too
// (CpuFeatures::ProbeImpl), so a processor (or a virtual machine) reporting
// SSE4.1 without them has no WebAssembly SIMD either.
TEST(MoltNetCpuPolicyTest, X86_64NeedsWhatV8NeedsForSse41) {
  EXPECT_EQ(CheckMoltNetCpu(X86_64(true, false, true)),
            MoltNetCpuCheck::kNoSse41);
  EXPECT_EQ(CheckMoltNetCpu(X86_64(false, true, true)),
            MoltNetCpuCheck::kNoSse41);
  EXPECT_EQ(CheckMoltNetCpu(X86_64(false, false, true)),
            MoltNetCpuCheck::kNoSse41);
  EXPECT_EQ(CheckMoltNetCpu(X86_64(false, false, false)),
            MoltNetCpuCheck::kNoSse41);
}

// Every arm64 processor: Wasm SIMD is always there, and the features V8
// probes (dot product, FP16, JSCVT, LSE, SVE, ...) change no answer.
TEST(MoltNetCpuPolicyTest, Arm64Opens) {
  EXPECT_EQ(CheckMoltNetCpu(Arm64()), MoltNetCpuCheck::kOk);
  // The x86-64 fields mean nothing on arm64.
  MoltNetCpuFeatures cpu = Arm64();
  cpu.sse41 = true;
  EXPECT_EQ(CheckMoltNetCpu(cpu), MoltNetCpuCheck::kOk);
}

TEST(MoltNetCpuPolicyTest, OtherArchitecturesRefuse) {
  MoltNetCpuFeatures cpu;
  cpu.arch = MoltNetCpuArch::kOther;
  cpu.sse3 = cpu.ssse3 = cpu.sse41 = true;
  EXPECT_EQ(CheckMoltNetCpu(cpu), MoltNetCpuCheck::kUnsupportedArch);
}

// One plain sentence for each refusal, none for kOk.
TEST(MoltNetCpuPolicyTest, Messages) {
  EXPECT_EQ(MoltNetCpuMessage(MoltNetCpuCheck::kOk), "");
  for (MoltNetCpuCheck check :
       {MoltNetCpuCheck::kNoSse41, MoltNetCpuCheck::kUnsupportedArch}) {
    const std::string message = MoltNetCpuMessage(check);
    ASSERT_FALSE(message.empty());
    EXPECT_EQ(message.rfind("MoltNet can't open on this computer: ", 0), 0u)
        << message;
    EXPECT_EQ(message.back(), '.') << message;
    // One sentence: no full stop before the end.
    EXPECT_EQ(message.find(". "), std::string::npos) << message;
    EXPECT_EQ(message.find('\n'), std::string::npos) << message;
  }
  EXPECT_NE(MoltNetCpuMessage(MoltNetCpuCheck::kNoSse41).find("SSE4.1"),
            std::string::npos);
  EXPECT_NE(MoltNetCpuMessage(MoltNetCpuCheck::kUnsupportedArch).find("arm64"),
            std::string::npos);
}

// The check is a constant expression, so it can't depend on anything but
// its argument.
static_assert(CheckMoltNetCpu(MoltNetCpuFeatures{
                  .arch = MoltNetCpuArch::kX86_64,
                  .sse3 = true,
                  .ssse3 = true,
                  .sse41 = true}) == MoltNetCpuCheck::kOk);
static_assert(CheckMoltNetCpu(MoltNetCpuFeatures{
                  .arch = MoltNetCpuArch::kX86_64,
                  .sse3 = true,
                  .ssse3 = true,
                  .sse41 = false}) == MoltNetCpuCheck::kNoSse41);

}  // namespace
}  // namespace molt_ai::tor
