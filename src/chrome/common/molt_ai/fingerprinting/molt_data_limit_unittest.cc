// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Linux sandbox recognises MoltNet's renderers by the switch this
// directory defines (sandbox/policy/linux/molt_data_limit.h, which can't
// include it).

#include "sandbox/policy/linux/molt_data_limit.h"

#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr uint64_t kGiB = uint64_t{1} << 30;

base::CommandLine RendererWithMode(RendererMode mode) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  if (mode != RendererMode::kOff) {
    command_line.AppendSwitchASCII(switches::kMode,
                                   std::string(ModeToSwitchValue(mode)));
  }
  return command_line;
}

TEST(MoltDataLimitTest, NamesAgree) {
  EXPECT_STREQ(sandbox::policy::molt::kFingerprintingModeSwitch,
               switches::kMode);
  EXPECT_EQ(sandbox::policy::molt::kUniformMode,
            ModeToSwitchValue(RendererMode::kUniform));
}

TEST(MoltDataLimitTest, MoltNetRenderersGetTheSmallestTier) {
  using sandbox::policy::molt::ProcessDataSizeLimit;
  const base::CommandLine moltnet = RendererWithMode(RendererMode::kUniform);
  // Chromium's tiers: 8 GiB (up to 16 GB), 16, 32, and doubled.
  for (const uint64_t chromium : {8 * kGiB, 16 * kGiB, 32 * kGiB, 64 * kGiB}) {
    EXPECT_EQ(ProcessDataSizeLimit(moltnet, /*is_renderer=*/true, chromium),
              8 * kGiB);
  }
  // A smaller limit (32-bit) is kept.
  EXPECT_EQ(ProcessDataSizeLimit(moltnet, true, 2 * kGiB), 2 * kGiB);
  // Other processes of a MoltNet window keep theirs.
  EXPECT_EQ(ProcessDataSizeLimit(moltnet, /*is_renderer=*/false, 64 * kGiB),
            64 * kGiB);
}

TEST(MoltDataLimitTest, OtherRenderersKeepChromiums) {
  using sandbox::policy::molt::ProcessDataSizeLimit;
  for (const RendererMode mode : {RendererMode::kOff, RendererMode::kFarble}) {
    const base::CommandLine renderer = RendererWithMode(mode);
    EXPECT_EQ(ProcessDataSizeLimit(renderer, true, 32 * kGiB), 32 * kGiB);
  }
  base::CommandLine bogus(base::CommandLine::NO_PROGRAM);
  bogus.AppendSwitchASCII(switches::kMode, "Uniform");
  EXPECT_EQ(ProcessDataSizeLimit(bogus, true, 32 * kGiB), 32 * kGiB);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
