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

// ---- Drafts from web agent runs (agent_run_draft.h) --------------------------

Script UnconfirmedDraft() {
  Script draft;
  draft.id = "agent_1";
  draft.security.sites_unconfirmed = true;
  draft.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://shop.example/home"));
  Step click = MakeStep(StepType::CLICK, "#buy");
  base::DictValue extra;
  extra.Set("host", "pay.example");
  click.extra = base::Value(std::move(extra));
  draft.steps.push_back(std::move(click));
  return draft;
}

TEST(AutomationSecurityTest, AnUnconfirmedDraftMayDoNothing) {
  Script draft = UnconfirmedDraft();
  // Trust set on it meanwhile (molt://ai-automation) doesn't count.
  draft.security.trust = TrustLevel::TRUSTED;
  SecurityPolicy p = AutomationSecurity::EffectivePolicy(draft);
  EXPECT_EQ(p.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(p.domain_whitelist.empty());
  EXPECT_TRUE(p.sites_unconfirmed);
  AutomationSecurity sec;
  EXPECT_FALSE(sec.AllowNavigation(p, "https://shop.example/home"));
  EXPECT_FALSE(sec.AllowClick(p, "https://pay.example/", "#buy"));
  EXPECT_FALSE(sec.AllowType(p, "https://shop.example/", "#q", "x"));
  EXPECT_FALSE(sec.AllowSubmit(p));
  EXPECT_FALSE(sec.AllowHeadless(p));
}

TEST(AutomationSecurityTest, SavingNeverConfirmsADraft) {
  Script existing = UnconfirmedDraft();
  Script incoming = UnconfirmedDraft();
  incoming.security.sites_unconfirmed = false;  // claimed by UI JSON
  incoming.security.domain_whitelist = {"evil.example"};
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_TRUE(incoming.security.sites_unconfirmed);
  EXPECT_EQ(incoming.security.trust, TrustLevel::CASUAL);
  // A confirmed workflow stays confirmed; a new one is what it says.
  existing.security.sites_unconfirmed = false;
  incoming.security.sites_unconfirmed = false;
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_FALSE(incoming.security.sites_unconfirmed);
  Script fresh;
  AutomationSecurity::ClampTrustForSave(fresh, nullptr);
  EXPECT_FALSE(fresh.security.sites_unconfirmed);
}

TEST(AutomationSecurityTest, ConfirmDraftSitesNeedsWhatTheUserSaw) {
  Script draft = UnconfirmedDraft();
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(draft, {}));
  EXPECT_FALSE(
      AutomationSecurity::ConfirmDraftSites(draft, {"shop.example"}));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(
      draft, {"shop.example", "pay.example", "evil.example"}));
  EXPECT_TRUE(draft.security.sites_unconfirmed);
  EXPECT_TRUE(draft.security.domain_whitelist.empty());
  // Any order.
  EXPECT_TRUE(AutomationSecurity::ConfirmDraftSites(
      draft, {"pay.example", "shop.example"}));
  EXPECT_FALSE(draft.security.sites_unconfirmed);
  EXPECT_EQ(draft.security.domain_whitelist,
            (std::vector<std::string>{"shop.example", "pay.example"}));
  EXPECT_EQ(draft.security.trust, TrustLevel::CASUAL);
  // Only once, and only for a draft.
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(
      draft, {"pay.example", "shop.example"}));
  // A draft with no site at all can't be confirmed.
  Script empty;
  empty.security.sites_unconfirmed = true;
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(empty, {}));
  // Once confirmed it is an ordinary CASUAL workflow on its sites.
  SecurityPolicy p = AutomationSecurity::EffectivePolicy(draft);
  AutomationSecurity sec;
  EXPECT_TRUE(sec.AllowNavigation(p, "https://shop.example/home"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.example/"));
  EXPECT_FALSE(sec.AllowSubmit(p));
}

TEST(AutomationSecurityTest, TheDraftMarkIsStoredOnlyWhenSet) {
  Script draft = UnconfirmedDraft();
  const std::string json = draft.ToJSONString();
  EXPECT_NE(json.find("\"sites_unconfirmed\": true"), std::string::npos);
  std::optional<Script> loaded = Script::FromJSONString(json);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->security.sites_unconfirmed);
  draft.security.sites_unconfirmed = false;
  EXPECT_EQ(draft.ToJSONString().find("sites_unconfirmed"),
            std::string::npos);
}

// ---- Secret steps (S7) ----------------------------------------------------

TEST(AutomationSecurityTest, MarkStepSecretDropsValueKeepsExtra) {
  Step s = MakeStep(StepType::TYPE, "#pw", "hunter2");
  base::DictValue extra;
  extra.Set("text", "Password");
  s.extra = base::Value(std::move(extra));
  AutomationSecurity::MarkStepSecret(s, FieldKind::kPassword, "example.com");
  EXPECT_TRUE(s.value.empty());
  ASSERT_TRUE(s.extra.is_dict());
  EXPECT_EQ(*s.extra.GetDict().FindString(kSecretKey), kSecretVault);
  EXPECT_EQ(*s.extra.GetDict().FindString(kSecretKindKey), "password");
  EXPECT_EQ(*s.extra.GetDict().FindString(kSecretHostKey), "example.com");
  EXPECT_EQ(*s.extra.GetDict().FindString("text"), "Password");
  EXPECT_EQ(AutomationSecurity::SecretStepKind(s), FieldKind::kPassword);

  Step card = MakeStep(StepType::TYPE, "#cc");
  AutomationSecurity::MarkStepSecret(card, FieldKind::kPaymentCard, "");
  EXPECT_EQ(AutomationSecurity::SecretStepKind(card), FieldKind::kPaymentCard);
  EXPECT_EQ(card.extra.GetDict().FindString(kSecretHostKey), nullptr);

  EXPECT_EQ(AutomationSecurity::SecretStepKind(MakeStep(StepType::TYPE, "#q")),
            FieldKind::kOrdinary);
}

base::DictValue TypeStepJson(std::string target,
                             std::string value,
                             std::string description) {
  base::DictValue d;
  d.Set("type", "type");
  d.Set("target", target);
  d.Set("value", value);
  d.Set("description", description);
  return d;
}

TEST(AutomationSecurityTest, ScrubMigratesLegacyPasswordSteps) {
  base::DictValue script;
  base::ListValue steps;
  // Old recorder: masked description, real password in "value".
  base::DictValue legacy = TypeStepJson(
      "#f2", "hunter2",
      "Type \"\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
      "\xE2\x80\xA2\" in the \"Password\" field");
  base::DictValue extra;
  extra.Set("host", "accounts.example.com");
  legacy.Set("extra", std::move(extra));
  steps.Append(std::move(legacy));
  // Selector names the password field; description quotes the value.
  steps.Append(TypeStepJson("[name=\"passwd\"]", "s3cret!",
                            "Type \"s3cret!\" in the field"));
  // Ordinary field: untouched.
  steps.Append(TypeStepJson("#q", "flights to bali", "Type a query"));
  script.Set("steps", std::move(steps));

  base::DictValue stats;
  base::ListValue history;
  base::DictValue run;
  base::ListValue run_steps;
  run_steps.Append(base::DictValue().Set("desc", "Type \"s3cret!\" in the field"));
  run_steps.Append(base::DictValue().Set("desc", "Type a query"));
  run.Set("steps", std::move(run_steps));
  history.Append(std::move(run));
  stats.Set("run_history", std::move(history));
  script.Set("stats", std::move(stats));

  ASSERT_TRUE(AutomationSecurity::ScrubPlaintextSecrets(script));

  const base::ListValue& out = *script.FindList("steps");
  const base::DictValue& s0 = out[0].GetDict();
  EXPECT_EQ(s0.FindString("value"), nullptr);
  EXPECT_EQ(*s0.FindDict("extra")->FindString(kSecretKey), kSecretVault);
  EXPECT_EQ(*s0.FindDict("extra")->FindString(kSecretKindKey), "password");
  EXPECT_EQ(*s0.FindDict("extra")->FindString(kSecretHostKey),
            "accounts.example.com");

  const base::DictValue& s1 = out[1].GetDict();
  EXPECT_EQ(s1.FindString("value"), nullptr);
  EXPECT_EQ(s1.FindString("description")->find("s3cret!"), std::string::npos);

  const base::DictValue& s2 = out[2].GetDict();
  EXPECT_EQ(*s2.FindString("value"), "flights to bali");
  EXPECT_EQ(s2.FindDict("extra"), nullptr);

  const base::ListValue& hist_steps = *script.FindDict("stats")
                                           ->FindList("run_history")
                                           ->operator[](0)
                                           .GetDict()
                                           .FindList("steps");
  EXPECT_EQ(*hist_steps[0].GetDict().FindString("desc"), "[redacted]");
  EXPECT_EQ(*hist_steps[1].GetDict().FindString("desc"), "Type a query");

  // Idempotent: a second pass finds nothing left to remove.
  EXPECT_FALSE(AutomationSecurity::ScrubPlaintextSecrets(script));
}

TEST(AutomationSecurityTest, ScrubDropsValueFromAlreadySecretStep) {
  base::DictValue script;
  base::ListValue steps;
  base::DictValue step = TypeStepJson("#f9", "typed-in-editor", "Fill password");
  base::DictValue extra;
  extra.Set(kSecretKey, kSecretVault);
  extra.Set(kSecretKindKey, "password");
  step.Set("extra", std::move(extra));
  steps.Append(std::move(step));
  script.Set("steps", std::move(steps));
  EXPECT_TRUE(AutomationSecurity::ScrubPlaintextSecrets(script));
  EXPECT_EQ(script.FindList("steps")->operator[](0).GetDict().FindString(
                "value"),
            nullptr);
}

TEST(AutomationSecurityTest, ScrubLeavesOrdinaryScriptsAlone) {
  base::DictValue script;
  base::ListValue steps;
  steps.Append(TypeStepJson("#email", "me@example.com", "Type email"));
  steps.Append(base::DictValue().Set("type", "click").Set("target", "#pwd"));
  script.Set("steps", std::move(steps));
  EXPECT_FALSE(AutomationSecurity::ScrubPlaintextSecrets(script));
  base::DictValue empty;
  EXPECT_FALSE(AutomationSecurity::ScrubPlaintextSecrets(empty));
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
