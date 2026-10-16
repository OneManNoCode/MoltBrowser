// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_prefs.h"

#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::shield::prefs {
namespace {

TEST(MoltShieldPrefsTest, RemoveTrackingParamsDefaultsOn) {
  TestingPrefServiceSimple prefs;
  RegisterProfilePrefs(prefs.registry());
  EXPECT_TRUE(prefs.GetBoolean(kRemoveTrackingParams));
  EXPECT_TRUE(IsRemoveTrackingParamsOn(&prefs));
}

TEST(MoltShieldPrefsTest, RemoveTrackingParamsFollowsTheUser) {
  TestingPrefServiceSimple prefs;
  RegisterProfilePrefs(prefs.registry());
  prefs.SetBoolean(kRemoveTrackingParams, false);
  EXPECT_FALSE(IsRemoveTrackingParamsOn(&prefs));
  prefs.SetBoolean(kRemoveTrackingParams, true);
  EXPECT_TRUE(IsRemoveTrackingParamsOn(&prefs));
}

TEST(MoltShieldPrefsTest, MissingPrefsCountAsOn) {
  // Stripping is the privacy-preserving side: without the pref (not
  // registered, no profile) it stays on.
  EXPECT_TRUE(IsRemoveTrackingParamsOn(nullptr));
  TestingPrefServiceSimple unregistered;
  EXPECT_TRUE(IsRemoveTrackingParamsOn(&unregistered));
}

}  // namespace
}  // namespace molt_ai::shield::prefs
