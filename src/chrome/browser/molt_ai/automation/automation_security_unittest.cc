// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_security.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

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

// A whitelist entry that is itself a registry (a public suffix), from the
// file's whitelist or a step's host, allows no site under it; an ordinary
// one allows its subdomains within its own site, and an IP address or a
// single label only itself.
TEST(AutomationSecurityTest, ARegistryInTheWhitelistAllowsNoSiteUnderIt) {
  AutomationSecurity sec;
  SecurityPolicy p;
  p.domain_whitelist = {"com", "co.uk", "github.io"};
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.com/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://shop.co.uk/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://someone.github.io/"));
  EXPECT_FALSE(sec.AllowClick(p, "https://evil.com/", "#go"));
  p.domain_whitelist = {"kayak.com", "user.github.io", "192.168.1.1",
                        "localhost"};
  EXPECT_TRUE(sec.AllowNavigation(p, "https://kayak.com/"));
  EXPECT_TRUE(sec.AllowNavigation(p, "https://www.kayak.com/"));
  EXPECT_TRUE(sec.AllowNavigation(p, "https://a.user.github.io/"));
  EXPECT_TRUE(sec.AllowNavigation(p, "http://192.168.1.1/"));
  EXPECT_TRUE(sec.AllowNavigation(p, "http://localhost:8080/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evilkayak.com/"));
  // Nor across a registry below the entry: a bucket under s3.amazonaws.com
  // is someone else's site.
  p.domain_whitelist = {"amazonaws.com"};
  EXPECT_FALSE(sec.AllowNavigation(p, "https://bucket.s3.amazonaws.com/"));
  EXPECT_TRUE(sec.AllowNavigation(p, "https://console.amazonaws.com/"));

  // A step's recorded host, as a file can give it.
  Script script;
  Step click = MakeStep(StepType::CLICK, "#go");
  base::DictValue extra;
  extra.Set("host", "com");
  click.extra = base::Value(std::move(extra));
  script.steps.push_back(std::move(click));
  EXPECT_FALSE(sec.AllowNavigation(AutomationSecurity::EffectivePolicy(script),
                                   "https://attacker.com/?d=secret"));
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
  // A stored list adds to the defaults, never drops one.
  EXPECT_EQ(p.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
}

// A file can't opt a workflow out of an approval (an import from an older
// build, a hand-edited file): below APPROVED, submits and sensitive fields
// stay blocked whatever its list says; categories it adds are kept.
TEST(AutomationSecurityTest, EffectivePolicyNeverDropsADefaultApproval) {
  for (const std::vector<std::string>& listed :
       {std::vector<std::string>{"x"}, std::vector<std::string>{""},
        std::vector<std::string>{"payment"},
        std::vector<std::string>{"later_category", "login"}}) {
    Script script;
    script.security.require_approval_for = listed;
    SecurityPolicy p = AutomationSecurity::EffectivePolicy(script);
    for (const char* category : {"form_submit", "payment", "login"}) {
      EXPECT_NE(std::find(p.require_approval_for.begin(),
                          p.require_approval_for.end(), category),
                p.require_approval_for.end())
          << category;
    }
    AutomationSecurity sec;
    EXPECT_FALSE(sec.AllowSubmit(p));
    EXPECT_FALSE(sec.AllowSensitiveField(
        p, molt_ai::agent_safety::FieldKind::kPassword));
    EXPECT_FALSE(sec.AllowSensitiveField(
        p, molt_ai::agent_safety::FieldKind::kPaymentCard));
  }
  Script added;
  added.security.require_approval_for = {"later_category"};
  EXPECT_EQ(AutomationSecurity::EffectivePolicy(added).require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login",
                                      "later_category"}));
}

// Below TRUSTED a run is at most the default 300 s, whatever its file says;
// a shorter limit is kept, and TRUSTED keeps its own.
TEST(AutomationSecurityTest, EffectivePolicyBoundsTheRuntimeBelowTrusted) {
  for (int stored : {0, -1, 301, std::numeric_limits<int>::max()}) {
    Script script;
    script.security.max_runtime_seconds = stored;
    EXPECT_EQ(AutomationSecurity::EffectivePolicy(script).max_runtime_seconds,
              300)
        << stored;
    script.security.trust = TrustLevel::APPROVED;
    EXPECT_EQ(AutomationSecurity::EffectivePolicy(script).max_runtime_seconds,
              300)
        << stored;
  }
  Script shorter;
  shorter.security.max_runtime_seconds = 60;
  EXPECT_EQ(AutomationSecurity::EffectivePolicy(shorter).max_runtime_seconds,
            60);
  Script trusted;
  trusted.security.trust = TrustLevel::TRUSTED;
  trusted.security.max_runtime_seconds = 3600;
  EXPECT_EQ(AutomationSecurity::EffectivePolicy(trusted).max_runtime_seconds,
            3600);
}

// A run's time limit is a deadline: its policy's seconds, below TRUSTED at
// most 300 whatever the file says; TRUSTED keeps its own, and a limit that
// isn't positive allows no time.
TEST(AutomationSecurityTest, RunTimeLimitIsTheEffectivePolicysSeconds) {
  for (int stored : {0, -1, 301, 99999}) {
    Script casual;
    casual.security.max_runtime_seconds = stored;
    EXPECT_EQ(AutomationSecurity::RunTimeLimit(
                  AutomationSecurity::EffectivePolicy(casual)),
              base::Seconds(300))
        << stored;
  }
  Script shorter;
  shorter.security.max_runtime_seconds = 30;
  EXPECT_EQ(AutomationSecurity::RunTimeLimit(
                AutomationSecurity::EffectivePolicy(shorter)),
            base::Seconds(30));
  Script trusted;
  trusted.security.trust = TrustLevel::TRUSTED;
  trusted.security.max_runtime_seconds = 3600;
  EXPECT_EQ(AutomationSecurity::RunTimeLimit(
                AutomationSecurity::EffectivePolicy(trusted)),
            base::Seconds(3600));
  trusted.security.max_runtime_seconds = std::numeric_limits<int>::max();
  EXPECT_EQ(AutomationSecurity::RunTimeLimit(
                AutomationSecurity::EffectivePolicy(trusted)),
            base::Seconds(std::numeric_limits<int>::max()));
  for (int stored : {0, -1, std::numeric_limits<int>::min()}) {
    trusted.security.max_runtime_seconds = stored;
    EXPECT_EQ(AutomationSecurity::RunTimeLimit(
                  AutomationSecurity::EffectivePolicy(trusted)),
              base::TimeDelta())
        << stored;
  }
}

// Below TRUSTED a run opens at most the default 20 pages and loads at most
// the default 200 MB, whatever its file says; a lower limit is kept, and
// TRUSTED keeps its own (no run is held to them: RunLimitMessage()).
TEST(AutomationSecurityTest, EffectivePolicyBoundsTheRunLimitsBelowTrusted) {
  const SecurityPolicy defaults;
  for (TrustLevel trust : {TrustLevel::CASUAL, TrustLevel::APPROVED}) {
    for (int stored : {0, -1, 21, std::numeric_limits<int>::max()}) {
      Script script;
      script.security.trust = trust;
      script.security.max_navigations = stored;
      EXPECT_EQ(AutomationSecurity::EffectivePolicy(script).max_navigations,
                defaults.max_navigations)
          << stored;
    }
    for (int64_t stored :
         {int64_t{0}, int64_t{-1}, defaults.max_network_bytes + 1,
          std::numeric_limits<int64_t>::max()}) {
      Script script;
      script.security.trust = trust;
      script.security.max_network_bytes = stored;
      EXPECT_EQ(AutomationSecurity::EffectivePolicy(script).max_network_bytes,
                defaults.max_network_bytes)
          << stored;
    }
    Script lower;
    lower.security.trust = trust;
    lower.security.max_navigations = 3;
    lower.security.max_network_bytes = 1000;
    const SecurityPolicy p = AutomationSecurity::EffectivePolicy(lower);
    EXPECT_EQ(p.max_navigations, 3);
    EXPECT_EQ(p.max_network_bytes, 1000);
  }
  Script trusted;
  trusted.security.trust = TrustLevel::TRUSTED;
  trusted.security.max_navigations = 500;
  trusted.security.max_network_bytes = std::numeric_limits<int64_t>::max();
  SecurityPolicy p = AutomationSecurity::EffectivePolicy(trusted);
  EXPECT_EQ(p.max_navigations, 500);
  EXPECT_EQ(p.max_network_bytes, std::numeric_limits<int64_t>::max());
  // An unconfirmed draft is CASUAL whatever trust it was given.
  trusted.security.sites_unconfirmed = true;
  p = AutomationSecurity::EffectivePolicy(trusted);
  EXPECT_EQ(p.max_navigations, defaults.max_navigations);
  EXPECT_EQ(p.max_network_bytes, defaults.max_network_bytes);
}

// A Casual run stops one page or one byte past a limit; at its limits it
// goes on. A workflow the user recorded or approved (APPROVED and above)
// is held to neither.
TEST(AutomationSecurityTest, RunLimitMessageStopsARunPastItsLimits) {
  const SecurityPolicy defaults;
  const uint64_t max_bytes = static_cast<uint64_t>(defaults.max_network_bytes);
  for (TrustLevel trust : {TrustLevel::CASUAL}) {
    Script script;
    script.security.trust = trust;
    const SecurityPolicy p = AutomationSecurity::EffectivePolicy(script);
    EXPECT_EQ(AutomationSecurity::RunLimitMessage(p, {20, max_bytes}), "");
    EXPECT_EQ(AutomationSecurity::RunLimitMessage(p, {21, 0}),
              "Stopped: this run opened more than 20 pages, the most this "
              "workflow may open in one run. Approve the workflow (Trust) if "
              "you made it or checked it: an approved workflow has no such "
              "limit.");
    EXPECT_EQ(AutomationSecurity::RunLimitMessage(p, {0, max_bytes + 1}),
              "Stopped: this run's pages loaded more than 200 MB, the most "
              "this workflow may load in one run. Approve the workflow "
              "(Trust) if you made it or checked it: an approved workflow "
              "has no such limit.");
    // Past both: the pages are named.
    EXPECT_NE(AutomationSecurity::RunLimitMessage(p, {21, max_bytes + 1})
                  .find("more than 20 pages,"),
              std::string::npos);
  }
  for (TrustLevel trust :
       {TrustLevel::APPROVED, TrustLevel::TRUSTED, TrustLevel::ADMIN}) {
    SecurityPolicy p;
    p.trust = trust;
    EXPECT_EQ(AutomationSecurity::RunLimitMessage(
                  p, {std::numeric_limits<int>::max(),
                      std::numeric_limits<uint64_t>::max()}),
              "");
  }
  // A lower limit, in its own words.
  Script lower;
  lower.security.max_navigations = 1;
  lower.security.max_network_bytes = 1000;
  const SecurityPolicy p = AutomationSecurity::EffectivePolicy(lower);
  EXPECT_EQ(AutomationSecurity::RunLimitMessage(p, {1, 1000}), "");
  EXPECT_NE(
      AutomationSecurity::RunLimitMessage(p, {2, 0}).find("more than 1 page,"),
      std::string::npos);
  EXPECT_NE(AutomationSecurity::RunLimitMessage(p, {0, 1001})
                .find("more than 1000 bytes,"),
            std::string::npos);
  // A policy that isn't a run's (no EffectivePolicy()) is held to the
  // defaults, never to none.
  SecurityPolicy raw;
  raw.max_navigations = 0;
  raw.max_network_bytes = -1;
  EXPECT_EQ(AutomationSecurity::RunLimitMessage(raw, {20, max_bytes}), "");
  EXPECT_FALSE(AutomationSecurity::RunLimitMessage(raw, {21, 0}).empty());
  EXPECT_FALSE(
      AutomationSecurity::RunLimitMessage(raw, {0, max_bytes + 1}).empty());
}

// A workflow a save brings in (an import, a copy, a page's save of one not
// stored) believes nothing its JSON says about what it may do: the default
// approvals and each limit within its default (a lower one kept), its sites
// kept; and a schedule it brings is kept, turned off.
TEST(AutomationSecurityTest, ANewSaveGetsThePolicyFloorAndItsScheduleOff) {
  Script incoming;
  incoming.security.require_approval_for = {"x"};
  incoming.security.max_runtime_seconds = std::numeric_limits<int>::max();
  incoming.security.max_navigations = -1;
  incoming.security.max_network_bytes = 0;
  incoming.security.domain_whitelist = {"shop.example"};
  incoming.trigger.type = TriggerType::CRON;
  incoming.trigger.expression = "*/5 * * * *";
  AutomationSecurity::ClampTrustForSave(incoming, nullptr);
  const SecurityPolicy defaults;
  EXPECT_EQ(incoming.security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  EXPECT_EQ(incoming.security.max_runtime_seconds,
            defaults.max_runtime_seconds);
  EXPECT_EQ(incoming.security.max_navigations, defaults.max_navigations);
  EXPECT_EQ(incoming.security.max_network_bytes, defaults.max_network_bytes);
  EXPECT_EQ(incoming.security.domain_whitelist,
            std::vector<std::string>{"shop.example"});
  EXPECT_EQ(incoming.trigger.type, TriggerType::CRON);
  EXPECT_EQ(incoming.trigger.expression, "*/5 * * * *");
  EXPECT_FALSE(incoming.enabled);
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(incoming));

  // Lower limits are kept; a manual workflow stays on (nothing to fire).
  Script lower;
  lower.security.max_runtime_seconds = 60;
  lower.security.max_navigations = 3;
  lower.security.max_network_bytes = 1000;
  AutomationSecurity::ClampTrustForSave(lower, nullptr);
  EXPECT_EQ(lower.security.max_runtime_seconds, 60);
  EXPECT_EQ(lower.security.max_navigations, 3);
  EXPECT_EQ(lower.security.max_network_bytes, 1000);
  EXPECT_TRUE(lower.enabled);

  for (TriggerType type : {TriggerType::INTERVAL, TriggerType::AT}) {
    Script scheduled;
    scheduled.trigger.type = type;
    AutomationSecurity::ClampTrustForSave(scheduled, nullptr);
    EXPECT_FALSE(scheduled.enabled);
  }
  Script on_event;
  on_event.trigger.type = TriggerType::ON_EVENT;
  AutomationSecurity::ClampTrustForSave(on_event, nullptr);
  EXPECT_TRUE(on_event.enabled);

  // A stored workflow's save keeps its switch, and the policy its caller
  // put there.
  Script stored;
  stored.trigger.type = TriggerType::CRON;
  stored.security.max_runtime_seconds = 600;
  stored.security.trust = TrustLevel::TRUSTED;
  Script edit;
  edit.trigger.type = TriggerType::CRON;
  edit.security = stored.security;
  AutomationSecurity::ClampTrustForSave(edit, &stored);
  EXPECT_TRUE(edit.enabled);
  EXPECT_EQ(edit.security.max_runtime_seconds, 600);
  EXPECT_EQ(edit.security.trust, TrustLevel::TRUSTED);
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
  // A draft with no site at all can't be confirmed (a file changed outside
  // can: AChangeWithNoSiteIsConfirmedWithNone).
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
  // step's extra, U+2028 in strings. (A script holds valid UTF-8 only: a
  // file that doesn't is never read, below.)
  s.created_at_unix = std::numeric_limits<int64_t>::max();
  s.trigger.until_unix = (int64_t{1} << 53) + 1;
  base::ListValue nested;
  nested.Append(1.5);
  nested.Append(base::DictValue().Set("k", "v"));
  s.steps[1].extra.GetDict().Set("nested", std::move(nested));
  s.name = "a \xE2\x80\xA8 line separator";
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

// A file with invalid UTF-8 in it (written by something else) doesn't
// parse, as the storage's readers parse it: never a script to digest.
TEST(AutomationSecurityTest, AFileWithInvalidUtf8IsNoScript) {
  const std::string placeholder = "Q7X-NAME-Q7X";
  Script s = RichScript();
  s.name = placeholder;
  std::string bytes = s.ToJSONString();
  const size_t at = bytes.find(placeholder);
  ASSERT_NE(at, std::string::npos);
  ASSERT_TRUE(Script::FromJSONString(bytes));
  bytes.replace(at, placeholder.size(), "bad \xC3 UTF-8");
  EXPECT_FALSE(Script::FromJSONString(bytes));
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
  // Not what it doesn't show of the schedule (it runs in the browser's time
  // zone, with no end), nor what no run reads.
  EXPECT_TRUE(s.trigger.timezone.empty());
  EXPECT_EQ(s.trigger.until_unix, 0);
  EXPECT_EQ(s.version, Script().version);
  EXPECT_EQ(s.ai_model, Script().ai_model);
  EXPECT_EQ(s.id, "wf_rich");
  EXPECT_EQ(s.created_at_unix, 1'800'000'000);
  // A variable's name, not its default value (§3.6.6).
  ASSERT_EQ(s.default_variables.count("city"), 1u);
  EXPECT_EQ(s.default_variables["city"], "");
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

// The review shows each step's type, target and value, its site and the
// schedule; not its fallback selectors, locators, text anchor, picture or
// description, which the runner acts on when the target shown misses, nor
// which workflow a RUN_SCRIPT step runs (and so that workflow's sites),
// nor the default value of a variable a field only partly is. The clamp
// drops those, so the review token is of what is shown.
TEST(AutomationSecurityTest, ClampUnvouchedDropsWhatTheReviewDoesntShow) {
  Script s = RichScript();
  s.steps[0].description = "Go to the shop";
  base::DictValue& extra = s.steps[1].extra.GetDict();
  extra.Set(kStepTextAnchorKey, "Delete account");
  extra.Set(kStepRefShotKey, "data:image/png;base64,AAAA");
  // Another workflow run inside this one, one by a variable's default.
  s.steps.insert(s.steps.begin() + 1, MakeStep(StepType::RUN_SCRIPT, "pay"));
  s.steps.push_back(MakeStep(StepType::RUN_SCRIPT, "{{next}}"));
  s.default_variables["next"] = "pay";
  s.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://shop.example/{{path}}"));
  s.default_variables["path"] = "settings/forwarding?add=x@evil.example";
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 4u);
  for (const Step& step : s.steps) {
    EXPECT_NE(step.type, StepType::RUN_SCRIPT);
  }
  // The variables are still named, with no value.
  EXPECT_EQ(s.default_variables,
            (std::map<std::string, std::string>{
                {"city", ""}, {"next", ""}, {"path", ""}}));
  EXPECT_EQ(s.steps[3].target, "https://shop.example/{{path}}");
  for (const Step& step : s.steps) {
    EXPECT_TRUE(step.description.empty());
    EXPECT_TRUE(step.selector_fallbacks.empty());
    EXPECT_FALSE(step.locators);
  }
  const Step& click = s.steps[1];
  ASSERT_TRUE(click.extra.is_dict());
  EXPECT_FALSE(click.extra.GetDict().Find(kStepTextAnchorKey));
  EXPECT_FALSE(click.extra.GetDict().Find(kStepRefShotKey));
  // What is shown, and the site it names, is kept; nothing else (a click's
  // value, store_as, transform, timeout, loop count and retries aren't
  // shown, nor any other extra key).
  EXPECT_EQ(click.type, StepType::CLICK);
  EXPECT_EQ(click.target, "#buy");
  EXPECT_TRUE(click.value.empty());
  EXPECT_TRUE(click.store_as.empty());
  EXPECT_TRUE(click.transform.empty());
  EXPECT_EQ(click.timeout_ms, Step().timeout_ms);
  EXPECT_EQ(click.max_iterations, Step().max_iterations);
  EXPECT_EQ(click.retries, Step().retries);
  EXPECT_EQ(click.extra.GetDict().size(), 1u);
  ASSERT_TRUE(click.extra.GetDict().FindString(kStepHostKey));
  EXPECT_EQ(*click.extra.GetDict().FindString(kStepHostKey), "shop.example");
  EXPECT_EQ(s.steps[0].target, "https://shop.example/");
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(s.steps),
            AutomationSecurity::HostsReferencedBy(RichScript().steps));
  // Clamping twice is clamping once: the review token doesn't move.
  const std::string token = AutomationSecurity::SecurityDigest(s);
  AutomationSecurity::ClampUnvouched(s);
  EXPECT_EQ(AutomationSecurity::SecurityDigest(s), token);
}

// The clamp is an allowlist: only the step types the Agent studio's review
// shows in full are kept, in order; control flow, page snapshots, code,
// the on-device model's steps, another workflow and unknown steps are
// dropped (§3.6.6).
TEST(AutomationSecurityTest, ClampUnvouchedKeepsOnlyStepTypesTheReviewShows) {
  constexpr StepType kKept[] = {
      StepType::NAVIGATE, StepType::CLICK,    StepType::TYPE,
      StepType::SCROLL,   StepType::KEYPRESS, StepType::WAIT,
      StepType::WAIT_FOR, StepType::EXTRACT,  StepType::ASSERT,
      StepType::NOTIFY,   StepType::CALL_TOOL};
  constexpr StepType kDropped[] = {
      StepType::IF,         StepType::ELSE,       StepType::END_IF,
      StepType::LOOP,       StepType::END_LOOP,   StepType::SCREENSHOT,
      StepType::RUN_JS,     StepType::RUN_SCRIPT, StepType::AI_DECIDE,
      StepType::AI_EXTRACT, StepType::UNKNOWN};
  Script s = RichScript();
  s.steps.clear();
  std::vector<StepType> expected;
  for (size_t i = 0; i < std::size(kKept); ++i) {
    s.steps.push_back(MakeStep(kDropped[i], "x", "document.body.remove()"));
    s.steps.push_back(MakeStep(kKept[i], "#a", "b"));
    expected.push_back(kKept[i]);
  }
  AutomationSecurity::ClampUnvouched(s);
  std::vector<StepType> kept;
  for (const Step& step : s.steps) {
    kept.push_back(step.type);
  }
  EXPECT_EQ(kept, expected);
  const std::string token = AutomationSecurity::SecurityDigest(s);
  AutomationSecurity::ClampUnvouched(s);
  EXPECT_EQ(AutomationSecurity::SecurityDigest(s), token);
}

// Of each kept step, only the fields the review shows, as the runner uses
// them; every other field its default.
TEST(AutomationSecurityTest, ClampUnvouchedKeepsOnlyTheFieldsTheReviewShows) {
  Script s = RichScript();
  s.steps.clear();
  for (StepType type :
       {StepType::NAVIGATE, StepType::CLICK, StepType::TYPE, StepType::SCROLL,
        StepType::KEYPRESS, StepType::WAIT, StepType::WAIT_FOR,
        StepType::EXTRACT, StepType::ASSERT, StepType::NOTIFY}) {
    Step step = MakeStep(type, "t", "v");
    step.store_as = "s";
    step.transform = "parseInt";
    step.timeout_ms = 86'400'000;
    step.max_iterations = 9;
    step.retries = 5;
    s.steps.push_back(std::move(step));
  }
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 10u);
  for (const Step& step : s.steps) {
    const StepType t = step.type;
    const bool target = t == StepType::NAVIGATE || t == StepType::CLICK ||
                        t == StepType::TYPE || t == StepType::WAIT_FOR ||
                        t == StepType::EXTRACT || t == StepType::ASSERT;
    const bool value = t == StepType::TYPE || t == StepType::SCROLL ||
                       t == StepType::KEYPRESS || t == StepType::WAIT ||
                       t == StepType::ASSERT || t == StepType::NOTIFY;
    const std::string what = StepTypeToString(t);
    EXPECT_EQ(step.target, target ? "t" : "") << what;
    EXPECT_EQ(step.value, value ? "v" : "") << what;
    EXPECT_EQ(step.store_as, t == StepType::EXTRACT ? "s" : "") << what;
    EXPECT_TRUE(step.transform.empty()) << what;
    EXPECT_EQ(step.timeout_ms, Step().timeout_ms) << what;
    EXPECT_EQ(step.max_iterations, Step().max_iterations) << what;
    EXPECT_EQ(step.retries, Step().retries) << what;
    EXPECT_TRUE(step.extra.is_none()) << what;
  }
}

// Of a step's extra, only "host", a saved secret as the runner reads it,
// and a site tool's tool as the review shows it: review notes, the agent's
// ask-before flag, a recorded host for the secret, and any key a later
// build might add are gone; a host that isn't printable ASCII too.
TEST(AutomationSecurityTest,
     ClampUnvouchedKeepsOnlyTheExtraKeysTheReviewShows) {
  Script s = RichScript();
  s.steps.clear();
  Step typed = MakeStep(StepType::TYPE, "#notes", "ship to 1 Evil St");
  base::DictValue extra;
  extra.Set(kStepHostKey, "shop.example");
  extra.Set("review", "secret_field");
  extra.Set("ask_before_sensitive", true);
  extra.Set(kStepTextAnchorKey, "Notes");
  extra.Set("future_key", "x");
  typed.extra = base::Value(extra.Clone());
  s.steps.push_back(std::move(typed));
  // A secret step: "vault" and its kind as the runner reads it (one it
  // doesn't know is a password), never a value.
  Step secret = MakeStep(StepType::TYPE, "#pw", "hunter2");
  base::DictValue secret_extra = extra.Clone();
  secret_extra.Set(kSecretKey, "anything");
  secret_extra.Set(kSecretKindKey, "weird");
  secret_extra.Set(kSecretHostKey, "other.example");
  secret.extra = base::Value(std::move(secret_extra));
  s.steps.push_back(std::move(secret));
  Step card = MakeStep(StepType::TYPE, "#cc");
  base::DictValue card_extra;
  card_extra.Set(kSecretKey, kSecretVault);
  card_extra.Set(kSecretKindKey, "payment_card");
  card.extra = base::Value(std::move(card_extra));
  s.steps.push_back(std::move(card));
  // A site tool: its origin, frame, values, the names not saved and that
  // it is a form tool; not what it says it does.
  Step tool = MakeStep(StepType::CALL_TOOL, "transfer");
  base::DictValue tool_dict;
  tool_dict.Set("origin", "https://shop.example");
  base::DictValue frame;
  frame.Set("origin", "https://w.example");
  frame.Set("path", "/f");
  frame.Set("number", 3);
  tool_dict.Set("frame", std::move(frame));
  base::DictValue args;
  args.Set("to", "acct-1");
  tool_dict.Set("args", std::move(args));
  base::ListValue secret_args;
  secret_args.Append("otp");
  tool_dict.Set("secret_args", std::move(secret_args));
  tool_dict.Set("read_only", true);
  tool_dict.Set("form", true);
  tool_dict.Set("future_key", 1);
  base::DictValue tool_extra;
  tool_extra.Set(kStepHostKey, "shop.example");
  tool_extra.Set("review", "secret_argument");
  tool_extra.Set("tool", std::move(tool_dict));
  tool.extra = base::Value(std::move(tool_extra));
  s.steps.push_back(std::move(tool));
  // A host that would read otherwise than it is.
  Step bidi = MakeStep(StepType::CLICK, "#ok");
  base::DictValue bidi_extra;
  bidi_extra.Set(kStepHostKey,
                 "evil\xE2\x80\xAE"
                 "elpmaxe.com");
  bidi.extra = base::Value(std::move(bidi_extra));
  s.steps.push_back(std::move(bidi));

  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 5u);
  base::DictValue host_only;
  host_only.Set(kStepHostKey, "shop.example");
  EXPECT_EQ(s.steps[0].extra.GetDict(), host_only);
  EXPECT_EQ(s.steps[0].value, "ship to 1 Evil St");
  base::DictValue vault = host_only.Clone();
  vault.Set(kSecretKey, kSecretVault);
  vault.Set(kSecretKindKey, "password");
  EXPECT_EQ(s.steps[1].extra.GetDict(), vault);
  EXPECT_TRUE(s.steps[1].value.empty());
  EXPECT_EQ(AutomationSecurity::SecretStepKind(s.steps[2]),
            FieldKind::kPaymentCard);
  const base::DictValue& kept_tool = s.steps[3].extra.GetDict();
  EXPECT_EQ(kept_tool.size(), 2u);
  EXPECT_EQ(*kept_tool.FindString(kStepHostKey), "shop.example");
  const base::DictValue* shown_tool = kept_tool.FindDict("tool");
  ASSERT_TRUE(shown_tool);
  EXPECT_EQ(shown_tool->size(), 5u);
  EXPECT_EQ(*shown_tool->FindString("origin"), "https://shop.example");
  EXPECT_EQ(shown_tool->FindDict("frame")->size(), 2u);
  EXPECT_EQ(*shown_tool->FindDict("frame")->FindString("path"), "/f");
  EXPECT_EQ(*shown_tool->FindDict("args")->FindString("to"), "acct-1");
  EXPECT_EQ(shown_tool->FindList("secret_args")->size(), 1u);
  EXPECT_EQ(shown_tool->FindBool("form"), true);
  EXPECT_FALSE(shown_tool->Find("read_only"));
  EXPECT_TRUE(s.steps[4].extra.is_none());
  const std::string token = AutomationSecurity::SecurityDigest(s);
  AutomationSecurity::ClampUnvouched(s);
  EXPECT_EQ(AutomationSecurity::SecurityDigest(s), token);
}

// A step of a routine taught by voice keeps its type, target, value and
// extra "voice" and "host" (its claim keeps of "voice" only what the review
// describes, voice::DescribeRoutineStepsAsRun()); never a secret key, which
// voice mode doesn't read, nor anything else.
TEST(AutomationSecurityTest, ClampUnvouchedKeepsOnlyAVoiceStepsVoiceAndHost) {
  Script s = VoiceRoutine();
  ASSERT_FALSE(s.steps.empty());
  Step& step = s.steps[0];
  step.type = StepType::TYPE;
  step.description = "Fill your saved password";
  step.selector_fallbacks = {"#pw"};
  step.timeout_ms = 1;
  base::DictValue& extra = step.extra.GetDict();
  extra.Set(kStepHostKey, "news.example");
  extra.Set(kSecretKey, kSecretVault);
  extra.Set(kSecretKindKey, "password");
  extra.Set("review", "secret_field");
  const base::DictValue voice = extra.FindDict(kVoiceStepKey)->Clone();
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_FALSE(s.steps.empty());
  const Step& kept = s.steps[0];
  EXPECT_EQ(kept.type, StepType::TYPE);
  EXPECT_TRUE(kept.description.empty());
  EXPECT_TRUE(kept.selector_fallbacks.empty());
  EXPECT_EQ(kept.timeout_ms, Step().timeout_ms);
  ASSERT_TRUE(kept.extra.is_dict());
  EXPECT_EQ(kept.extra.GetDict().size(), 2u);
  EXPECT_EQ(*kept.extra.GetDict().FindDict(kVoiceStepKey), voice);
  EXPECT_EQ(*kept.extra.GetDict().FindString(kStepHostKey), "news.example");
  EXPECT_EQ(AutomationSecurity::SecretStepKind(kept), FieldKind::kOrdinary);
}

// A one-line field the review shows in an <input> must read as it runs: a
// step whose target, value or store_as holds a line break (an <input>
// drops it: "div\na" would read "diva"), a tab, another control character,
// DEL or a character that reorders, hides or breaks text is dropped. So is
// the schedule of an expression that holds one, or of a workflow the file
// has turned off.
TEST(AutomationSecurityTest, ClampUnvouchedDropsAStepThatReadsOtherwise) {
  Script s = RichScript();
  s.steps.clear();
  s.steps.push_back(MakeStep(StepType::CLICK, "div\na"));
  s.steps.push_back(MakeStep(StepType::TYPE, "#q", "a\r\nb"));
  s.steps.push_back(MakeStep(StepType::NAVIGATE,
                             "https://shop.example/\xE2\x80\xAE"
                             "evil"));
  Step extract = MakeStep(StepType::EXTRACT, "#price");
  extract.store_as = "price\t";
  s.steps.push_back(std::move(extract));
  s.steps.push_back(MakeStep(StepType::NOTIFY, "", "Done\xE2\x80\x8B"));
  s.steps.push_back(MakeStep(StepType::KEYPRESS, "", "Enter\x7F"));
  s.steps.push_back(MakeStep(StepType::CLICK, "#ok"));
  s.steps.push_back(MakeStep(StepType::TYPE, "#q", "caf\xC3\xA9 \xE6\x97\xA5"));
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 2u);
  EXPECT_EQ(s.steps[0].target, "#ok");
  EXPECT_EQ(s.steps[1].value, "caf\xC3\xA9 \xE6\x97\xA5");
  EXPECT_EQ(s.trigger.type, TriggerType::CRON);

  // A site tool given a value the review would lay out as lines or space
  // (a line break, a carriage return, a tab: in a list too), or one named
  // so, is dropped: a value could push those after it out of view. A
  // character that reorders, hides or breaks text in a value is shown as
  // it runs (U+FFFD, each value isolated): kept.
  const auto tool_step = [](base::DictValue args) {
    Step tool = MakeStep(StepType::CALL_TOOL, "send");
    base::DictValue tool_dict;
    tool_dict.Set("origin", "https://shop.example");
    tool_dict.Set("args", std::move(args));
    base::DictValue tool_extra;
    tool_extra.Set(kStepHostKey, "shop.example");
    tool_extra.Set("tool", std::move(tool_dict));
    tool.extra = base::Value(std::move(tool_extra));
    return tool;
  };
  Script tools = RichScript();
  tools.steps.clear();
  base::DictValue lines;
  lines.Set("memo", "Rent" + std::string(470, '\n'));
  lines.Set("to", "acct-1");
  tools.steps.push_back(tool_step(std::move(lines)));
  base::DictValue returned;
  returned.Set("memo", "Rent\r");
  tools.steps.push_back(tool_step(std::move(returned)));
  base::DictValue tab;
  base::ListValue items;
  items.Append("a");
  items.Append("b\tc");
  tab.Set("items", std::move(items));
  tools.steps.push_back(tool_step(std::move(tab)));
  base::DictValue named;
  named.Set("to\n", "acct-1");
  tools.steps.push_back(tool_step(std::move(named)));
  base::DictValue shown;
  shown.Set("account",
            "\xE2\x80\xAE"
            "87654321");
  shown.Set("amount", 100);
  base::ListValue tags;
  tags.Append("x y");
  tags.Append(2);
  shown.Set("tags", std::move(tags));
  tools.steps.push_back(tool_step(std::move(shown)));
  AutomationSecurity::ClampUnvouched(tools);
  ASSERT_EQ(tools.steps.size(), 1u);
  ASSERT_TRUE(tools.steps[0].extra.is_dict());
  const base::DictValue* kept_tool =
      tools.steps[0].extra.GetDict().FindDict("tool");
  ASSERT_TRUE(kept_tool);
  const base::DictValue* kept_args = kept_tool->FindDict("args");
  ASSERT_TRUE(kept_args);
  EXPECT_EQ(kept_args->size(), 3u);
  EXPECT_EQ(*kept_args->FindString("account"),
            "\xE2\x80\xAE"
            "87654321");

  Script newline = RichScript();
  newline.trigger.expression = "0 9 * * *\n";
  AutomationSecurity::ClampUnvouched(newline);
  EXPECT_EQ(newline.trigger.type, TriggerType::MANUAL);
  EXPECT_TRUE(newline.trigger.expression.empty());
  Script off = RichScript();
  off.enabled = false;
  AutomationSecurity::ClampUnvouched(off);
  EXPECT_EQ(off.trigger.type, TriggerType::MANUAL);
  EXPECT_TRUE(off.enabled);
  EXPECT_FALSE(AutomationSecurity::MayFireUnattended(off));
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

// A file changed outside whose steps open only addresses that start with a
// {{variable}} (or no page) has no site to confirm: it is confirmed with
// none. Its whitelist is then empty, so below TRUSTED it opens and acts on
// no page by itself, and another workflow that runs it adds no site to its
// run.
TEST(AutomationSecurityTest, AChangeWithNoSiteIsConfirmedWithNone) {
  Script s;
  s.id = "wf_1";
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{base}}/search?q={{q}}"));
  s.steps.push_back(MakeStep(StepType::CLICK, "#go"));
  s.steps.push_back(MakeStep(StepType::WAIT, "", "1"));
  // What the file claims.
  s.security.trust = TrustLevel::TRUSTED;
  s.security.domain_whitelist = {"evil.example"};
  s.default_variables["url"] = "https://evil.example/";
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 4u);
  ASSERT_TRUE(AutomationSecurity::HostsReferencedBy(s.steps).empty());
  EXPECT_TRUE(AutomationSecurity::MayConfirmWithoutSites(s));
  // Never with a site its steps don't target.
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {"evil.example"}));
  EXPECT_TRUE(s.security.sites_unconfirmed);
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(s, {}));
  EXPECT_FALSE(s.security.sites_unconfirmed);
  EXPECT_FALSE(s.security.changed_outside);
  EXPECT_TRUE(s.security.domain_whitelist.empty());
  EXPECT_EQ(s.security.trust, TrustLevel::CASUAL);
  // Only once.
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(s));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {}));
  AutomationSecurity sec;
  for (TrustLevel trust : {TrustLevel::CASUAL, TrustLevel::APPROVED}) {
    s.security.trust = trust;
    const SecurityPolicy p = AutomationSecurity::EffectivePolicy(s);
    EXPECT_TRUE(p.domain_whitelist.empty());
    EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.example/"));
    EXPECT_FALSE(sec.AllowNavigation(p, "https://news.example/a"));
    EXPECT_FALSE(sec.AllowClick(p, "about:blank", "#go"));
    EXPECT_FALSE(sec.AllowClick(p, "https://evil.example/", "#go"));
    EXPECT_FALSE(sec.AllowType(p, "https://evil.example/", "#q", "x"));
    // Given a value for its address, its run window still opens no page.
    s.default_variables["url"] = "https://evil.example/";
    EXPECT_EQ(AutomationSecurity::RunWindowStartURL(s, 0), "");
  }
  // Another workflow may run it, and adds none of its sites to its run's.
  EXPECT_TRUE(AutomationSecurity::MayRunAsChild(s));
  EXPECT_TRUE(AutomationSecurity::EffectivePolicy(s).domain_whitelist.empty());
  // Trusted (its own click in molt://ai-automation) is any site, as for
  // every workflow.
  s.security.trust = TrustLevel::TRUSTED;
  EXPECT_TRUE(sec.AllowNavigation(AutomationSecurity::EffectivePolicy(s),
                                  "https://evil.example/"));
  // A step that opens a fixed address adds its site, and only it.
  s.security.trust = TrustLevel::CASUAL;
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "https://news.example/"));
  const SecurityPolicy with_site = AutomationSecurity::EffectivePolicy(s);
  EXPECT_TRUE(sec.AllowNavigation(with_site, "https://news.example/a"));
  EXPECT_TRUE(sec.AllowNavigation(with_site, "https://www.news.example/"));
  EXPECT_FALSE(sec.AllowNavigation(with_site, "https://evil.example/"));
}

// HostsReferencedBy() lists no site for an address whose host, port or
// user name a value may give or change, nor for one the URL parser could
// read otherwise than it is written (a tab, slashes it skips, a '\'), so
// the review would list none for it. A change with such an address that
// doesn't start with its value isn't confirmed with none, so no fixed host
// goes unshown; nor with sites, having none to confirm. It stays a draft,
// as before. The same change whose addresses each start with a value is
// confirmed with none.
TEST(AutomationSecurityTest, AnAddressAValueMayMoveIsNotConfirmed) {
  for (const char* target :
       {"https://{{host}}/x", "https://shop.{{tld}}/",
        "https://shop.example{{x}}", "https://a@{{x}}/",
        "https://shop.example:{{p}}/", "https://a@shop.example/{{x}}",
        "https:///{{x}}", "https://\\{{x}}", "https://\t/b{{x}}",
        "https://shop.ex\tample/{{x}}", "https:{{x}}",
        " https://shop.example/{{x}}", "ftp://shop.example/{{x}}",
        "shop.example/{{path}}", "http://shop.example\\@{{x}}",
        "https://shop.example\\{{x}}",
        // A fixed site after a value that may be empty.
        "{{x}}https://attacker.example/c?d={{y}}", "{{x}}//attacker.example/",
        "{{x}}attacker.example/", "{{a}}{{b}}", "{{x{{y}}/"}) {
    Script s;
    s.id = "wf_shop";
    s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
    s.steps.push_back(MakeStep(StepType::NAVIGATE, target));
    s.security.sites_unconfirmed = true;
    s.security.changed_outside = true;
    EXPECT_TRUE(AutomationSecurity::HostsReferencedBy(s.steps).empty())
        << target;
    EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(s)) << target;
    EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {})) << target;
    EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {"shop.example"}))
        << target;
    EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {"attacker.example"}))
        << target;
    EXPECT_TRUE(s.security.sites_unconfirmed) << target;
    EXPECT_TRUE(s.security.changed_outside) << target;
    EXPECT_FALSE(AutomationSecurity::MayRunAsChild(s)) << target;
  }
  // Each address a value's (then only a path, query or fragment):
  // confirmed with none.
  Script s;
  s.id = "wf_shop";
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{shop}}/search?q={{q}}"));
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{shop}}?q=1"));
  s.steps.push_back(MakeStep(StepType::NAVIGATE, "{{shop}}#top"));
  s.security.sites_unconfirmed = true;
  s.security.changed_outside = true;
  EXPECT_TRUE(AutomationSecurity::MayConfirmWithoutSites(s));
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(s, {}));
  AutomationSecurity sec;
  const SecurityPolicy p = AutomationSecurity::EffectivePolicy(s);
  EXPECT_TRUE(p.domain_whitelist.empty());
  EXPECT_FALSE(sec.AllowNavigation(p, "https://shop.example/search?q=x"));
  s.default_variables["url"] = "https://shop.example/";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(s, 0), "");
  EXPECT_TRUE(AutomationSecurity::MayRunAsChild(s));
}

// A fixed host before an address's first {{variable}}, its whole authority
// ended by '/', '?' or '#', is a site of the workflow's: the review lists
// it, the change is confirmed only on it, and no value moves the address
// off it, so the step then opens it with any value.
TEST(AutomationSecurityTest, AFixedHostBeforeAValueIsASite) {
  for (const auto& [target, host] :
       std::vector<std::pair<std::string, std::string>>{
           {"https://shop.example/search?q={{q}}", "shop.example"},
           {"https://shop.example/{{path}}", "shop.example"},
           {"https://Shop.Example:8443/x?q={{q}}", "shop.example"},
           {"http://shop.example?{{q}}", "shop.example"},
           {"https://shop.example#{{q}}", "shop.example"},
           {"http://192.168.1.1/reboot?x={{t}}", "192.168.1.1"}}) {
    std::vector<Step> one;
    one.push_back(MakeStep(StepType::NAVIGATE, target));
    EXPECT_EQ(AutomationSecurity::HostsReferencedBy(one),
              std::vector<std::string>{host})
        << target;
  }

  Script s;
  s.id = "wf_shop";
  s.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://shop.example/search?q={{q}}"));
  s.steps.push_back(MakeStep(StepType::CLICK, ".result"));
  // What the file claims.
  s.security.trust = TrustLevel::TRUSTED;
  s.security.domain_whitelist = {"evil.example"};
  AutomationSecurity::ClampUnvouched(s);
  ASSERT_EQ(s.steps.size(), 2u);
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(s.steps),
            std::vector<std::string>{"shop.example"});
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(s));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {}));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(s, {"evil.example"}));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(
      s, {"shop.example", "evil.example"}));
  EXPECT_TRUE(s.security.sites_unconfirmed);
  // Unconfirmed, it allows nothing.
  AutomationSecurity sec;
  EXPECT_FALSE(sec.AllowNavigation(AutomationSecurity::EffectivePolicy(s),
                                   "https://shop.example/search?q=lamp"));
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(s, {"shop.example"}));
  EXPECT_EQ(s.security.domain_whitelist,
            std::vector<std::string>{"shop.example"});
  EXPECT_EQ(s.security.trust, TrustLevel::CASUAL);
  const SecurityPolicy p = AutomationSecurity::EffectivePolicy(s);
  EXPECT_TRUE(sec.AllowNavigation(p, "https://shop.example/search?q=lamp"));
  EXPECT_TRUE(sec.AllowNavigation(p, "https://www.shop.example/"));
  EXPECT_FALSE(sec.AllowNavigation(p, "https://evil.example/"));
  // Its run window opens it once a value is given, resolved.
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(s, 0), "");
  s.default_variables["q"] = "lamp";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(s, 0),
            "https://shop.example/search?q=lamp");

  // No value moves such an address off its host, as the URL parser reads
  // it.
  for (const char* prefix :
       {"https://shop.example/search?q=", "https://shop.example/",
        "https://shop.example#"}) {
    for (const char* value :
         {"", "@evil.example", "//evil.example/", "\\evil.example",
          ":@evil.example/", "\t@evil.example", "%40evil.example"}) {
      const GURL resolved(std::string(prefix) + value);
      EXPECT_TRUE(resolved.is_valid()) << prefix << value;
      EXPECT_EQ(std::string(resolved.host()), "shop.example")
          << prefix << value;
    }
  }

  // A registry (a public suffix) written as the host matches only itself.
  Script suffix;
  suffix.steps.push_back(MakeStep(StepType::NAVIGATE, "https://com/{{x}}"));
  EXPECT_EQ(AutomationSecurity::HostsReferencedBy(suffix.steps),
            std::vector<std::string>{"com"});
  const SecurityPolicy suffix_policy =
      AutomationSecurity::EffectivePolicy(suffix);
  EXPECT_TRUE(sec.AllowNavigation(suffix_policy, "https://com/"));
  EXPECT_FALSE(sec.AllowNavigation(suffix_policy, "https://evil.com/"));
}

// Only a file changed outside, below TRUSTED and not a routine taught by
// voice, whose steps target no site, is confirmed with none.
TEST(AutomationSecurityTest, OnlyAChangeWithNoSiteIsConfirmedWithNone) {
  // A draft from an agent run (confirming it says where the model may go).
  Script draft;
  draft.id = "agent_1";
  draft.security.sites_unconfirmed = true;
  draft.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(draft));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(draft, {}));
  EXPECT_TRUE(draft.security.sites_unconfirmed);
  // A routine taught by voice (voice mode acts on the tab in front of the
  // user), changed outside, with no site.
  Script routine = VoiceRoutine();
  routine.steps[0].target = "{{url}}";
  routine.security.sites_unconfirmed = true;
  routine.security.changed_outside = true;
  routine.security.trust = TrustLevel::CASUAL;
  routine.security.domain_whitelist.clear();
  ASSERT_TRUE(AutomationSecurity::HostsReferencedBy(routine.steps).empty());
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(routine));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(routine, {}));
  EXPECT_TRUE(routine.security.sites_unconfirmed);
  // A change with a site: confirmed only with it.
  Script with_site = RichScript();
  AutomationSecurity::ClampUnvouched(with_site);
  ASSERT_FALSE(AutomationSecurity::HostsReferencedBy(with_site.steps).empty());
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(with_site));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(with_site, {}));
  EXPECT_TRUE(with_site.security.sites_unconfirmed);
  // Not a draft (nothing to confirm).
  Script plain;
  plain.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
  plain.security.changed_outside = true;
  EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(plain));
  EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(plain, {}));
  // A change left TRUSTED or ADMIN (molt://ai-automation's save of a stored
  // workflow keeps its trust): confirmed with no site, it would act on any.
  for (TrustLevel trust : {TrustLevel::TRUSTED, TrustLevel::ADMIN}) {
    Script trusted;
    trusted.steps.push_back(MakeStep(StepType::NAVIGATE, "{{url}}"));
    trusted.security.sites_unconfirmed = true;
    trusted.security.changed_outside = true;
    trusted.security.trust = trust;
    EXPECT_FALSE(AutomationSecurity::MayConfirmWithoutSites(trusted));
    EXPECT_FALSE(AutomationSecurity::ConfirmDraftSites(trusted, {}));
    EXPECT_TRUE(trusted.security.sites_unconfirmed);
  }
  // A change whose draft carries a whitelist (a page's save): confirmed
  // with none, it has none.
  Script listed;
  listed.steps.push_back(MakeStep(StepType::WAIT, "", "1"));
  listed.security.sites_unconfirmed = true;
  listed.security.changed_outside = true;
  listed.security.domain_whitelist = {"evil.example"};
  EXPECT_TRUE(AutomationSecurity::MayConfirmWithoutSites(listed));
  ASSERT_TRUE(AutomationSecurity::ConfirmDraftSites(listed, {}));
  EXPECT_TRUE(listed.security.domain_whitelist.empty());
  EXPECT_FALSE(AutomationSecurity().AllowNavigation(
      AutomationSecurity::EffectivePolicy(listed), "https://evil.example/"));
  // No steps at all: no page, confirmed with none.
  Script empty_change;
  empty_change.security.sites_unconfirmed = true;
  empty_change.security.changed_outside = true;
  EXPECT_TRUE(AutomationSecurity::MayConfirmWithoutSites(empty_change));
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

// A change confirmed on the one site its review listed (a page host),
// its first step opening |first|.
Script ConfirmedOnItsListedSite(const std::string& first) {
  Script confirmed;
  confirmed.id = "wf_1";
  confirmed.security.sites_unconfirmed = true;
  confirmed.security.changed_outside = true;
  confirmed.steps.push_back(MakeStep(StepType::NAVIGATE, first));
  Step wait = MakeStep(StepType::WAIT, "", "1");
  base::DictValue extra;
  extra.Set("host", "news.example");
  wait.extra = base::Value(std::move(extra));
  confirmed.steps.push_back(std::move(wait));
  std::vector<std::string> sites =
      AutomationSecurity::HostsReferencedBy(confirmed.steps);
  EXPECT_TRUE(AutomationSecurity::ConfirmDraftSites(confirmed, sites));
  return confirmed;
}

// The page a run window opens before its runner's checks, in the user's
// profile: only the first NAVIGATE step's URL as the runner opens it (its
// {{variables}} given the run's values), when it is http(s) and one of the
// workflow's sites. A {{variable}} URL's site is one the review listed only
// when its host is written before its first value (HostsReferencedBy());
// either way only its resolved URL is checked.
TEST(AutomationSecurityTest, ARunWindowOpensOnlyAPageOfItsSites) {
  // A {{variable}} URL whose host is a value, which the review didn't
  // list, with or without a value for it.
  Script unlisted =
      ConfirmedOnItsListedSite("http://{{router}}/reboot?x={{t}}");
  ASSERT_FALSE(unlisted.security.sites_unconfirmed);
  EXPECT_EQ(unlisted.security.domain_whitelist,
            std::vector<std::string>{"news.example"});
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(unlisted, 0), "");
  unlisted.default_variables["router"] = "192.168.1.1";
  unlisted.default_variables["t"] = "1";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(unlisted, 0), "");
  // A fixed host before its value is listed: its page, resolved, once a
  // value is given.
  Script listed = ConfirmedOnItsListedSite("http://192.168.1.1/reboot?x={{t}}");
  ASSERT_FALSE(listed.security.sites_unconfirmed);
  EXPECT_EQ(listed.security.domain_whitelist,
            (std::vector<std::string>{"192.168.1.1", "news.example"}));
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(listed, 0), "");
  listed.default_variables["t"] = "1";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(listed, 0),
            "http://192.168.1.1/reboot?x=1");
  // Nor a later step's page, before the first's.
  unlisted.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://news.example/today"));
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(unlisted, 0), "");
  // A {{variable}} URL on one of its sites with no value for it, at any
  // trust: not the page the step opens.
  Script templated = ConfirmedOnItsListedSite("https://news.example/?q={{q}}");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(templated, 0), "");
  templated.security.trust = TrustLevel::TRUSTED;
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(templated, 0), "");
  // With one, the page the step opens.
  templated.security.trust = TrustLevel::APPROVED;
  templated.default_variables["q"] = "tides";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(templated, 0),
            "https://news.example/?q=tides");
  // A static page of its sites.
  Script fixed = ConfirmedOnItsListedSite("https://news.example/today");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(fixed, 0),
            "https://news.example/today");
  // Never a page that isn't http(s).
  for (const char* code : {"javascript:alert(1)", "file:///etc/passwd"}) {
    EXPECT_EQ(AutomationSecurity::RunWindowStartURL(
                  ConfirmedOnItsListedSite(code), 0),
              "")
        << code;
  }
  // No page to open.
  fixed.steps.erase(fixed.steps.begin());
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(fixed, 0), "");
  // An unconfirmed draft, a change not confirmed yet and a routine taught
  // by voice open nothing.
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(UnconfirmedDraft(), 0), "");
  Script changed = RichScript();
  ASSERT_EQ(AutomationSecurity::RunWindowStartURL(changed, 0),
            "https://shop.example/");
  AutomationSecurity::ClampUnvouched(changed);
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(changed, 0), "");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(VoiceRoutine(), 0), "");
}

// A confirmed workflow on shop.example whose first page has a {{variable}}.
Script ShopSearch() {
  Script shop;
  shop.id = "wf_shop";
  shop.security.trust = TrustLevel::APPROVED;
  shop.security.domain_whitelist = {"shop.example"};
  shop.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://shop.example/search?q={{item}}"));
  shop.steps.push_back(MakeStep(StepType::CLICK, ".result"));
  shop.steps.push_back(MakeStep(StepType::EXTRACT, ".price"));
  shop.default_variables["item"] = "lamp";
  return shop;
}

// A run started at a later step (molt://ai-automation's "retry from") skips
// the steps before it, so its window opens the page that step expects: the
// last NAVIGATE before it (with none before it, the first), resolved with
// the run's values and checked as the runner checks that step.
TEST(AutomationSecurityTest, ARunWindowOpensThePageItsFirstStepExpects) {
  Script shop = ShopSearch();
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 0),
            "https://shop.example/search?q=lamp");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1),
            "https://shop.example/search?q=lamp");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 2),
            "https://shop.example/search?q=lamp");
  // Out of range: the runner starts at the first step.
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 99),
            "https://shop.example/search?q=lamp");
  // A step before the first NAVIGATE.
  shop.steps.insert(shop.steps.begin(), MakeStep(StepType::CLICK, ".cookies"));
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 0),
            "https://shop.example/search?q=lamp");

  // No value for a variable, or one that only a step sets: nothing.
  shop = ShopSearch();
  shop.default_variables.clear();
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1), "");
  shop.default_variables["item"] = "{{other}}";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1), "");

  // A value that takes it off its sites, or off http(s): nothing.
  shop = ShopSearch();
  shop.steps[0].target = "https://{{store}}/search";
  shop.default_variables["store"] = "www.shop.example";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1),
            "https://www.shop.example/search");
  shop.default_variables["store"] = "evil.example";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1), "");
  shop.steps[0].target = "{{page}}";
  shop.default_variables["page"] = "javascript:alert(1)";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(shop, 1), "");

  // Several pages: the last one before the step, never an earlier one.
  Script tour;
  tour.id = "wf_tour";
  tour.security.trust = TrustLevel::APPROVED;
  tour.security.domain_whitelist = {"shop.example"};
  tour.steps.push_back(MakeStep(StepType::NAVIGATE, "https://shop.example/a"));
  tour.steps.push_back(MakeStep(StepType::CLICK, ".next"));
  tour.steps.push_back(
      MakeStep(StepType::NAVIGATE, "https://shop.example/b?q={{item}}"));
  tour.steps.push_back(MakeStep(StepType::CLICK, ".buy"));
  tour.steps.push_back(MakeStep(StepType::NAVIGATE, "https://shop.example/c"));
  tour.default_variables["item"] = "lamp";
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 0),
            "https://shop.example/a");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 1),
            "https://shop.example/a");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 2),
            "https://shop.example/a");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 3),
            "https://shop.example/b?q=lamp");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 4),
            "https://shop.example/b?q=lamp");
  tour.default_variables.clear();
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 3), "");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(tour, 1),
            "https://shop.example/a");

  // A draft, a change not confirmed yet and a routine taught by voice open
  // nothing from any step.
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(UnconfirmedDraft(), 1), "");
  Script changed = RichScript();
  AutomationSecurity::ClampUnvouched(changed);
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(changed, 1), "");
  EXPECT_EQ(AutomationSecurity::RunWindowStartURL(VoiceRoutine(), 1), "");
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
