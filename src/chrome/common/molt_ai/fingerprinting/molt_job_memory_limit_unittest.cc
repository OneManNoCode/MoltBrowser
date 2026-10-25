// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Windows sandbox recognises MoltNet's renderers by the switch this
// directory defines (sandbox/policy/win/molt_job_memory_limit.h, which can't
// include it).

#include "sandbox/policy/win/molt_job_memory_limit.h"

#include <cstdint>
#include <string>

#include "base/command_line.h"
#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

base::CommandLine RendererWithMode(RendererMode mode) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  if (mode != RendererMode::kOff) {
    command_line.AppendSwitchASCII(switches::kMode,
                                   std::string(ModeToSwitchValue(mode)));
  }
  return command_line;
}

TEST(MoltJobMemoryLimitTest, NamesAgree) {
  EXPECT_STREQ(sandbox::policy::molt_win::kFingerprintingModeSwitch,
               switches::kMode);
  EXPECT_EQ(sandbox::policy::molt_win::kUniformMode,
            ModeToSwitchValue(RendererMode::kUniform));
}

TEST(MoltJobMemoryLimitTest, OnlyMoltNetRenderers) {
  using sandbox::policy::molt_win::IsMoltNetRenderer;
  EXPECT_TRUE(IsMoltNetRenderer(RendererWithMode(RendererMode::kUniform)));
  EXPECT_FALSE(IsMoltNetRenderer(RendererWithMode(RendererMode::kFarble)));
  EXPECT_FALSE(IsMoltNetRenderer(RendererWithMode(RendererMode::kOff)));
  base::CommandLine bogus(base::CommandLine::NO_PROGRAM);
  bogus.AppendSwitchASCII(switches::kMode, "Uniform");
  EXPECT_FALSE(IsMoltNetRenderer(bogus));
  // The limit Chromium gave renderers on machines up to 16 GB, and Linux
  // MoltNet renderers' data limit.
  EXPECT_EQ(sandbox::policy::molt_win::kMoltNetRendererJobMemoryLimit,
            uint64_t{8} << 30);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
