// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_writer.h"
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

// A routine taught by voice (automation_voice_steps.h), as a file brings it.
Script VoiceRoutine() {
  Script routine;
  routine.id = "voice_1";
  Step open = MakeStep(StepType::NAVIGATE, "https://evil.example/");
  base::DictValue voice;
  voice.Set("command", "open site");
  base::DictValue extra;
  extra.Set("voice", std::move(voice));
  open.extra = base::Value(std::move(extra));
  routine.steps.push_back(std::move(open));
  routine.security.trust = TrustLevel::APPROVED;
  routine.security.domain_whitelist = {"evil.example"};
  return routine;
}

TEST(AutomationSecurityTest, ABroughtInVoiceRoutineIsADraft) {
  // Imported or copied: whatever it claims, its sites aren't confirmed.
  Script incoming = VoiceRoutine();
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  EXPECT_TRUE(incoming.security.sites_unconfirmed);
  EXPECT_TRUE(incoming.security.domain_whitelist.empty());
  EXPECT_EQ(incoming.security.trust, TrustLevel::CASUAL);
  SecurityPolicy p = AutomationSecurity::EffectivePolicy(incoming);
  AutomationSecurity sec;
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.example/"));
  // Voice steps added to a workflow that had none: the same.
  Script plain;
  plain.security.trust = TrustLevel::APPROVED;
  plain.security.domain_whitelist = {"evil.example"};
  incoming = VoiceRoutine();
  AutomationSecurity::ClampTrustForSave(incoming, &plain);
  EXPECT_TRUE(incoming.security.sites_unconfirmed);
  EXPECT_TRUE(incoming.security.domain_whitelist.empty());
  // A voice routine edited where it lives keeps what it had.
  const Script taught = VoiceRoutine();
  incoming = VoiceRoutine();
  AutomationSecurity::ClampTrustForSave(incoming, &taught);
  EXPECT_FALSE(incoming.security.sites_unconfirmed);
  EXPECT_EQ(incoming.security.trust, TrustLevel::APPROVED);
  EXPECT_EQ(incoming.security.domain_whitelist,
            std::vector<std::string>{"evil.example"});
  // Its sites, once confirmed, are what it may use.
  incoming = VoiceRoutine();
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  EXPECT_TRUE(
      AutomationSecurity::ConfirmDraftSites(incoming, {"evil.example"}));
  EXPECT_TRUE(sec.AllowNavigation(AutomationSecurity::EffectivePolicy(incoming),
                                  "https://evil.example/"));
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

// ---- Workflow files the browser didn't write (workflow_record.h) ---------

// A workflow with something in every field the digest covers.
Script RichScript() {
  Script s;
  s.id = "wf_rich";
  s.name = "Prices";
  s.version = 2;
  s.created_at_unix = 1'800'000'000;
  s.ai_model = "m";
  s.enabled = true;
  s.trigger.type = TriggerType::CRON;
  s.trigger.expression = "0 9 * * *";
  s.trigger.timezone = "Europe/Paris";
  s.trigger.until_unix = 4'102'444'800;
  s.trigger.last_fired_unix = 1'800'000'100;
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "https://shop.example/"));
  Step click = MakeStep(StepType::CLICK, "#buy", "v");
  click.selector_fallbacks = {"#b"};
  click.store_as = "x";
  click.transform = "t";
  click.timeout_ms = 9000;
  click.max_iterations = 7;
  click.retries = 2;
  click.description = "Buy";
  base::DictValue extra;
  extra.Set("host", "shop.example");
  extra.Set("n", 0.5);
  click.extra = base::Value(std::move(extra));
  locators::StepLocators found;
  found.origin = "https://shop.example";
  found.element = {"button", "", "button"};
  locators::Locator role;
  role.by = locators::By::kRole;
  role.role = "button";
  role.value = "Buy";
  locators::Locator text;
  text.by = locators::By::kText;
  text.value = "Buy";
  found.list = {role, text};
  click.locators = found;
  s.steps.push_back(std::move(click));
  s.steps.push_back(MakeStep(StepType::NOTIFY, "Done", "body"));
  s.security.domain_whitelist = {"shop.example"};
  s.security.require_approval_for = {"payment"};
  s.security.max_runtime_seconds = 60;
  s.security.max_navigations = 3;
  s.security.max_network_bytes = 1000;
  s.security.trust = TrustLevel::APPROVED;
  s.stats.runs = 3;
  s.stats.successes = 2;
  s.default_variables["city"] = "Paris";
  return s;
}

TEST(AutomationSecurityTest, SecurityDigestIgnoresOnlyBookkeeping) {
  const Script rich = RichScript();
  const std::string digest = AutomationSecurity::SecurityDigest(rich);
  ASSERT_EQ(digest.size(), 64u);
  for (char c : digest) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << digest;
  }
  // The same workflow, the same digest.
  EXPECT_EQ(AutomationSecurity::SecurityDigest(RichScript()), digest);
  // Its id (its file's name), its history and its last fire: bookkeeping.
  Script run = RichScript();
  run.id = "wf_other";
  run.stats.runs = 99;
  run.stats.successes = 98;
  run.stats.last_result = "Done";
  run.stats.run_history.emplace_back();
  run.trigger.last_fired_unix = 1;
  EXPECT_EQ(AutomationSecurity::SecurityDigest(run), digest);
}

TEST(AutomationSecurityTest, SecurityDigestCoversEveryOtherField) {
  const std::string digest = AutomationSecurity::SecurityDigest(RichScript());
  using Change = void (*)(Script&);
  const std::pair<const char*, Change> kChanges[] = {
      {"name", [](Script& s) { s.name += "!"; }},
      {"version", [](Script& s) { s.version = 3; }},
      {"created_at_unix", [](Script& s) { s.created_at_unix += 1; }},
      {"ai_model", [](Script& s) { s.ai_model = "n"; }},
      {"enabled", [](Script& s) { s.enabled = false; }},
      {"a variable's value",
       [](Script& s) { s.default_variables["city"] = "Rome"; }},
      {"a variable added", [](Script& s) { s.default_variables["q"] = "x"; }},
      {"trigger type",
       [](Script& s) { s.trigger.type = TriggerType::INTERVAL; }},
      {"trigger expression",
       [](Script& s) { s.trigger.expression = "0 10 * * *"; }},
      {"trigger timezone", [](Script& s) { s.trigger.timezone.clear(); }},
      {"trigger until", [](Script& s) { s.trigger.until_unix += 1; }},
      {"a step added",
       [](Script& s) {
         s.steps.push_back(MakeStep(StepType::NAVIGATE, "https://x.example/"));
       }},
      {"a step removed", [](Script& s) { s.steps.pop_back(); }},
      {"steps reordered", [](Script& s) { std::swap(s.steps[0], s.steps[2]); }},
      {"step type", [](Script& s) { s.steps[2].type = StepType::SCREENSHOT; }},
      {"step target", [](Script& s) { s.steps[1].target = "#sell"; }},
      {"step value", [](Script& s) { s.steps[1].value = "w"; }},
      {"step selector_fallbacks",
       [](Script& s) { s.steps[1].selector_fallbacks.push_back("#c"); }},
      {"step locators",
       [](Script& s) { s.steps[1].locators->list.pop_back(); }},
      {"step locators removed", [](Script& s) { s.steps[1].locators.reset(); }},
      {"step store_as", [](Script& s) { s.steps[1].store_as = "y"; }},
      {"step transform", [](Script& s) { s.steps[1].transform = "u"; }},
      {"step timeout_ms", [](Script& s) { s.steps[1].timeout_ms = 1000; }},
      {"step max_iterations", [](Script& s) { s.steps[1].max_iterations = 8; }},
      {"step retries", [](Script& s) { s.steps[1].retries = 0; }},
      {"step extra",
       [](Script& s) { s.steps[1].extra.GetDict().Set("n", 0.25); }},
      {"step description", [](Script& s) { s.steps[1].description = "Sell"; }},
      {"whitelist",
       [](Script& s) {
         s.security.domain_whitelist.push_back("evil.example");
       }},
      {"require_approval_for",
       [](Script& s) { s.security.require_approval_for = {"login"}; }},
      {"max_runtime_seconds",
       [](Script& s) { s.security.max_runtime_seconds = 61; }},
      {"max_navigations", [](Script& s) { s.security.max_navigations = 4; }},
      {"max_network_bytes",
       [](Script& s) { s.security.max_network_bytes = 1001; }},
      {"trust", [](Script& s) { s.security.trust = TrustLevel::ADMIN; }},
      {"sites_unconfirmed",
       [](Script& s) { s.security.sites_unconfirmed = true; }},
      {"changed_outside", [](Script& s) { s.security.changed_outside = true; }},
  };
  for (const auto& [what, change] : kChanges) {
    Script changed = RichScript();
    change(changed);
    EXPECT_NE(AutomationSecurity::SecurityDigest(changed), digest) << what;
  }
  // An empty approval list isn't the default one written out (the
  // effective policy treats them alike; the file doesn't say the same).
  Script empty = RichScript();
  empty.security.require_approval_for.clear();
  Script defaults = RichScript();
  defaults.security.require_approval_for = {"form_submit", "payment", "login"};
  EXPECT_NE(AutomationSecurity::SecurityDigest(empty),
            AutomationSecurity::SecurityDigest(defaults));
}

TEST(AutomationSecurityTest, SecurityDigestSurvivesTheFileRoundTrip) {
  Script s = RichScript();
  // Times past what a double holds exactly, doubles and nested values in a
  // step's extra, invalid UTF-8 and U+2028 in strings.
  s.created_at_unix = std::numeric_limits<int64_t>::max();
  s.trigger.until_unix = (int64_t{1} << 53) + 1;
  base::ListValue nested;
  nested.Append(1.5);
  nested.Append(base::DictValue().Set("k", "v"));
  s.steps[1].extra.GetDict().Set("nested", std::move(nested));
  s.name = "bad \xC3 UTF-8 and a \xE2\x80\xA8 line separator";
  s.steps[2].value = "\xE2\x80\xA8";
  // As AutomationStorage writes it, and as its readers parse it.
  base::DictValue json = s.ToJSON();
  AutomationSecurity::ScrubPlaintextSecrets(json);
  std::string bytes;
  ASSERT_TRUE(base::JSONWriter::WriteWithOptions(
      json, base::JSONWriter::OPTIONS_PRETTY_PRINT, &bytes));
  std::optional<Script> read = Script::FromJSONString(bytes);
  ASSERT_TRUE(read);
  const std::string digest = AutomationSecurity::SecurityDigest(*read);
  ASSERT_EQ(digest.size(), 64u);
  EXPECT_EQ(AutomationSecurity::DigestAsStored(s), digest);
  EXPECT_EQ(AutomationSecurity::SecurityDigest(s), digest);
  // And again: written by the browser that read it, the same.
  EXPECT_EQ(AutomationSecurity::DigestAsStored(*read), digest);
}

TEST(AutomationSecurityTest, SecurityDigestIsScrubInvariant) {
  Script s = RichScript();
  Step password =
      MakeStep(StepType::TYPE, "[name=\"passwd\"]", std::string("s3cret!"));
  password.description = "Type \"s3cret!\" in the field";
  s.steps.push_back(std::move(password));
  base::DictValue json = s.ToJSON();
  ASSERT_TRUE(AutomationSecurity::ScrubPlaintextSecrets(json));
  std::optional<Script> scrubbed = Script::FromJSON(json);
  ASSERT_TRUE(scrubbed);
  EXPECT_TRUE(scrubbed->steps.back().value.empty());
  EXPECT_EQ(AutomationSecurity::SecurityDigest(*scrubbed),
            AutomationSecurity::SecurityDigest(s));
}

TEST(AutomationSecurityTest, ClampUnvouchedIsAnUnconfirmedImport) {
  Script s = RichScript();
  s.security.trust = TrustLevel::ADMIN;
  s.security.require_approval_for = {"x"};
  s.security.domain_whitelist = {"evil.example"};
  s.security.max_runtime_seconds = 99999;
  AutomationSecurity::ClampUnvouched(s);
  // Nothing the file said about what it may do.
  EXPECT_EQ(s.security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(s.security.domain_whitelist.empty());
  EXPECT_EQ(s.security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  const SecurityPolicy defaults{};
  EXPECT_EQ(s.security.max_runtime_seconds, defaults.max_runtime_seconds);
  EXPECT_EQ(s.security.max_navigations, defaults.max_navigations);
  EXPECT_EQ(s.security.max_network_bytes, defaults.max_network_bytes);
  EXPECT_TRUE(s.security.sites_unconfirmed);
  EXPECT_TRUE(s.security.changed_outside);
  // Nor its history.
  EXPECT_EQ(s.stats.runs, 0);
  EXPECT_EQ(s.stats.successes, 0);
  EXPECT_TRUE(s.stats.run_history.empty());
  // What the user reviews is kept.
  EXPECT_EQ(s.steps.size(), 3u);
  EXPECT_EQ(s.trigger.type, TriggerType::CRON);
  EXPECT_EQ(s.trigger.expression, "0 9 * * *");
  EXPECT_TRUE(s.enabled);
  EXPECT_EQ(s.default_variables["city"], "Paris");
  EXPECT_EQ(s.name, "Prices");
  // It may do nothing on its own.
  const SecurityPolicy p = AutomationSecurity::EffectivePolicy(s);
  EXPECT_EQ(p.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(p.domain_whitelist.empty());
  AutomationSecurity sec;
  EXPECT_FALSE(sec.AllowSubmit(p));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://shop.example/"));
  EXPECT_FALSE(sec.AllowHeadless(p));
}

TEST(AutomationSecurityTest,
     MayFireUnattendedRefusesDisabledDraftsChangedAndVoice) {
  EXPECT_TRUE(AutomationSecurity::MayFireUnattended(RichScript()));
  Script disabled = RichScript();
  disabled.enabled = false;
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(disabled));
  Script draft = UnconfirmedDraft();
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(draft));
  Script changed = RichScript();
  AutomationSecurity::ClampUnvouched(changed);
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(changed));
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(VoiceRoutine()));
}

TEST(AutomationSecurityTest, MayRunAsChildRefusesDraftsChangedAndVoice) {
  EXPECT_TRUE(AutomationSecurity::MayRunAsChild(RichScript()));
  // A workflow that isn't scheduled may still be run by another.
  Script disabled = RichScript();
  disabled.enabled = false;
  EXPECT_TRUE(AutomationSecurity::MayRunAsChild(disabled));
  EXPECT_FALSE(AutomationSecurity::MayRunAsChild(UnconfirmedDraft()));
  Script changed = RichScript();
  AutomationSecurity::ClampUnvouched(changed);
  EXPECT_FALSE(AutomationSecurity::MayRunAsChild(changed));
  EXPECT_FALSE(AutomationSecurity::MayRunAsChild(VoiceRoutine()));
}

TEST(AutomationSecurityTest, ConfirmDraftSitesClearsChangedOutside) {
  Script changed = UnconfirmedDraft();
  changed.security.changed_outside = true;
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(
      changed, {"shop.example", "pay.example"}));
  EXPECT_FALSE(changed.security.sites_unconfirmed);
  EXPECT_FALSE(changed.security.changed_outside);
  EXPECT_EQ(changed.security.trust, TrustLevel::CASUAL);
}

TEST(AutomationSecurityTest, ClampTrustForSaveCarriesChangedOutside) {
  Script existing = UnconfirmedDraft();
  existing.security.changed_outside = true;
  Script incoming = UnconfirmedDraft();
  incoming.security.sites_unconfirmed = false;  // claimed by UI JSON
  incoming.security.changed_outside = false;
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_TRUE(incoming.security.sites_unconfirmed);
  EXPECT_TRUE(incoming.security.changed_outside);
  // A draft from an agent run stays one, and says so.
  existing.security.changed_outside = false;
  incoming.security.changed_outside = true;
  AutomationSecurity::ClampTrustForSave(incoming, &existing);
  EXPECT_TRUE(incoming.security.sites_unconfirmed);
  EXPECT_FALSE(incoming.security.changed_outside);
}

TEST(AutomationSecurityTest, ReviewMessageNamesTheReason) {
  Script draft = UnconfirmedDraft();
  EXPECT_STREQ(AutomationSecurity::ReviewMessage(draft),
               kDraftNeedsReviewMessage);
  draft.security.changed_outside = true;
  EXPECT_STREQ(AutomationSecurity::ReviewMessage(draft),
               kChangedOutsideMessage);
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
