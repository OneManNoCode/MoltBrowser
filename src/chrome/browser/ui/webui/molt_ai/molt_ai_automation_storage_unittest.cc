// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt://ai-automation's storage steps (molt_ai_automation_storage.h) on a
// temporary directory: what each message reads and changes, as it did on
// the UI thread, now as one step on the storage's sequence.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_automation_storage.h"

#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation_page {
namespace {

using automation::AutomationStorage;
using automation::Script;
using automation::TrustLevel;

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
    storage_.emplace(temp_.GetPath().AppendASCII("automations"));
  }

  AutomationStorage& storage() { return *storage_; }

  base::ScopedTempDir temp_;
  std::optional<AutomationStorage> storage_;
};

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
  base::DictValue over = SaveWorkflow(Workflow("voice_1", "mine"), storage());
  EXPECT_EQ(over.FindBool("success"), false);
  EXPECT_EQ(*over.FindString("error"), kVoiceRoutinesElsewhere);
  base::DictValue as_one =
      SaveWorkflow(VoiceRoutine("wf_9", "news"), storage());
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
  EXPECT_EQ(SaveWorkflow(std::move(first), storage()).FindBool("success"),
            true);
  EXPECT_EQ(storage().Load("wf_1")->security.trust, TrustLevel::CASUAL);
  base::DictValue promoted =
      PromoteTrust("wf_1", TrustLevel::APPROVED, storage());
  EXPECT_EQ(promoted.FindBool("success"), true);
  EXPECT_EQ(*promoted.FindString("trust"), "approved");
  Script edit = Workflow("wf_1", "Prices, edited");
  edit.security.trust = TrustLevel::ADMIN;
  EXPECT_EQ(SaveWorkflow(std::move(edit), storage()).FindBool("success"), true);
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

}  // namespace
}  // namespace automation_page
}  // namespace molt_ai
