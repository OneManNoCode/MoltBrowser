// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_feature_overrides.h"

#include <memory>
#include <string>
#include <vector>

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/translate/core/browser/translate_ranker_impl.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace privacy {
namespace {

// Same order as VariationsFieldTrialCreator::SetUpFieldTrials(): the command
// line first, then the extra overrides (the field trial testing config would
// come after both).
std::unique_ptr<base::FeatureList> BuildFeatureList(
    const std::string& enable_features,
    const std::string& disable_features) {
  auto feature_list = std::make_unique<base::FeatureList>();
  feature_list->InitFromCommandLine(enable_features, disable_features);
  std::vector<base::FeatureList::FeatureOverrideInfo> overrides;
  AppendDefaultFeatureOverrides(overrides);
  feature_list->RegisterExtraFeatureOverrides(overrides);
  return feature_list;
}

TEST(MoltFeatureOverridesTest, OnlyDisablesFeatures) {
  std::vector<base::FeatureList::FeatureOverrideInfo> overrides;
  AppendDefaultFeatureOverrides(overrides);
  ASSERT_FALSE(overrides.empty());
  for (const auto& [feature, state] : overrides) {
    EXPECT_EQ(state, base::FeatureList::OVERRIDE_DISABLE_FEATURE)
        << feature.get().name;
  }
}

TEST(MoltFeatureOverridesTest, DisablesKeylessGoogleRequests) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList("", ""));
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      autofill::features::debug::kAutofillServerCommunication));
  EXPECT_FALSE(base::FeatureList::IsEnabled(translate::kTranslateRankerQuery));
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(translate::kTranslateRankerEnforcement));
  EXPECT_FALSE(base::FeatureList::IsEnabled(omnibox::kAimEnabled));
  EXPECT_FALSE(base::FeatureList::IsEnabled(omnibox::kAim3pEntrypoint));
}

// --enable-features, which chrome://flags also writes, must still win.
TEST(MoltFeatureOverridesTest, CommandLineStillWins) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList(
      autofill::features::debug::kAutofillServerCommunication.name, ""));
  EXPECT_TRUE(base::FeatureList::IsEnabled(
      autofill::features::debug::kAutofillServerCommunication));
  EXPECT_FALSE(base::FeatureList::IsEnabled(translate::kTranslateRankerQuery));
}

// The field trial testing config skips any study whose features are already
// overridden, so every listed feature must count as overridden.
TEST(MoltFeatureOverridesTest, ListedFeaturesCountAsOverridden) {
  std::unique_ptr<base::FeatureList> feature_list = BuildFeatureList("", "");
  std::vector<base::FeatureList::FeatureOverrideInfo> overrides;
  AppendDefaultFeatureOverrides(overrides);
  for (const auto& [feature, state] : overrides) {
    EXPECT_TRUE(feature_list->IsFeatureOverridden(feature.get().name))
        << feature.get().name;
  }
}

}  // namespace
}  // namespace privacy
}  // namespace molt_ai
