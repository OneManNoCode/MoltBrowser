// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Agent studio's storage steps (molt_ai_agent_storage.h), on real
// files: each message finds its workflow where it is and answers as the
// studio always did; saves go where they belong, with the same refusals;
// the studio's and voice mode's changes to the profile's routines on one
// sequence never interleave.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_storage.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_routine_store.h"
#include "chrome/browser/molt_ai/voice/voice_routines.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace agent_studio {
namespace {

using automation::AutomationStorage;
using automation::Script;
using automation::Step;
using automation::StepType;
using automation::TrustLevel;
using Kind = WorkflowRequest::Kind;

constexpr int64_t kNow = 1'800'000'000;

std::vector<voice::RoutineStep> MyNews() {
  voice::ParseContext context;
  std::optional<voice::RoutineStep> open = voice::RoutineStepFor(
      voice::ParseVoiceCommand("open YouTube", context), "", 0, {}, false);
  EXPECT_TRUE(open);
  open->url = "https://www.youtube.com/";
  return {*open};
}

// A routine taught by voice, as voice mode saves it.
Script Routine(const std::string& id, const std::string& name) {
  voice::BuiltRoutine built = voice::BuildRoutine(id, name, MyNews(), kNow);
  EXPECT_TRUE(built.script);
  return built.script ? std::move(*built.script) : Script();
}

// An ordinary workflow on news.example.
Script Workflow(const std::string& id, const std::string& name) {
  Script script;
  script.id = id;
  script.name = name;
  script.created_at_unix = kNow;
  Step step;
  step.type = StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

WorkflowRequest Request(Kind kind, const std::string& id) {
  WorkflowRequest request;
  request.kind = kind;
  request.id = id;
  return request;
}

SaveRequest Save(Script script, bool recorded_here = false) {
  SaveRequest request;
  request.script = std::move(script);
  request.recorded_here = recorded_here;
  return request;
}

const std::string* Error(const Answer& answer) {
  return answer.out.FindString("error");
}

class AgentStudioStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    places_.routines_dir =
        voice::VoiceRoutinesDir(temp_.GetPath().AppendASCII("Default"));
    places_.shared_root = temp_.GetPath().AppendASCII("home");
    places_.shared_dir =
        AutomationStorage::SharedDirectory(places_.shared_root);
  }

  AutomationStorage& Shared() {
    if (!shared_) {
      shared_.emplace(places_.shared_dir);
    }
    return *shared_;
  }
  AutomationStorage& Routines() {
    if (!routines_) {
      routines_.emplace(places_.routines_dir);
    }
    return *routines_;
  }
  // The message answered as the studio asks: the profile's routines first.
  Answer Ask(const WorkflowRequest& request) {
    std::optional<Answer> answer = AnswerInRoutines(places_, request);
    return answer ? std::move(*answer)
                  : AnswerInSharedStorage(places_, request);
  }

  base::ScopedTempDir temp_;
  StudioPlaces places_;
  std::optional<AutomationStorage> shared_;
  std::optional<AutomationStorage> routines_;
};

TEST_F(AgentStudioStorageTest, ListsEachStorageOnItsOwn) {
  ASSERT_TRUE(Shared().Save(Workflow("wf_1", "Prices")));
  ASSERT_TRUE(Shared().Save(Routine("voice_old", "my news")));
  ASSERT_TRUE(Routines().Save(Routine("voice_1", "mail")));
  RoutinesListing listing = ListRoutines(places_);
  ASSERT_EQ(listing.routines.size(), 1u);
  EXPECT_EQ(listing.routines[0].id, "voice_1");
  ASSERT_EQ(listing.unclaimed.size(), 1u);
  EXPECT_EQ(listing.unclaimed[0].id, "voice_old");
  std::vector<Script> workflows = ListSharedWorkflows(places_);
  ASSERT_EQ(workflows.size(), 1u);
  EXPECT_EQ(workflows[0].id, "wf_1");
  // Off the record: no routines; without a shared storage: no workflows.
  StudioPlaces off_the_record = places_;
  off_the_record.routines_dir = base::FilePath();
  EXPECT_TRUE(ListRoutines(off_the_record).routines.empty());
  StudioPlaces no_shared = places_;
  no_shared.shared_dir = base::FilePath();
  EXPECT_TRUE(ListSharedWorkflows(no_shared).empty());
}

TEST_F(AgentStudioStorageTest, FindsAWorkflowWhereItIs) {
  Script workflow = Workflow("wf_1", "Prices");
  workflow.default_variables["city"] = "Paris";
  ASSERT_TRUE(Shared().Save(workflow));
  ASSERT_TRUE(Shared().Save(Routine("voice_old", "my news")));
  ASSERT_TRUE(Routines().Save(Routine("voice_1", "mail")));

  // A routine: answered on the profile's sequence.
  std::optional<Answer> routine =
      AnswerInRoutines(places_, Request(Kind::kGet, "voice_1"));
  ASSERT_TRUE(routine);
  EXPECT_EQ(*routine->out.FindString("name"), "mail");
  // Not a routine: passed on to the shared storage's.
  EXPECT_FALSE(AnswerInRoutines(places_, Request(Kind::kGet, "wf_1")));
  Answer shared = AnswerInSharedStorage(places_, Request(Kind::kGet, "wf_1"));
  EXPECT_EQ(*shared.out.FindString("name"), "Prices");
  ASSERT_TRUE(shared.out.FindDict("variables"));
  EXPECT_EQ(*shared.out.FindDict("variables")->FindString("city"), "Paris");
  // A routine from before profiles isn't found: it is kept here first.
  EXPECT_TRUE(Ask(Request(Kind::kGet, "voice_old")).out.empty());
  EXPECT_TRUE(Ask(Request(Kind::kGet, "nothing")).out.empty());
  Answer runs = Ask(Request(Kind::kRuns, "nothing"));
  EXPECT_EQ(*runs.out.FindString("id"), "nothing");
  ASSERT_TRUE(runs.out.FindList("runs"));
  EXPECT_TRUE(runs.out.FindList("runs")->empty());
}

TEST_F(AgentStudioStorageTest, SavesWhereItBelongsWithTheSameRefusals) {
  ASSERT_TRUE(Shared().Save(Workflow("wf_1", "Prices")));
  ASSERT_TRUE(Shared().Save(Routine("voice_old", "my news")));
  ASSERT_TRUE(Routines().Save(Routine("voice_1", "mail")));

  // A new workflow: the shared storage, Casual; recorded here: Approved.
  Answer saved = SaveSharedWorkflow(places_, Save(Workflow("wf_2", "New")),
                                    Holds::kNothing);
  EXPECT_EQ(*saved.out.FindString("trust"), "casual");
  EXPECT_TRUE(Shared().Load("wf_2"));
  Answer recorded = SaveSharedWorkflow(
      places_, Save(Workflow("wf_3", "Rec"), true), Holds::kNothing);
  EXPECT_EQ(*recorded.out.FindString("trust"), "approved");
  // An edit keeps the stored policy, whatever the page sent.
  Script edit = Workflow("wf_2", "Renamed");
  edit.security.trust = TrustLevel::ADMIN;
  edit.security.domain_whitelist = {"evil.example"};
  Answer edited =
      SaveSharedWorkflow(places_, Save(std::move(edit)), Holds::kNothing);
  EXPECT_EQ(*edited.out.FindString("name"), "Renamed");
  EXPECT_EQ(*edited.out.FindString("trust"), "casual");
  EXPECT_EQ(Shared().Load("wf_2")->security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(Shared().Load("wf_2")->security.domain_whitelist.empty());

  // A routine: the profile's, its name made its own.
  Answer routine =
      SaveRoutine(places_, Save(Routine("wf_4", "Mail")), Holds::kNothing);
  EXPECT_EQ(*routine.out.FindString("name"), "Mail (2)");
  EXPECT_TRUE(routine.routines_changed);
  EXPECT_TRUE(Routines().Load("wf_4"));
  EXPECT_FALSE(Shared().Load("wf_4"));

  // The refusals, word for word.
  EXPECT_EQ(*Error(SaveRoutine(places_, Save(Routine("voice_old", "x")),
                               Holds::kRoutine)),
            "This routine was made before routines belonged to a profile. "
            "Keep it here first.");
  EXPECT_EQ(*Error(SaveRoutine(places_, Save(Routine("wf_1", "x")),
                               Holds::kOrdinary)),
            "Steps taught by voice can't be added to this workflow.");
  EXPECT_EQ(*Error(SaveSharedWorkflow(places_, Save(Workflow("voice_1", "x")),
                                      Holds::kRoutine)),
            "A routine taught by voice keeps its voice steps.");
  EXPECT_EQ(*Error(SaveSharedWorkflow(places_, Save(Workflow("voice_old", "x")),
                                      Holds::kNothing)),
            "This routine was made before routines belonged to a profile. "
            "Keep it here first.");
  StudioPlaces off_the_record = places_;
  off_the_record.routines_dir = base::FilePath();
  EXPECT_EQ(*Error(SaveSharedWorkflow(
                off_the_record, Save(Routine("wf_5", "x")), Holds::kNothing)),
            "Routines taught by voice can't be kept in this window.");
  EXPECT_FALSE(Shared().Load("wf_5"));
}

TEST_F(AgentStudioStorageTest, ChangesAWorkflowWhereItIs) {
  ASSERT_TRUE(Shared().Save(Workflow("wf_1", "Prices")));
  ASSERT_TRUE(Routines().Save(Routine("voice_1", "mail")));

  // Trust: between Casual and Approved only, on a click (the caller's).
  WorkflowRequest trust = Request(Kind::kTrust, "wf_1");
  trust.level = "approved";
  Answer trusted = Ask(trust);
  EXPECT_EQ(trusted.out.FindBool("ok"), true);
  EXPECT_EQ(Shared().Load("wf_1")->security.trust, TrustLevel::APPROVED);
  trust.level = "trusted";
  EXPECT_EQ(*Error(Ask(trust)),
            "Manage this workflow's trust in molt://ai-automation");
  trust.id = "nothing";
  EXPECT_EQ(*Error(Ask(trust)), "Workflow not found");

  // A schedule: saved on an ordinary workflow, never on a routine.
  WorkflowRequest schedule = Request(Kind::kSchedule, "wf_1");
  schedule.trigger.type = automation::TriggerType::INTERVAL;
  schedule.trigger.expression = "3600";
  Answer scheduled = Ask(schedule);
  EXPECT_EQ(scheduled.out.FindBool("ok"), true);
  ASSERT_TRUE(scheduled.trigger);
  EXPECT_TRUE(Shared().Load("wf_1")->enabled);
  EXPECT_EQ(Shared().Load("wf_1")->trigger.expression, "3600");
  schedule.id = "voice_1";
  EXPECT_EQ(*Error(Ask(schedule)), automation::kVoiceRoutineMessage);

  // A run: the workflow to run, never a routine.
  Answer run = Ask(Request(Kind::kCheckRun, "wf_1"));
  ASSERT_TRUE(run.script);
  EXPECT_EQ(run.script->id, "wf_1");
  EXPECT_EQ(*Error(Ask(Request(Kind::kCheckRun, "voice_1"))),
            automation::kVoiceRoutineMessage);
  EXPECT_EQ(Ask(Request(Kind::kCheckRun, "nothing")).out.FindBool("ok"), false);

  // A copy, in the same storage, under a new id; a routine's renamed.
  WorkflowRequest copy = Request(Kind::kDuplicate, "voice_1");
  copy.name = "mail";
  Answer copied = Ask(copy);
  ASSERT_TRUE(copied.out.FindString("id"));
  EXPECT_EQ(*copied.out.FindString("name"), "mail (2)");
  EXPECT_TRUE(copied.routines_changed);
  EXPECT_TRUE(Routines().Load(*copied.out.FindString("id")));

  // Deleted where it is.
  Answer deleted = Ask(Request(Kind::kDelete, "voice_1"));
  EXPECT_EQ(deleted.out.FindBool("ok"), true);
  EXPECT_TRUE(deleted.routines_changed);
  EXPECT_FALSE(Routines().Load("voice_1"));
  EXPECT_EQ(Ask(Request(Kind::kDelete, "voice_1")).out.FindBool("ok"), false);
}

TEST_F(AgentStudioStorageTest, ConfirmsADraftsSitesExactly) {
  Script draft = Workflow("wf_draft", "Draft");
  draft.security.sites_unconfirmed = true;
  ASSERT_TRUE(Shared().Save(draft));
  Answer get = Ask(Request(Kind::kGet, "wf_draft"));
  ASSERT_TRUE(get.out.FindList("draftSites"));
  // Not trusted before its sites are confirmed, nor run.
  WorkflowRequest trust = Request(Kind::kTrust, "wf_draft");
  trust.level = "approved";
  EXPECT_EQ(*Error(Ask(trust)), "Confirm this draft's sites first.");
  EXPECT_EQ(*Error(Ask(Request(Kind::kCheckRun, "wf_draft"))),
            automation::kDraftNeedsReviewMessage);
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_draft");
  confirm.sites = {"other.example"};
  EXPECT_EQ(*Error(Ask(confirm)),
            "The draft's steps changed. Save them, then confirm again.");
  confirm.sites = {"news.example"};
  Answer confirmed = Ask(confirm);
  EXPECT_EQ(confirmed.out.FindBool("ok"), true);
  EXPECT_FALSE(Shared().Load("wf_draft")->security.sites_unconfirmed);
}

TEST_F(AgentStudioStorageTest, ARecordingsIdIsUnused) {
  ASSERT_TRUE(Shared().Save(Workflow("wf_9_rec", "Taken")));
  EXPECT_EQ(UnusedWorkflowId(places_, "wf_9_rec", {}), "wf_9_rec_");
  EXPECT_EQ(UnusedWorkflowId(places_, "wf_9_rec", {"wf_9_rec_"}), "wf_9_rec__");
  EXPECT_EQ(UnusedWorkflowId(places_, "wf_8_rec", {}), "wf_8_rec");
}

// The studio's save of a routine and voice mode's, on the profile's one
// sequence, as they are posted: each name answers to exactly one routine.
TEST_F(AgentStudioStorageTest, TheStudioAndVoiceModeShareOneSequence) {
  base::test::TaskEnvironment task_environment;
  scoped_refptr<base::SequencedTaskRunner> sequence =
      base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
  constexpr int kEach = 8;
  for (int i = 0; i < kEach; ++i) {
    sequence->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](StudioPlaces places, int i) {
              SaveRoutine(
                  places,
                  Save(Routine("wf_" + base::NumberToString(i), "my news")),
                  Holds::kNothing);
            },
            places_, i));
    sequence->PostTask(FROM_HERE,
                       base::BindOnce(
                           [](StudioPlaces places, int i) {
                             voice::VoiceRoutineStore store(places.routines_dir,
                                                            places.shared_root);
                             store.Save("voice_" + base::NumberToString(i),
                                        "my news", MyNews(), /*replace=*/false,
                                        kNow + i);
                           },
                           places_, i));
  }
  base::RunLoop run_loop;
  sequence->PostTask(FROM_HERE, run_loop.QuitClosure());
  run_loop.Run();
  voice::VoiceRoutineStore store(places_.routines_dir, places_.shared_root);
  std::vector<std::string> names;
  for (const voice::RoutineSummary& routine : store.Summaries()) {
    names.push_back(routine.name);
  }
  ASSERT_EQ(names.size(), static_cast<size_t>(kEach));
  for (const std::string& name : names) {
    EXPECT_EQ(voice::MatchRoutineName(names, name).count, 1) << name;
  }
}

}  // namespace
}  // namespace agent_studio
}  // namespace molt_ai
