// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_cpu.h"

#include "base/cpu.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

TEST(MoltNetCpuErrorTest, ReadsWhatBaseCpuReports) {
  const MoltNetCpuFeatures features = ReadMoltNetCpuFeatures();
#if defined(ARCH_CPU_X86_64)
  const base::CPU& cpu = base::CPU::GetInstanceNoAllocation();
  EXPECT_EQ(features.arch, MoltNetCpuArch::kX86_64);
  EXPECT_EQ(features.sse3, cpu.has_sse3());
  EXPECT_EQ(features.ssse3, cpu.has_ssse3());
  EXPECT_EQ(features.sse41, cpu.has_sse41());
#elif defined(ARCH_CPU_ARM64)
  EXPECT_EQ(features.arch, MoltNetCpuArch::kArm64);
#else
  EXPECT_EQ(features.arch, MoltNetCpuArch::kOther);
#endif
}

// The cached answer is the policy's for this machine, and the same object
// every time.
TEST(MoltNetCpuErrorTest, ErrorIsThePolicysAndCached) {
  EXPECT_EQ(MoltNetCpuError(),
            MoltNetCpuMessage(CheckMoltNetCpu(ReadMoltNetCpuFeatures())));
  EXPECT_EQ(&MoltNetCpuError(), &MoltNetCpuError());
}

}  // namespace
}  // namespace molt_ai::tor
