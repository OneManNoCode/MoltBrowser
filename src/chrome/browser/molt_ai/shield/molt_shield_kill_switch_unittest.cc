// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShield runtime kill switches (chrome/common/molt_ai/shield/
// molt_shield_features.h). The feature and flag names are user-facing
// (chrome://flags, --disable-features, ARCHITECTURE.md), so they are pinned
// here.

#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/memory/raw_ptr.h"
#include "base/test/scoped_command_line.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_shield {
namespace {

bool AllHooksEnabled() {
  return IsRequestProxyEnabled() && IsFrameBlockingEnabled() &&
         IsWebSocketEnabled() && IsKeepAliveEnabled() &&
         IsCosmeticsEnabled() && IsConsentKillerEnabled() &&
         IsRemoveParamEnabled();
}

bool AnyHookEnabled() {
  return IsRequestProxyEnabled() || IsFrameBlockingEnabled() ||
         IsWebSocketEnabled() || IsKeepAliveEnabled() ||
         IsCosmeticsEnabled() || IsConsentKillerEnabled() ||
         IsRemoveParamEnabled();
}

TEST(MoltShieldKillSwitchTest, FeatureNamesAreStable) {
  EXPECT_STREQ(kMoltShieldRequestProxy.name, "MoltShieldRequestProxy");
  EXPECT_STREQ(kMoltShieldFrameBlocking.name, "MoltShieldFrameBlocking");
  EXPECT_STREQ(kMoltShieldWebSocket.name, "MoltShieldWebSocket");
  EXPECT_STREQ(kMoltShieldKeepAlive.name, "MoltShieldKeepAlive");
  EXPECT_STREQ(kMoltShieldCosmetics.name, "MoltShieldCosmetics");
  EXPECT_STREQ(kMoltShieldConsentKiller.name, "MoltShieldConsentKiller");
  EXPECT_STREQ(kMoltShieldRemoveParam.name, "MoltShieldRemoveParam");
  EXPECT_STREQ(switches::kDisableMoltShield, "disable-molt-shield");
}

TEST(MoltShieldKillSwitchTest, EverythingOnByDefault) {
  EXPECT_FALSE(IsMoltShieldDisabledByCommandLine());
  EXPECT_TRUE(AllHooksEnabled());
  EXPECT_TRUE(FlagNamesOfDisabledHooks().empty());
}

TEST(MoltShieldKillSwitchTest, FeatureTurnsOffOnlyItsHook) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(kMoltShieldWebSocket);
  EXPECT_FALSE(IsWebSocketEnabled());
  EXPECT_TRUE(IsRequestProxyEnabled());
  EXPECT_TRUE(IsFrameBlockingEnabled());
  EXPECT_TRUE(IsKeepAliveEnabled());
  EXPECT_TRUE(IsCosmeticsEnabled());
  EXPECT_TRUE(IsConsentKillerEnabled());
  EXPECT_TRUE(IsRemoveParamEnabled());
  EXPECT_FALSE(IsMoltShieldDisabledByCommandLine());
  EXPECT_EQ(FlagNamesOfDisabledHooks(),
            std::vector<std::string_view>{"molt-shield-websocket"});
}

TEST(MoltShieldKillSwitchTest, EachFeatureMapsToItsFlag) {
  struct {
    raw_ptr<const base::Feature> feature;
    std::string_view flag;
    bool (*enabled)();
  } const kCases[] = {
      {&kMoltShieldRequestProxy, "molt-shield-request-proxy",
       &IsRequestProxyEnabled},
      {&kMoltShieldFrameBlocking, "molt-shield-frame-blocking",
       &IsFrameBlockingEnabled},
      {&kMoltShieldWebSocket, "molt-shield-websocket", &IsWebSocketEnabled},
      {&kMoltShieldKeepAlive, "molt-shield-keepalive", &IsKeepAliveEnabled},
      {&kMoltShieldCosmetics, "molt-shield-cosmetics", &IsCosmeticsEnabled},
      {&kMoltShieldConsentKiller, "molt-shield-consent-killer",
       &IsConsentKillerEnabled},
      {&kMoltShieldRemoveParam, "molt-shield-removeparam",
       &IsRemoveParamEnabled},
  };
  for (const auto& c : kCases) {
    SCOPED_TRACE(c.flag);
    base::test::ScopedFeatureList features;
    features.InitAndDisableFeature(*c.feature);
    EXPECT_FALSE(c.enabled());
    EXPECT_EQ(FlagNamesOfDisabledHooks(),
              std::vector<std::string_view>{c.flag});
  }
}

TEST(MoltShieldKillSwitchTest, CommandLineSwitchTurnsEverythingOff) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitch(
      switches::kDisableMoltShield);
  EXPECT_TRUE(IsMoltShieldDisabledByCommandLine());
  EXPECT_FALSE(AnyHookEnabled());
  // Even with every feature explicitly enabled.
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {kMoltShieldRequestProxy, kMoltShieldFrameBlocking, kMoltShieldWebSocket,
       kMoltShieldKeepAlive, kMoltShieldCosmetics, kMoltShieldConsentKiller,
       kMoltShieldRemoveParam},
      {});
  EXPECT_FALSE(AnyHookEnabled());
}

}  // namespace
}  // namespace molt_shield
