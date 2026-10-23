// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_proxy_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

ProfileProxyPrefs MoltNetsOwnProxy() {
  ProfileProxyPrefs prefs;
  prefs.proxy_is_default = false;
  prefs.proxy_is_user_controlled = true;
  return prefs;
}

TEST(MoltNetProxyPolicyTest, MoltNetsOwnProxyIsAccepted) {
  EXPECT_FALSE(ProxyPrefsOverrideMoltNet(MoltNetsOwnProxy()));
}

TEST(MoltNetProxyPolicyTest, DefaultProxyIsNotAnOverride) {
  // Before MoltNet writes its proxy (and in a regular profile nobody
  // configured), the setting has its default value.
  EXPECT_FALSE(ProxyPrefsOverrideMoltNet(ProfileProxyPrefs()));
}

TEST(MoltNetProxyPolicyTest, ProxyFromElsewhereIsAnOverride) {
  // A policy, an extension or a command-line flag outranks the user store.
  ProfileProxyPrefs prefs;
  prefs.proxy_is_default = false;
  prefs.proxy_is_user_controlled = false;
  EXPECT_TRUE(ProxyPrefsOverrideMoltNet(prefs));
}

TEST(MoltNetProxyPolicyTest, AnyOverrideRuleIsAnOverride) {
  for (size_t rules : {1u, 2u, 50u}) {
    ProfileProxyPrefs prefs = MoltNetsOwnProxy();
    prefs.override_rule_count = rules;
    EXPECT_TRUE(ProxyPrefsOverrideMoltNet(prefs)) << rules;
    // Also on top of the default proxy setting.
    ProfileProxyPrefs defaults;
    defaults.override_rule_count = rules;
    EXPECT_TRUE(ProxyPrefsOverrideMoltNet(defaults)) << rules;
  }
}

TEST(MoltNetProxyPolicyTest, AnEmptyRuleListIsNoOverride) {
  ProfileProxyPrefs prefs = MoltNetsOwnProxy();
  prefs.override_rule_count = 0;
  EXPECT_FALSE(ProxyPrefsOverrideMoltNet(prefs));
}

TEST(MoltNetProxyPolicyTest, IsAConstantExpression) {
  static_assert(!ProxyPrefsOverrideMoltNet(ProfileProxyPrefs()));
  static_assert(ProxyPrefsOverrideMoltNet(
      ProfileProxyPrefs{.proxy_is_default = true,
                        .proxy_is_user_controlled = false,
                        .override_rule_count = 1}));
}

}  // namespace
}  // namespace molt_ai::tor
