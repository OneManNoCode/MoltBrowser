// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_cpu.h"

#include "base/cpu.h"
#include "base/no_destructor.h"
#include "build/build_config.h"

namespace molt_ai::tor {

MoltNetCpuFeatures ReadMoltNetCpuFeatures() {
  MoltNetCpuFeatures features;
#if defined(ARCH_CPU_X86_64)
  // The CPUID bits V8 reads for the same features (v8/src/base/cpu).
  const base::CPU& cpu = base::CPU::GetInstanceNoAllocation();
  features.arch = MoltNetCpuArch::kX86_64;
  features.sse3 = cpu.has_sse3();
  features.ssse3 = cpu.has_ssse3();
  features.sse41 = cpu.has_sse41();
#elif defined(ARCH_CPU_ARM64)
  features.arch = MoltNetCpuArch::kArm64;
#endif
  return features;
}

const std::string& MoltNetCpuError() {
  static const base::NoDestructor<std::string> error(
      MoltNetCpuMessage(CheckMoltNetCpu(ReadMoltNetCpuFeatures())));
  return *error;
}

}  // namespace molt_ai::tor
