// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_feature_overrides.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/feature_list.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/dictation/features.h"
#include "chrome/common/chrome_features.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/history_embeddings/core/history_embeddings_features.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/optimization_guide/core/delivery/prediction_model_component_configs.h"
#include "components/translate/core/browser/translate_ranker_impl.h"
#include "third_party/blink/public/common/features.h"
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

// Everything is forced off except Global Privacy Control and the Writer and
// Rewriter web APIs, which are forced on.
TEST(MoltFeatureOverridesTest, OnlyTheListedFeaturesAreEnabled) {
  std::vector<base::FeatureList::FeatureOverrideInfo> overrides;
  AppendDefaultFeatureOverrides(overrides);
  ASSERT_FALSE(overrides.empty());
  for (const auto& [feature, state] : overrides) {
    const bool enabled =
        &feature.get() == &blink::features::kGlobalPrivacyControlForce ||
        &feature.get() == &blink::features::kAIWriterAPI ||
        &feature.get() == &blink::features::kAIRewriterAPI;
    EXPECT_EQ(state, enabled ? base::FeatureList::OVERRIDE_ENABLE_FEATURE
                             : base::FeatureList::OVERRIDE_DISABLE_FEATURE)
        << feature.get().name;
  }
}

// Writer and Rewriter are on, so Blink exposes them to every page (they
// are origin-trial features at 152); MoltBrowser's own model serves them.
TEST(MoltFeatureOverridesTest, ExposesWriterAndRewriter) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList("", ""));
  EXPECT_TRUE(base::FeatureList::IsEnabled(blink::features::kAIWriterAPI));
  EXPECT_TRUE(base::FeatureList::IsEnabled(blink::features::kAIRewriterAPI));
  // Counting as overridden is what turns the Blink features on.
  EXPECT_TRUE(base::FeatureList::GetInstance()->IsFeatureOverridden(
      blink::features::kAIWriterAPI.name));
}

TEST(MoltFeatureOverridesTest, WriterCanBeTurnedOff) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(
      BuildFeatureList("", blink::features::kAIWriterAPI.name));
  EXPECT_FALSE(base::FeatureList::IsEnabled(blink::features::kAIWriterAPI));
}

TEST(MoltFeatureOverridesTest, SendsGlobalPrivacyControl) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList("", ""));
  EXPECT_TRUE(base::FeatureList::IsEnabled(
      blink::features::kGlobalPrivacyControlForce));
}

// chrome://flags writes --disable-features, which must still win.
TEST(MoltFeatureOverridesTest, GlobalPrivacyControlCanBeTurnedOff) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList(
      "", blink::features::kGlobalPrivacyControlForce.name));
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      blink::features::kGlobalPrivacyControlForce));
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

// Components and models that only serve Google features (Chromium 156
// audit, docs/NETWORK_TRAFFIC.md).
TEST(MoltFeatureOverridesTest, DisablesGoogleOnlyComponents) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList("", ""));
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kIndigo));
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kIndigoComponent));
  EXPECT_FALSE(base::FeatureList::IsEnabled(dictation::kDictation));
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(history_embeddings::kHistoryEmbeddings));
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      optimization_guide::kPredictionModelComponentDelivery));
  EXPECT_FALSE(base::FeatureList::IsEnabled(
      omnibox::kOnDeviceHeadProviderNonIncognito));
  EXPECT_FALSE(
      base::FeatureList::IsEnabled(omnibox::kOnDeviceHeadProviderIncognito));
  // No prediction model target is delivered as a component, so the
  // permission and phishing models stay with the key-gated download path.
  EXPECT_TRUE(optimization_guide::GetPredictionModelTargets().empty());
}

// History embeddings decides on "overridden" first, then on the country and
// locale; the override must make it count as overridden (and off).
TEST(MoltFeatureOverridesTest, HistoryEmbeddingsCountsAsOverriddenOff) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(BuildFeatureList("", ""));
  std::optional<bool> state = base::FeatureList::GetStateIfOverridden(
      history_embeddings::kHistoryEmbeddings);
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(*state);
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

TEST(MoltFeatureOverridesTest, CommandLineCanTurnDictationBackOn) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatureList(
      BuildFeatureList(dictation::kDictation.name, ""));
  EXPECT_TRUE(base::FeatureList::IsEnabled(dictation::kDictation));
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kIndigoComponent));
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
