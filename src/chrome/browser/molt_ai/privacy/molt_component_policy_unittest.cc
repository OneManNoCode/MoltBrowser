// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_component_policy.h"

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace privacy {
namespace {

// registration.cc skips the actor safety lists, CAPTCHA providers, amount
// extraction regexes and commerce heuristics by default.
TEST(MoltComponentPolicyTest, GoogleOnlyComponentsAreSkippedByDefault) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithEmptyFeatureAndFieldTrialLists();
  EXPECT_FALSE(ShouldRegisterGoogleOnlyComponents());
}

// --enable-features=MoltGoogleOnlyComponents brings them back.
TEST(MoltComponentPolicyTest, CommandLineCanRegisterThem) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitFromCommandLine("MoltGoogleOnlyComponents", "");
  EXPECT_TRUE(ShouldRegisterGoogleOnlyComponents());
}

// The feature's name is what the docs and the command line use.
TEST(MoltComponentPolicyTest, FeatureName) {
  EXPECT_STREQ(kMoltGoogleOnlyComponents.name, "MoltGoogleOnlyComponents");
}

}  // namespace
}  // namespace privacy
}  // namespace molt_ai
