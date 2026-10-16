// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

TEST(MoltFingerprintingSwitchesTest, ModeRoundTrips) {
  for (RendererMode mode :
       {RendererMode::kOff, RendererMode::kFarble, RendererMode::kUniform}) {
    EXPECT_EQ(ModeFromSwitchValue(ModeToSwitchValue(mode)), mode);
  }
  EXPECT_EQ(ModeToSwitchValue(RendererMode::kFarble), "farble");
  EXPECT_EQ(ModeToSwitchValue(RendererMode::kUniform), "uniform");
  EXPECT_TRUE(ModeToSwitchValue(RendererMode::kOff).empty());
}

TEST(MoltFingerprintingSwitchesTest, UnknownModesAreOff) {
  EXPECT_EQ(ModeFromSwitchValue(""), RendererMode::kOff);
  EXPECT_EQ(ModeFromSwitchValue("Farble"), RendererMode::kOff);
  EXPECT_EQ(ModeFromSwitchValue("uniform "), RendererMode::kOff);
  EXPECT_EQ(ModeFromSwitchValue("off"), RendererMode::kOff);
}

TEST(MoltFingerprintingSwitchesTest, NoSeedSwitch) {
  // The session seed must never be put on a command line: the only switch
  // is the (non-secret) mode.
  EXPECT_STREQ(switches::kMode, "molt-fingerprinting-mode");
  EXPECT_EQ(kSessionSeedSize, 32u);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
