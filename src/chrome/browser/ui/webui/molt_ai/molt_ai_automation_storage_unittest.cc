// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt://ai-automation's storage steps (molt_ai_automation_storage.h) on a
// temporary directory: what each message reads and changes, as it did on
// the UI thread, now as one step on the storage's sequence; with a workflow
// record (workflow_record.h), a workflow changed outside the browser is
// neither trusted nor laundered here; a page never moves a last fire later,
// and Snooze and Skip are holds in the record, refused for what they can't
// hold.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_automation_storage.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/automation/workflow_record_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation_page {
namespace {

using automation::AutomationStorage;
using automation::Script;
using automation::TrustLevel;

// The clock a save is given.
constexpr int64_t kNow = 1'800'000'000;

Script Workflow(const std::string& id, const std::string& name) {
  Script script;
  script.id = id;
  script.name = name;
  automation::Step step;
  step.type = automation::StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

Script VoiceRoutine(const std::string& id, const std::string& name) {
  Script script = Workflow(id, name);
  automation::Step step;
  step.type = automation::StepType::NOTIFY;
  base::DictValue voice;
  voice.Set("command", "open youtube");
  base::DictValue extra;
  extra.Set(automation::kVoiceStepKey, std::move(voice));
  step.extra = base::Value(std::move(extra));
  script.steps.push_back(std::move(step));
  return script;
}

class AutomationPageStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    storage_.emplace(Dir());
  }

  AutomationStorage& storage() { return *storage_; }
  base::FilePath Dir() { return temp_.GetPath().AppendASCII("automations"); }
  // The directory's workflow record, as the browser has one for the shared
  // storage (workflow_record.h).
  void UseRecord() {
    record_.emplace(Dir(),
                    temp_.GetPath().Append(automation::kWorkflowRecordFileName),
                    /*may_seed=*/false);
  }
  // A file written by another program.
  void Plant(const Script& script) {
    ASSERT_TRUE(base::CreateDirectory(Dir()));
    ASSERT_TRUE(base::WriteFile(Dir().AppendASCII(script.id + ".molt"),
                                script.ToJSONString()));
  }
  std::string Bytes(const std::string& id) {
    std::string bytes;
    EXPECT_TRUE(
        base::ReadFileToString(Dir().AppendASCII(id + ".molt"), &bytes));
    return bytes;
  }

  base::ScopedTempDir temp_;
  std::optional<automation::ScopedWorkflowRecordForTesting> record_;
  std::optional<AutomationStorage> storage_;
};

// What a file written outside the browser might claim.
Script Claiming(const std::string& id) {
  Script script = Workflow(id, "Prices");
  script.security.trust = TrustLevel::TRUSTED;
  script.security.domain_whitelist = {"evil.example"};
  script.stats.runs = 9;
  script.stats.successes = 9;
  return script;
}

TEST_F(AutomationPageStorageTest,
       RoutinesTaughtByVoiceAreNeitherListedNorChanged) {
  ASSERT_TRUE(storage().Save(Workflow("wf_1", "Prices")));
  ASSERT_TRUE(automation::HasVoiceSteps(VoiceRoutine("voice_1", "news")));
  ASSERT_TRUE(storage().Save(VoiceRoutine("voice_1", "news")));
  const std::vector<Script> listed = ListWorkflows(storage());
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed[0].id, "wf_1");
  EXPECT_FALSE(LoadWorkflow("voice_1", storage()));
  EXPECT_FALSE(DeleteWorkflow("voice_1", storage()));
  EXPECT_TRUE(storage().Load("voice_1"));
  // Saved over, or saved as one: neither.
  base::DictValue over = SaveWorkflow(Workflow("voice_1", "mine"), kNow,
                                      /*schedule_saved=*/false, storage());
  EXPECT_EQ(over.FindBool("success"), false);
  EXPECT_EQ(*over.FindString("error"), kVoiceRoutinesElsewhere);
  base::DictValue as_one = SaveWorkflow(VoiceRoutine("wf_9", "news"), kNow,
                                        /*schedule_saved=*/false, storage());
  EXPECT_EQ(as_one.FindBool("success"), false);
  EXPECT_EQ(storage().Load("voice_1")->name, "news");
  EXPECT_FALSE(storage().Load("wf_9"));
  // Nor trusted.
  base::DictValue trust =
      PromoteTrust("voice_1", TrustLevel::TRUSTED, storage());
  EXPECT_EQ(*trust.FindString("error"), "script not found");
}

// A save keeps the trust stored (changed only with a click); a new one
// starts Casual, whatever the page says.
TEST_F(AutomationPageStorageTest, ASaveKeepsTheStoredTrust) {
  Script first = Workflow("wf_1", "Prices");
  first.security.trust = TrustLevel::ADMIN;
  EXPECT_EQ(
      SaveWorkflow(std::move(first), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  EXPECT_EQ(storage().Load("wf_1")->security.trust, TrustLevel::CASUAL);
  base::DictValue promoted =
      PromoteTrust("wf_1", TrustLevel::APPROVED, storage());
  EXPECT_EQ(promoted.FindBool("success"), true);
  EXPECT_EQ(*promoted.FindString("trust"), "approved");
  Script edit = Workflow("wf_1", "Prices, edited");
  edit.security.trust = TrustLevel::ADMIN;
  EXPECT_EQ(
      SaveWorkflow(std::move(edit), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  std::optional<Script> saved = storage().Load("wf_1");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->name, "Prices, edited");
  EXPECT_EQ(saved->security.trust, TrustLevel::APPROVED);
  // The trust change is in the audit log.
  bool logged = false;
  for (const std::string& line : AuditTail(50, storage())) {
    logged |= line.find("wf_1 | trust_changed | casual -> approved") !=
              std::string::npos;
  }
  EXPECT_TRUE(logged);
}

// An unconfirmed draft is trusted only once its sites are confirmed.
TEST_F(AutomationPageStorageTest, ADraftIsNotTrustedHere) {
  Script draft = Workflow("agent_1", "Draft");
  draft.security.sites_unconfirmed = true;
  ASSERT_TRUE(storage().Save(draft));
  base::DictValue trust =
      PromoteTrust("agent_1", TrustLevel::APPROVED, storage());
  EXPECT_EQ(trust.FindBool("success"), false);
  EXPECT_EQ(*trust.FindString("error"),
            "Confirm this draft's sites in Agent mode first.");
  EXPECT_EQ(storage().Load("agent_1")->security.trust, TrustLevel::CASUAL);
  EXPECT_EQ(*PromoteTrust("gone", TrustLevel::APPROVED, storage())
                 .FindString("error"),
            "script not found");
}

// Imports and blank workflows never replace one: a free id is picked under
// the storage's lock.
TEST_F(AutomationPageStorageTest, NewWorkflowsNeverReplaceOne) {
  ASSERT_TRUE(storage().Save(Workflow("wf_1", "Prices")));
  Script imported = Workflow("wf_1", "Imported");
  imported.security.trust = TrustLevel::ADMIN;
  imported.stats.runs = 9;
  base::DictValue result =
      ImportWorkflow(std::move(imported), 1'800'000'000, storage());
  EXPECT_EQ(result.FindBool("success"), true);
  EXPECT_EQ(*result.FindString("script_id"), "wf_1-imported-1800000000");
  std::optional<Script> copy = storage().Load("wf_1-imported-1800000000");
  ASSERT_TRUE(copy);
  EXPECT_EQ(copy->security.trust, TrustLevel::CASUAL);
  EXPECT_EQ(copy->stats.runs, 0);
  EXPECT_EQ(storage().Load("wf_1")->name, "Prices");
  // The same import again in the same second: another id still.
  base::DictValue again =
      ImportWorkflow(Workflow("wf_1", "Imported"), 1'800'000'000, storage());
  EXPECT_EQ(*again.FindString("script_id"), "wf_1-imported-1800000000_");

  EXPECT_EQ(SaveBlank(Workflow("new-5", "New automation"), storage()), "new-5");
  EXPECT_EQ(SaveBlank(Workflow("new-5", "New automation"), storage()),
            "new-5_");
  // The sample has a fixed id: saved over itself.
  EXPECT_TRUE(SaveSample(Workflow("sample", "Sample"), storage()));
  EXPECT_TRUE(SaveSample(Workflow("sample", "Sample"), storage()));
  EXPECT_FALSE(storage().Load("sample_"));
}

// A run asked for: read, and said in the audit log from where it starts.
TEST_F(AutomationPageStorageTest, ARunAskedForIsLogged) {
  ASSERT_TRUE(storage().Save(Workflow("wf_1", "Prices")));
  EXPECT_TRUE(LoadToRun("wf_1", 0, storage()));
  EXPECT_TRUE(LoadToRun("wf_1", 7, storage()));
  EXPECT_FALSE(LoadToRun("gone", 0, storage()));
  Script one = Workflow("x", "x");
  EXPECT_EQ(StartIndex(one, 7), 1u);
  EXPECT_EQ(StartIndex(one, -3), 0u);
  std::vector<std::string> asked;
  for (const std::string& line : AuditTail(50, storage())) {
    if (line.find("manual_run_requested") != std::string::npos) {
      asked.push_back(line.substr(line.rfind('|') + 2));
    }
  }
  EXPECT_EQ(asked, (std::vector<std::string>{"from_ui", "from_step=1"}));
}

// A workflow whose file was changed outside the browser is trusted only
// once it is reviewed in Agent mode.
TEST_F(AutomationPageStorageTest, PromoteTrustRefusesAWorkflowChangedOutside) {
  UseRecord();
  Plant(Claiming("wf_1"));
  const std::string bytes = Bytes("wf_1");
  base::DictValue trust = PromoteTrust("wf_1", TrustLevel::ADMIN, storage());
  EXPECT_EQ(trust.FindBool("success"), false);
  EXPECT_EQ(*trust.FindString("error"),
            "This workflow was changed outside MoltBrowser. Review it in "
            "Agent mode first.");
  EXPECT_EQ(Bytes("wf_1"), bytes);
}

// Saving it here keeps it a draft (it isn't refused: the page's edits are
// kept, and the review is still to do).
TEST_F(AutomationPageStorageTest, ASaveKeepsAWorkflowChangedOutsideADraft) {
  UseRecord();
  Plant(Claiming("wf_1"));
  std::optional<Script> shown = LoadWorkflow("wf_1", storage());
  ASSERT_TRUE(shown);
  EXPECT_TRUE(shown->security.changed_outside);
  shown->name = "Prices, edited";
  EXPECT_EQ(
      SaveWorkflow(std::move(*shown), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  std::optional<Script> saved = storage().Load("wf_1");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->name, "Prices, edited");
  EXPECT_EQ(saved->security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(saved->security.sites_unconfirmed);
  EXPECT_TRUE(saved->security.changed_outside);
  EXPECT_TRUE(saved->security.domain_whitelist.empty());
}

// An import is the browser's own write: vouched for, at CASUAL, its history
// cleared.
TEST_F(AutomationPageStorageTest, AnImportIsVouchedAtCasual) {
  UseRecord();
  base::DictValue result =
      ImportWorkflow(Claiming("wf_9"), 1'800'000'000, storage());
  ASSERT_EQ(result.FindBool("success"), true);
  std::optional<Script> imported =
      storage().Load(*result.FindString("script_id"));
  ASSERT_TRUE(imported);
  EXPECT_FALSE(imported->security.changed_outside);
  EXPECT_FALSE(imported->security.sites_unconfirmed);
  EXPECT_EQ(imported->security.trust, TrustLevel::CASUAL);
  EXPECT_EQ(imported->stats.runs, 0);
  // No sites of its file's (which the page doesn't show): only the hosts
  // its steps name count.
  EXPECT_TRUE(imported->security.domain_whitelist.empty());
  EXPECT_EQ(automation::AutomationSecurity::EffectivePolicy(*imported)
                .domain_whitelist,
            std::vector<std::string>{"news.example"});
  // Trusted with a click, as ever.
  EXPECT_EQ(
      PromoteTrust("wf_9", TrustLevel::APPROVED, storage()).FindBool("success"),
      true);
}

// An import keeps no sites of its file's, but the site of an address with
// a value after its fixed host is one its steps name: such a workflow
// still opens that site once imported, and no other.
TEST_F(AutomationPageStorageTest, AnImportKeepsTheSiteOfAnAddressWithAValue) {
  UseRecord();
  Script file = Claiming("wf_9");
  file.steps[0].target = "https://shop.example/search?q={{q}}";
  file.security.domain_whitelist = {"shop.example"};
  base::DictValue result = ImportWorkflow(std::move(file), kNow, storage());
  ASSERT_EQ(result.FindBool("success"), true);
  std::optional<Script> imported =
      storage().Load(*result.FindString("script_id"));
  ASSERT_TRUE(imported);
  EXPECT_TRUE(imported->security.domain_whitelist.empty());
  const automation::SecurityPolicy policy =
      automation::AutomationSecurity::EffectivePolicy(*imported);
  EXPECT_EQ(policy.domain_whitelist, std::vector<std::string>{"shop.example"});
  automation::AutomationSecurity security;
  EXPECT_TRUE(
      security.AllowNavigation(policy, "https://shop.example/search?q=lamp"));
  EXPECT_FALSE(security.AllowNavigation(policy, "https://evil.example/"));
}

// An import starts its schedule over: a last fire its JSON gives (an
// exported workflow's, long past; one in 2100) was never this browser's, so
// it neither forces a catch-up run nor holds the schedule back. An
// interval counts from the import.
TEST_F(AutomationPageStorageTest, AnImportStartsItsScheduleOver) {
  UseRecord();
  constexpr int64_t kImportedAt = 1'800'000'000;
  Script weekly = Workflow("wf_weekly", "Weekly");
  weekly.trigger.type = automation::TriggerType::CRON;
  weekly.trigger.expression = "0 9 * * 1";
  weekly.trigger.last_fired_unix = 1;
  Script hourly = Workflow("wf_hourly", "Hourly");
  hourly.trigger.type = automation::TriggerType::INTERVAL;
  hourly.trigger.expression = "3600";
  hourly.trigger.last_fired_unix = 4'102'444'800;
  base::DictValue cron_result =
      ImportWorkflow(std::move(weekly), kImportedAt, storage());
  base::DictValue interval_result =
      ImportWorkflow(std::move(hourly), kImportedAt, storage());
  ASSERT_EQ(cron_result.FindBool("success"), true);
  ASSERT_EQ(interval_result.FindBool("success"), true);
  std::optional<Script> cron =
      storage().Load(*cron_result.FindString("script_id"));
  std::optional<Script> interval =
      storage().Load(*interval_result.FindString("script_id"));
  ASSERT_TRUE(cron);
  ASSERT_TRUE(interval);
  EXPECT_FALSE(cron->security.changed_outside);
  EXPECT_EQ(cron->trigger.expression, "0 9 * * 1");
  EXPECT_EQ(cron->trigger.last_fired_unix, 0);
  EXPECT_FALSE(interval->security.changed_outside);
  EXPECT_EQ(interval->trigger.last_fired_unix, kImportedAt);
  // Both kept, turned off: they run by themselves only once the user turns
  // them on (Save schedule), and the page and the audit log say so.
  EXPECT_FALSE(cron->enabled);
  EXPECT_FALSE(interval->enabled);
  EXPECT_FALSE(automation::AutomationSecurity::MayFireUnattended(*cron));
  EXPECT_EQ(cron_result.FindBool("schedule_off"), true);
  bool logged = false;
  for (const std::string& line : AuditTail(50, storage())) {
    logged |=
        line.find("imported | from_ui, schedule off") != std::string::npos;
  }
  EXPECT_TRUE(logged);
  // A manual import is left as it is.
  base::DictValue manual =
      ImportWorkflow(Workflow("wf_manual", "Manual"), kImportedAt, storage());
  EXPECT_EQ(manual.FindBool("schedule_off"), false);
  EXPECT_TRUE(storage().Load(*manual.FindString("script_id"))->enabled);
}

// Whatever an imported file says about what it may do, it gets the default
// approvals and limits: below Approved it can't submit a form or fill a
// password by leaving them off its list.
TEST_F(AutomationPageStorageTest, AnImportGetsTheDefaultApprovalsAndLimits) {
  UseRecord();
  Script file = Workflow("wf_9", "Prices");
  file.security.require_approval_for = {"x"};
  file.security.max_runtime_seconds = std::numeric_limits<int>::max();
  file.security.max_navigations = 100000;
  file.security.max_network_bytes = -1;
  base::DictValue result = ImportWorkflow(std::move(file), kNow, storage());
  ASSERT_EQ(result.FindBool("success"), true);
  std::optional<Script> imported =
      storage().Load(*result.FindString("script_id"));
  ASSERT_TRUE(imported);
  const automation::SecurityPolicy defaults;
  EXPECT_EQ(imported->security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  EXPECT_EQ(imported->security.max_runtime_seconds,
            defaults.max_runtime_seconds);
  EXPECT_EQ(imported->security.max_navigations, defaults.max_navigations);
  EXPECT_EQ(imported->security.max_network_bytes, defaults.max_network_bytes);
}

// A save keeps the policy stored, whatever the page's dict says.
TEST_F(AutomationPageStorageTest, ASaveKeepsTheStoredPolicy) {
  Script stored = Workflow("wf_1", "Prices");
  stored.security.max_runtime_seconds = 120;
  stored.security.domain_whitelist = {"news.example"};
  ASSERT_TRUE(storage().Save(stored));
  Script edit = Workflow("wf_1", "Prices, edited");
  edit.security.require_approval_for = {"x"};
  edit.security.max_runtime_seconds = 100000;
  edit.security.domain_whitelist = {"com"};
  EXPECT_EQ(
      SaveWorkflow(std::move(edit), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  std::optional<Script> saved = storage().Load("wf_1");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->name, "Prices, edited");
  EXPECT_EQ(saved->security.require_approval_for,
            stored.security.require_approval_for);
  EXPECT_EQ(saved->security.max_runtime_seconds, 120);
  EXPECT_EQ(saved->security.domain_whitelist,
            std::vector<std::string>{"news.example"});
}

// A save of a workflow not stored (deleted in another tab, or a dict the
// page made) is taken as an import: no runs, no last fire (an early one
// would force a catch-up run), the policy floor, and its schedule off.
TEST_F(AutomationPageStorageTest, ASaveOfAWorkflowNotStoredIsAnImport) {
  UseRecord();
  Script made = Claiming("wf_new");
  made.security.require_approval_for = {"x"};
  made.trigger.type = automation::TriggerType::CRON;
  made.trigger.expression = "* * * * *";
  made.trigger.last_fired_unix = 1;
  EXPECT_EQ(
      SaveWorkflow(std::move(made), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  std::optional<Script> saved = storage().Load("wf_new");
  ASSERT_TRUE(saved);
  EXPECT_EQ(saved->security.trust, TrustLevel::CASUAL);
  EXPECT_EQ(saved->stats.runs, 0);
  EXPECT_EQ(saved->trigger.last_fired_unix, 0);
  EXPECT_EQ(saved->security.require_approval_for,
            (std::vector<std::string>{"form_submit", "payment", "login"}));
  EXPECT_FALSE(saved->enabled);
  EXPECT_TRUE(saved->security.domain_whitelist.empty());
}

// An interval saved with no last fire (molt://ai-automation's Save schedule
// sends none) counts from the save, so it fires an interval later; the
// scheduler would count one with none from each listing, and never fire it.
// Turning an imported schedule on is that save.
TEST_F(AutomationPageStorageTest, AnIntervalSavedWithNoLastFireCountsFromIt) {
  UseRecord();
  Script hourly = Workflow("wf_hourly", "Hourly");
  hourly.trigger.type = automation::TriggerType::INTERVAL;
  hourly.trigger.expression = "3600";
  base::DictValue imported = ImportWorkflow(std::move(hourly), 1, storage());
  const std::string id = *imported.FindString("script_id");
  std::optional<Script> shown = LoadWorkflow(id, storage());
  ASSERT_TRUE(shown);
  ASSERT_FALSE(shown->enabled);
  // Tick Enabled, Save schedule: the page sends the trigger anew.
  shown->enabled = true;
  shown->trigger.last_fired_unix = 0;
  EXPECT_EQ(
      SaveWorkflow(std::move(*shown), kNow, /*schedule_saved=*/true, storage())
          .FindBool("success"),
      true);
  std::optional<Script> saved = storage().Load(id);
  ASSERT_TRUE(saved);
  EXPECT_TRUE(saved->enabled);
  EXPECT_EQ(saved->trigger.last_fired_unix, kNow);
  EXPECT_TRUE(automation::AutomationSecurity::MayFireUnattended(*saved));
}

// An interval an earlier version saved with no last fire (shown "not
// started") is started only by Save schedule: saving its steps or values
// leaves it as it is.
TEST_F(AutomationPageStorageTest,
       OnlySaveScheduleStartsAnIntervalNeverStarted) {
  UseRecord();
  Script hourly = Workflow("wf_hourly", "Hourly");
  hourly.trigger.type = automation::TriggerType::INTERVAL;
  hourly.trigger.expression = "3600";
  ASSERT_TRUE(storage().Save(hourly));
  ASSERT_EQ(storage().Load("wf_hourly")->trigger.last_fired_unix, 0);
  Script steps_saved = *storage().Load("wf_hourly");
  steps_saved.steps[0].description = "Open the news";
  EXPECT_EQ(SaveWorkflow(std::move(steps_saved), kNow,
                         /*schedule_saved=*/false, storage())
                .FindBool("success"),
            true);
  EXPECT_EQ(storage().Load("wf_hourly")->trigger.last_fired_unix, 0);
  EXPECT_EQ(storage().Load("wf_hourly")->steps[0].description, "Open the news");
  Script rescheduled = *storage().Load("wf_hourly");
  EXPECT_EQ(SaveWorkflow(std::move(rescheduled), kNow,
                         /*schedule_saved=*/true, storage())
                .FindBool("success"),
            true);
  EXPECT_EQ(storage().Load("wf_hourly")->trigger.last_fired_unix, kNow);
  // A card listed while it was not started (an open card isn't refreshed)
  // saves its steps afterwards: it stays started.
  Script stale = *storage().Load("wf_hourly");
  stale.trigger.last_fired_unix = 0;
  stale.name = "Hourly, renamed";
  EXPECT_EQ(SaveWorkflow(std::move(stale), kNow + 600,
                         /*schedule_saved=*/false, storage())
                .FindBool("success"),
            true);
  EXPECT_EQ(storage().Load("wf_hourly")->name, "Hourly, renamed");
  EXPECT_EQ(storage().Load("wf_hourly")->trigger.last_fired_unix, kNow);
}

// A workflow on |trigger_type| |expression|, saved by the browser with
// |last_fired| as its last fire.
Script Scheduled(const std::string& id,
                 automation::TriggerType trigger_type,
                 const std::string& expression,
                 int64_t last_fired) {
  Script script = Workflow(id, "Prices");
  script.trigger.type = trigger_type;
  script.trigger.expression = expression;
  script.trigger.last_fired_unix = last_fired;
  return script;
}

// A unix time in a page's answer (a number), in whole seconds.
std::optional<int64_t> TimeIn(const base::DictValue& answer,
                              const std::string& key) {
  const std::optional<double> value = answer.FindDouble(key);
  if (!value) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*value);
}

// The holds the audit log has for workflow |id|, oldest first, each as
// "<event> | until=<unix time>".
std::vector<std::string> HoldsLogged(const std::string& id,
                                     AutomationStorage& storage) {
  std::vector<std::string> out;
  const std::string of_id = " | " + id + " | ";
  for (const std::string& line : AuditTail(200, storage)) {
    const size_t at = line.find(of_id);
    if (at == std::string::npos) {
      continue;
    }
    std::string rest = line.substr(at + of_id.size());
    if (rest.starts_with("snoozed | ") || rest.starts_with("run_skipped | ")) {
      out.push_back(std::move(rest));
    }
  }
  return out;
}

// "<event> | until=<held>", as HoldsLogged() gives it.
std::string HoldLine(const std::string& event, int64_t held) {
  return event + " | until=" + base::NumberToString(held);
}

// A page never moves a stored last fire later (a fire time far ahead would
// stall the schedule; Snooze and Skip are holds of their own), whichever
// save it is; an earlier one is left to the storage, which keeps the
// stored one for the same schedule.
TEST_F(AutomationPageStorageTest, APageCannotMoveTheLastFireLater) {
  UseRecord();
  constexpr int64_t kLast = kNow - 600;
  constexpr int64_t kFarAhead = 4'102'444'800;
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_1", automation::TriggerType::INTERVAL, "3600", kLast)));
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_2", automation::TriggerType::CRON, "0 9 * * *", 0)));
  for (const bool schedule_saved : {false, true}) {
    Script ahead = *storage().Load("wf_1");
    ahead.trigger.last_fired_unix = kFarAhead;
    ahead.name = "Prices, renamed";
    EXPECT_EQ(SaveWorkflow(std::move(ahead), kNow, schedule_saved, storage())
                  .FindBool("success"),
              true);
    std::optional<Script> saved = storage().Load("wf_1");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->name, "Prices, renamed");
    EXPECT_EQ(saved->trigger.last_fired_unix, kLast) << schedule_saved;
    EXPECT_FALSE(saved->security.changed_outside);
  }
  // A cron never fired here: still none.
  Script cron = *storage().Load("wf_2");
  cron.trigger.last_fired_unix = kFarAhead;
  EXPECT_EQ(
      SaveWorkflow(std::move(cron), kNow, /*schedule_saved=*/false, storage())
          .FindBool("success"),
      true);
  EXPECT_EQ(storage().Load("wf_2")->trigger.last_fired_unix, 0);
}

// Snooze holds the schedule in this browser's record, from now for the
// seconds given: the file isn't written (still vouched for, as it was), and
// the answer says until when and the next run after it. Each hold set is in
// the audit log ("snoozed", until when).
TEST_F(AutomationPageStorageTest, SnoozeHoldsWithoutWritingTheFile) {
  UseRecord();
  // Every five minutes (in any time zone); kNow is on one.
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_1", automation::TriggerType::CRON, "*/5 * * * *", 0)));
  const std::string bytes = Bytes("wf_1");
  base::DictValue snoozed = Snooze("wf_1", 3600, kNow, storage());
  EXPECT_EQ(snoozed.FindBool("success"), true);
  EXPECT_EQ(TimeIn(snoozed, "held_until_unix"), kNow + 3600);
  EXPECT_EQ(TimeIn(snoozed, "next_fire_unix"), kNow + 3600);
  std::optional<Script> loaded = storage().Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->trigger.held_until_unix, kNow + 3600);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(Bytes("wf_1"), bytes);
  // A shorter one ends nothing.
  EXPECT_EQ(TimeIn(Snooze("wf_1", 60, kNow, storage()), "held_until_unix"),
            kNow + 3600);
  // An interval: its first time at or after the snooze, its phase kept.
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_2", automation::TriggerType::INTERVAL, "900", kNow - 600)));
  base::DictValue interval = Snooze("wf_2", 3600, kNow, storage());
  EXPECT_EQ(interval.FindBool("success"), true);
  EXPECT_EQ(TimeIn(interval, "held_until_unix"), kNow + 3600);
  EXPECT_EQ(TimeIn(interval, "next_fire_unix"), kNow - 600 + 5 * 900);
  // The shorter one logged as the hold it left.
  EXPECT_EQ(HoldsLogged("wf_1", storage()),
            (std::vector<std::string>{HoldLine("snoozed", kNow + 3600),
                                      HoldLine("snoozed", kNow + 3600)}));
  EXPECT_EQ(HoldsLogged("wf_2", storage()),
            std::vector<std::string>{HoldLine("snoozed", kNow + 3600)});
}

// Skip next holds just past the first run after now, so the run after it is
// next; a second Skip skips that one too. An interval keeps its phase, and
// an overdue one (its times passed while it was off) skips its next time
// to come, not the one already passed. Each Skip is in the audit log
// ("run_skipped", until when); one with nothing to skip isn't.
TEST_F(AutomationPageStorageTest, SkipNextHoldsPastTheNextRunOnly) {
  UseRecord();
  const base::Time now = base::Time::FromTimeT(kNow);
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_cron", automation::TriggerType::CRON, "*/5 * * * *", 0)));
  base::DictValue skipped = SkipNext("wf_cron", now, storage());
  EXPECT_EQ(skipped.FindBool("success"), true);
  EXPECT_EQ(TimeIn(skipped, "held_until_unix"), kNow + 300 + 1);
  EXPECT_EQ(TimeIn(skipped, "next_fire_unix"), kNow + 600);
  base::DictValue again = SkipNext("wf_cron", now, storage());
  EXPECT_EQ(TimeIn(again, "held_until_unix"), kNow + 600 + 1);
  EXPECT_EQ(TimeIn(again, "next_fire_unix"), kNow + 900);
  EXPECT_EQ(storage().Load("wf_cron")->trigger.held_until_unix, kNow + 600 + 1);
  // Every 15 minutes, last run 100 s ago: next at 800 s, then 1700 s.
  ASSERT_TRUE(storage().Save(Scheduled(
      "wf_interval", automation::TriggerType::INTERVAL, "900", kNow - 100)));
  base::DictValue interval = SkipNext("wf_interval", now, storage());
  EXPECT_EQ(TimeIn(interval, "held_until_unix"), kNow + 800 + 1);
  EXPECT_EQ(TimeIn(interval, "next_fire_unix"), kNow + 1700);
  // Last run 2000 s ago: due 1100 s and 200 s ago (overdue), next at 700 s.
  ASSERT_TRUE(storage().Save(Scheduled(
      "wf_overdue", automation::TriggerType::INTERVAL, "900", kNow - 2000)));
  base::DictValue overdue = SkipNext("wf_overdue", now, storage());
  EXPECT_EQ(TimeIn(overdue, "held_until_unix"), kNow + 700 + 1);
  EXPECT_EQ(TimeIn(overdue, "next_fire_unix"), kNow + 1600);
  // No run to come (its end has passed): nothing to skip.
  Script ended =
      Scheduled("wf_ended", automation::TriggerType::CRON, "*/5 * * * *", 0);
  ended.trigger.until_unix = kNow - 1;
  ASSERT_TRUE(storage().Save(ended));
  base::DictValue none = SkipNext("wf_ended", now, storage());
  EXPECT_EQ(none.FindBool("success"), false);
  EXPECT_EQ(*none.FindString("error"), "No upcoming run to skip");
  EXPECT_EQ(storage().Load("wf_ended")->trigger.held_until_unix, 0);
  EXPECT_EQ(
      HoldsLogged("wf_cron", storage()),
      (std::vector<std::string>{HoldLine("run_skipped", kNow + 300 + 1),
                                HoldLine("run_skipped", kNow + 600 + 1)}));
  EXPECT_EQ(HoldsLogged("wf_interval", storage()),
            std::vector<std::string>{HoldLine("run_skipped", kNow + 800 + 1)});
  EXPECT_EQ(HoldsLogged("wf_overdue", storage()),
            std::vector<std::string>{HoldLine("run_skipped", kNow + 700 + 1)});
  EXPECT_TRUE(HoldsLogged("wf_ended", storage()).empty());
}

// Snooze and Skip refuse what this page doesn't hold: a routine taught by
// voice or a workflow that isn't there, an unconfirmed draft (in the words
// PromoteTrust uses), a workflow that doesn't repeat, and an interval never
// started (only Save schedule starts it). Nothing is held or logged.
TEST_F(AutomationPageStorageTest, SnoozeAndSkipRefuseWhatTheyCantHold) {
  UseRecord();
  const base::Time now = base::Time::FromTimeT(kNow);
  const auto refusals = [&](const std::string& id) {
    base::DictValue snoozed = Snooze(id, 3600, kNow, storage());
    base::DictValue skipped = SkipNext(id, now, storage());
    EXPECT_EQ(snoozed.FindBool("success"), false) << id;
    EXPECT_EQ(skipped.FindBool("success"), false) << id;
    EXPECT_FALSE(snoozed.FindDouble("held_until_unix")) << id;
    const std::string* snooze_error = snoozed.FindString("error");
    const std::string* skip_error = skipped.FindString("error");
    EXPECT_TRUE(snooze_error && skip_error && *snooze_error == *skip_error)
        << id;
    if (std::optional<Script> stored = storage().Load(id)) {
      EXPECT_EQ(stored->trigger.held_until_unix, 0) << id;
    }
    EXPECT_TRUE(HoldsLogged(id, storage()).empty()) << id;
    return snooze_error ? *snooze_error : std::string();
  };
  EXPECT_EQ(refusals("gone"), "script not found");
  Script routine = VoiceRoutine("voice_1", "news");
  routine.trigger.type = automation::TriggerType::CRON;
  routine.trigger.expression = "0 9 * * *";
  ASSERT_TRUE(storage().Save(routine));
  EXPECT_EQ(refusals("voice_1"), "script not found");
  Script draft =
      Scheduled("agent_1", automation::TriggerType::CRON, "0 9 * * *", 0);
  draft.security.sites_unconfirmed = true;
  ASSERT_TRUE(storage().Save(draft));
  EXPECT_EQ(refusals("agent_1"),
            "Confirm this draft's sites in Agent mode first.");
  Script planted = Claiming("wf_planted");
  planted.trigger.type = automation::TriggerType::CRON;
  planted.trigger.expression = "0 9 * * *";
  Plant(planted);
  EXPECT_EQ(refusals("wf_planted"),
            "This workflow was changed outside MoltBrowser. Review it in "
            "Agent mode first.");
  ASSERT_TRUE(storage().Save(Workflow("wf_manual", "Manual")));
  EXPECT_EQ(refusals("wf_manual"),
            "Only a schedule that repeats can be snoozed or skipped");
  ASSERT_TRUE(storage().Save(Scheduled("wf_once", automation::TriggerType::AT,
                                       "2027-01-15T09:00:00Z", 0)));
  EXPECT_EQ(refusals("wf_once"),
            "Only a schedule that repeats can be snoozed or skipped");
  ASSERT_TRUE(storage().Save(
      Scheduled("wf_old", automation::TriggerType::INTERVAL, "3600", 0)));
  EXPECT_EQ(refusals("wf_old"),
            "Not started \xE2\x80\x94 click Save schedule to start it");
}

}  // namespace
}  // namespace automation_page
}  // namespace molt_ai
