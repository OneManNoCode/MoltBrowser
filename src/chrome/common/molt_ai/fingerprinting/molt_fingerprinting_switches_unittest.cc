// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"

#include <optional>
#include <string>

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

TEST(MoltFingerprintingSwitchesTest, SeedRoundTrips) {
  SessionSeed seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(0xf0 ^ (i * 7));
  }
  const std::string hex = EncodeSeed(seed);
  EXPECT_EQ(hex.size(), 64u);
  EXPECT_EQ(hex.substr(0, 4), "f0f7");
  const std::optional<SessionSeed> decoded = DecodeSeed(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, seed);
}

TEST(MoltFingerprintingSwitchesTest, DecodeAcceptsUpperCase) {
  const std::optional<SessionSeed> decoded = DecodeSeed(std::string(64, 'A'));
  ASSERT_TRUE(decoded.has_value());
  for (uint8_t b : *decoded) {
    EXPECT_EQ(b, 0xaa);
  }
}

TEST(MoltFingerprintingSwitchesTest, DecodeRejectsBadSeeds) {
  EXPECT_FALSE(DecodeSeed("").has_value());
  EXPECT_FALSE(DecodeSeed(std::string(62, '0')).has_value());
  EXPECT_FALSE(DecodeSeed(std::string(66, '0')).has_value());
  std::string bad(64, '0');
  bad[17] = 'g';
  EXPECT_FALSE(DecodeSeed(bad).has_value());
  bad[17] = ' ';
  EXPECT_FALSE(DecodeSeed(bad).has_value());
}

}  // namespace
}  // namespace molt_ai::fingerprinting
