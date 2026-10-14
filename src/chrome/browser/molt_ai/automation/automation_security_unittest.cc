// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <string>
#include <utility>
#include <vector>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

using agent_safety::FieldKind;

Step MakeStep(StepType type, std::string target, std::string value = "") {
  Step s;
  s.type = type;
  s.target = std::move(target);
  s.value = std::move(value);
  return s;
}

SecurityPolicy CasualPolicy() {
  SecurityPolicy p;
  p.trust = TrustLevel::CASUAL;
  p.domain_whitelist = {"kayak.com"};
  p.require_approval_for = {"form_submit", "payment", "login"};
  return p;
}

TEST(AutomationSecurityTest, NavigationFollowsWhitelistBelowTrusted) {
  AutomationSecurity sec;
  SecurityPolicy p = CasualPolicy();
  EXPECT_TRUE(sec.AllowNavigation(p, "https://kayak.com/flights"));
  EXPECT_TRUE(sec.AllowNavigation(p, "https://www.kayak.com/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.example/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://notkayak.com/"));
  p.trust = TrustLevel::TRUSTED;
  EXPECT_TRUE(sec.AllowNavigation(p, "https://evil.example/"));
}

TEST(AutomationSecurityTest, SubmitNeedsApprovedTrust) {
  AutomationSecurity sec;
  SecurityPolicy p = CasualPolicy();
  EXPECT_FALSE(sec.AllowSubmit(p));
  p.trust = TrustLevel::APPROVED;
  EXPECT_TRUE(sec.AllowSubmit(p));
  p.trust = TrustLevel::TRUSTED;
  EXPECT_TRUE(sec.AllowSubmit(p));
  // Opting out of the category lifts the gate.
  SecurityPolicy no_submit_gate = CasualPolicy();
  no_submit_gate.require_approval_for = {"payment"};
  EXPECT_TRUE(sec.AllowSubmit(no_submit_gate));
}

TEST(AutomationSecurityTest, SensitiveFieldsNeedApprovedTrust) {
  AutomationSecurity sec;
  SecurityPolicy p = CasualPolicy();
  EXPECT_TRUE(sec.AllowSensitiveField(p, FieldKind::kOrdinary));
  EXPECT_FALSE(sec.AllowSensitiveField(p, FieldKind::kPassword));
  EXPECT_FALSE(sec.AllowSensitiveField(p, FieldKind::kPaymentCard));
  EXPECT_FALSE(sec.AllowSensitiveField(p, FieldKind::kOneTimeCode));
  p.trust = TrustLevel::APPROVED;
  EXPECT_TRUE(sec.AllowSensitiveField(p, FieldKind::kPassword));
  EXPECT_TRUE(sec.AllowSensitiveField(p, FieldKind::kPaymentCard));
}

TEST(AutomationSecurityTest, TypePreflightBlocksPasswordSelectorsWhenCasual) {
  AutomationSecurity sec;
  SecurityPolicy p = CasualPolicy();
  EXPECT_TRUE(sec.AllowType(p, "https://kayak.com/", "#q", "bali"));
  EXPECT_FALSE(sec.AllowType(p, "https://kayak.com/", "#password", ""));
  EXPECT_FALSE(
      sec.AllowType(p, "https://kayak.com/", "#cc", "4111 1111 1111 1111"));
  EXPECT_FALSE(sec.AllowType(p, "https://evil.example/", "#q", "bali"));
}

TEST(AutomationSecurityTest, EffectivePolicyAddsHostsTheStepsTarget) {
  Script script;
  // Studio-saved scripts were stored with an empty policy.
  script.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://WWW.Example.com/start"));
  script.steps.push_back(MakeStep(StepType::NAVIGATE, "{{next_url}}"));
  script.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://{{host}}.example.org/"));
  script.steps.push_back(MakeStep(StepType::NAVIGATE, "javascript:alert(1)"));
  Step click = MakeStep(StepType::CLICK, "#go");
  base::DictValue extra;
  extra.Set("host", "accounts.example.net");
  click.extra = base::Value(std::move(extra));
  script.steps.push_back(std::move(click));

  SecurityPolicy p = AutomationSecurity::EffectivePolicy(script);
  EXPECT_EQ(p.domain_whitelist,
            (std::vector<std::string>{"www.example.com",
                                      "accounts.example.net"}));
  EXPECT_EQ(p.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  EXPECT_EQ(p.trust, TrustLevel::CASUAL);

  AutomationSecurity sec;
  EXPECT_TRUE(sec.AllowNavigation(p, "https://www.example.com/x"));
  // A {{variable}} URL resolving elsewhere is still refused.
  EXPECT_FALSE(sec.AllowNavigation(p, "https://attacker.example/?d=secret"));
}

TEST(AutomationSecurityTest, EffectivePolicyKeepsStoredFields) {
  Script script;
  script.security.trust = TrustLevel::APPROVED;
  script.security.domain_whitelist = {"kayak.com"};
  script.security.require_approval_for = {"payment"};
  script.steps.push_back(MakeStep(StepType::NAVIGATE, "https://kayak.com/"));
  SecurityPolicy p = AutomationSecurity::EffectivePolicy(script);
  EXPECT_EQ(p.trust, TrustLevel::APPROVED);
  EXPECT_EQ(p.domain_whitelist, std::vector<std::string>{"kayak.com"});
  EXPECT_EQ(p.require_approval_for, std::vector<std::string>{"payment"});
}

TEST(AutomationSecurityTest, SavingNeverRaisesTrust) {
  Script incoming;
  incoming.security.trust = TrustLevel::ADMIN;  // claimed by UI JSON
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  EXPECT_EQ(incoming.security.trust, TrustLevel::CASUAL);

  Script existing;
  existing.security.trust = TrustLevel::APPROVED;  // set via promoteTrust
  incoming.security.trust = TrustLevel::ADMIN;
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_EQ(incoming.security.trust, TrustLevel::APPROVED);

  existing.security.trust = TrustLevel::CASUAL;
  incoming.security.trust = TrustLevel::TRUSTED;
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_EQ(incoming.security.trust, TrustLevel::CASUAL);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
