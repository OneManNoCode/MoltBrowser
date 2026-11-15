// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The Agent studio's storage steps (molt_ai_agent_storage.h), on real
// files: each message finds its workflow where it is and answers as the
// studio always did; saves go where they belong, with the same refusals;
// the studio's and voice mode's changes to the profile's routines on one
// sequence never interleave; a workflow whose file was changed outside the
// browser (workflow_record.h) is shown for review, neither runs nor is
// scheduled or trusted, and is confirmed only as it was reviewed; an edit
// of a workflow gone since it was opened is refused, an id sent is always
// an edit, a new one never goes over another file, an interval schedule
// counts from when it is written, a schedule is answered as saved, and a
// confirmed change starts its schedule over.

#include "chrome/browser/ui/webui/molt_ai/molt_ai_agent_storage.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_security.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/automation_voice_steps.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/automation/workflow_record_test_util.h"
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

// A save of a workflow the page read (an edit).
SaveRequest Save(Script script) {
  SaveRequest request;
  request.script = std::move(script);
  return request;
}

// A save of a new workflow, its id made for it (blank, imported, or
// recorded here).
SaveRequest New(Script script, bool recorded_here = false) {
  SaveRequest request;
  request.script = std::move(script);
  request.recorded_here = recorded_here;
  request.new_workflow = true;
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
  // The shared storage's workflow record, as the browser has one
  // (workflow_record.h).
  void UseRecord() {
    record_.emplace(places_.shared_dir,
                    temp_.GetPath().Append(automation::kWorkflowRecordFileName),
                    /*may_seed=*/false);
  }
  // A file written into the shared storage by another program.
  void Plant(const Script& script) {
    ASSERT_TRUE(base::CreateDirectory(places_.shared_dir));
    ASSERT_TRUE(
        base::WriteFile(places_.shared_dir.AppendASCII(script.id + ".molt"),
                        script.ToJSONString()));
  }
  std::string Bytes(const std::string& id) {
    std::string bytes;
    EXPECT_TRUE(base::ReadFileToString(
        places_.shared_dir.AppendASCII(id + ".molt"), &bytes));
    return bytes;
  }

  base::ScopedTempDir temp_;
  std::optional<automation::ScopedWorkflowRecordForTesting> record_;
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
  Answer saved = SaveSharedWorkflow(places_, New(Workflow("wf_2", "New")),
                                    Holds::kNothing);
  EXPECT_EQ(*saved.out.FindString("id"), "wf_2");
  EXPECT_EQ(*saved.out.FindString("trust"), "casual");
  EXPECT_TRUE(Shared().Load("wf_2"));
  Answer recorded = SaveSharedWorkflow(
      places_, New(Workflow("wf_3", "Rec"), true), Holds::kNothing);
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
  EXPECT_EQ(*Error(SaveSharedWorkflow(off_the_record, New(Routine("wf_5", "x")),
                                      Holds::kNothing)),
            "Routines taught by voice can't be kept in this window.");
  EXPECT_FALSE(Shared().Load("wf_5"));
  // An edit of a workflow that isn't there: refused, not saved as new.
  EXPECT_EQ(*Error(SaveSharedWorkflow(places_, Save(Workflow("wf_6", "x")),
                                      Holds::kNothing)),
            "This workflow was deleted or changed since you opened it. Open "
            "it again.");
  EXPECT_FALSE(Shared().Load("wf_6"));
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
  ASSERT_TRUE(get.out.FindString("reviewToken"));
  // Not trusted before its sites are confirmed, nor run.
  WorkflowRequest trust = Request(Kind::kTrust, "wf_draft");
  trust.level = "approved";
  EXPECT_EQ(*Error(Ask(trust)), "Confirm this draft's sites first.");
  EXPECT_EQ(*Error(Ask(Request(Kind::kCheckRun, "wf_draft"))),
            automation::kDraftNeedsReviewMessage);
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_draft");
  confirm.review_token = *get.out.FindString("reviewToken");
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

// ---- Workflows changed outside the browser (workflow_record.h) ----------

// What a file planted in the shared storage might claim.
Script Claiming(const std::string& id) {
  Script script = Workflow(id, "Prices");
  script.security.trust = TrustLevel::TRUSTED;
  script.security.domain_whitelist = {"evil.example"};
  script.trigger.type = automation::TriggerType::INTERVAL;
  script.trigger.expression = "3600";
  return script;
}

bool IsReviewToken(const std::string* token) {
  if (!token || token->size() != 64) {
    return false;
  }
  for (char c : *token) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

TEST_F(AgentStudioStorageTest, AWorkflowChangedOutsideIsShownForReview) {
  UseRecord();
  Plant(Claiming("wf_1"));
  Answer get = Ask(Request(Kind::kGet, "wf_1"));
  const base::DictValue* security = get.out.FindDict("security");
  ASSERT_TRUE(security);
  EXPECT_EQ(security->FindBool("changed_outside"), true);
  EXPECT_EQ(security->FindBool("sites_unconfirmed"), true);
  EXPECT_EQ(*security->FindString("trust"), "casual");
  const base::ListValue* sites = get.out.FindList("draftSites");
  ASSERT_TRUE(sites);
  ASSERT_EQ(sites->size(), 1u);
  EXPECT_EQ((*sites)[0].GetString(), "news.example");
  EXPECT_TRUE(IsReviewToken(get.out.FindString("reviewToken")));
  // Its schedule is shown for review too.
  ASSERT_TRUE(get.out.FindDict("trigger"));
  EXPECT_EQ(*get.out.FindDict("trigger")->FindString("expression"), "3600");
}

TEST_F(AgentStudioStorageTest,
       AWorkflowChangedOutsideNeitherRunsNorSchedulesNorIsTrusted) {
  UseRecord();
  Plant(Claiming("wf_1"));
  const std::string bytes = Bytes("wf_1");
  Answer run = Ask(Request(Kind::kCheckRun, "wf_1"));
  EXPECT_EQ(run.out.FindBool("ok"), false);
  EXPECT_EQ(*Error(run), automation::kChangedOutsideMessage);
  EXPECT_FALSE(run.script);
  WorkflowRequest schedule = Request(Kind::kSchedule, "wf_1");
  schedule.trigger.type = automation::TriggerType::INTERVAL;
  schedule.trigger.expression = "60";
  Answer scheduled = Ask(schedule);
  EXPECT_EQ(scheduled.out.FindBool("ok"), false);
  EXPECT_EQ(*Error(scheduled), automation::kChangedOutsideMessage);
  EXPECT_FALSE(scheduled.trigger);
  WorkflowRequest trust = Request(Kind::kTrust, "wf_1");
  trust.level = "approved";
  EXPECT_EQ(*Error(Ask(trust)),
            "Review this workflow and confirm its sites first.");
  EXPECT_EQ(Bytes("wf_1"), bytes);
}

TEST_F(AgentStudioStorageTest, ConfirmingVouchesTheReviewedContentAtCasual) {
  UseRecord();
  Plant(Claiming("wf_1"));
  Answer get = Ask(Request(Kind::kGet, "wf_1"));
  ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken")));
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_1");
  confirm.sites = {"news.example"};
  confirm.review_token = *get.out.FindString("reviewToken");
  Answer confirmed = Ask(confirm);
  EXPECT_EQ(confirmed.out.FindBool("ok"), true);
  EXPECT_EQ(*confirmed.out.FindString("trust"), "casual");
  std::optional<Script> stored = Shared().Load("wf_1");
  ASSERT_TRUE(stored);
  EXPECT_FALSE(stored->security.changed_outside);
  EXPECT_FALSE(stored->security.sites_unconfirmed);
  EXPECT_EQ(stored->security.trust, TrustLevel::CASUAL);
  EXPECT_EQ(stored->security.domain_whitelist,
            std::vector<std::string>{"news.example"});
  // The schedule it was reviewed with.
  EXPECT_EQ(stored->trigger.expression, "3600");
  // Said so in the audit log.
  bool logged = false;
  for (const std::string& line : Shared().ReadAuditTail()) {
    logged |= line.find("wf_1 | outside_change_confirmed | news.example") !=
              std::string::npos;
  }
  EXPECT_TRUE(logged);
  // Then trusted with its own click, and run.
  WorkflowRequest trust = Request(Kind::kTrust, "wf_1");
  trust.level = "approved";
  EXPECT_EQ(Ask(trust).out.FindBool("ok"), true);
  EXPECT_EQ(Shared().Load("wf_1")->security.trust, TrustLevel::APPROVED);
  EXPECT_TRUE(Ask(Request(Kind::kCheckRun, "wf_1")).script);
}

TEST_F(AgentStudioStorageTest, ConfirmIsRefusedWhenTheFileChangedAfterReview) {
  UseRecord();
  Plant(Claiming("wf_1"));
  Answer get = Ask(Request(Kind::kGet, "wf_1"));
  ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken")));
  // Changed again before the click: the same site, another step.
  Script changed = Claiming("wf_1");
  Step another;
  another.type = StepType::NAVIGATE;
  another.target = "https://news.example/account/delete";
  changed.steps.push_back(std::move(another));
  Plant(changed);
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_1");
  confirm.sites = {"news.example"};
  confirm.review_token = *get.out.FindString("reviewToken");
  EXPECT_EQ(*Error(Ask(confirm)),
            "This workflow changed since you reviewed it. Review it again.");
  EXPECT_TRUE(Shared().Load("wf_1")->security.changed_outside);
}

TEST_F(AgentStudioStorageTest, ConfirmNeedsTheReviewToken) {
  UseRecord();
  Plant(Claiming("wf_1"));
  // An AI draft, saved by the browser, too.
  Script draft = Workflow("wf_draft", "Draft");
  draft.security.sites_unconfirmed = true;
  ASSERT_TRUE(Shared().Save(draft));
  for (const char* id : {"wf_1", "wf_draft"}) {
    WorkflowRequest confirm = Request(Kind::kConfirmSites, id);
    confirm.sites = {"news.example"};
    EXPECT_EQ(*Error(Ask(confirm)),
              "This workflow changed since you reviewed it. Review it again.")
        << id;
    confirm.review_token = std::string(64, 'a');
    EXPECT_EQ(*Error(Ask(confirm)),
              "This workflow changed since you reviewed it. Review it again.")
        << id;
    EXPECT_TRUE(Shared().Load(id)->security.sites_unconfirmed) << id;
    confirm.review_token =
        *Ask(Request(Kind::kGet, id)).out.FindString("reviewToken");
    EXPECT_EQ(Ask(confirm).out.FindBool("ok"), true) << id;
  }
}

TEST_F(AgentStudioStorageTest, EditingAWorkflowChangedOutsideKeepsItADraft) {
  UseRecord();
  Plant(Claiming("wf_1"));
  Script edit = Workflow("wf_1", "Prices, edited");
  edit.security.trust = TrustLevel::ADMIN;  // whatever the page sends
  Answer saved =
      SaveSharedWorkflow(places_, Save(std::move(edit)), Holds::kNothing);
  EXPECT_EQ(*saved.out.FindString("trust"), "casual");
  ASSERT_TRUE(saved.out.FindList("draftSites"));
  ASSERT_TRUE(IsReviewToken(saved.out.FindString("reviewToken")));
  std::optional<Script> stored = Shared().Load("wf_1");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->name, "Prices, edited");
  EXPECT_TRUE(stored->security.sites_unconfirmed);
  EXPECT_TRUE(stored->security.changed_outside);
  EXPECT_EQ(stored->security.trust, TrustLevel::CASUAL);
  // The save's token is what is stored now: it confirms it.
  EXPECT_EQ(*saved.out.FindString("reviewToken"),
            *Ask(Request(Kind::kGet, "wf_1")).out.FindString("reviewToken"));
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_1");
  confirm.sites = {"news.example"};
  confirm.review_token = *saved.out.FindString("reviewToken");
  EXPECT_EQ(Ask(confirm).out.FindBool("ok"), true);
}

TEST_F(AgentStudioStorageTest, ADuplicateOfAWorkflowChangedOutsideIsADraft) {
  UseRecord();
  Plant(Claiming("wf_1"));
  WorkflowRequest copy = Request(Kind::kDuplicate, "wf_1");
  copy.name = "Prices (copy)";
  Answer copied = Ask(copy);
  ASSERT_TRUE(copied.out.FindString("id"));
  std::optional<Script> stored = Shared().Load(*copied.out.FindString("id"));
  ASSERT_TRUE(stored);
  EXPECT_TRUE(stored->security.sites_unconfirmed);
  EXPECT_TRUE(stored->security.changed_outside);
  EXPECT_EQ(stored->security.trust, TrustLevel::CASUAL);
  EXPECT_TRUE(stored->security.domain_whitelist.empty());
  EXPECT_EQ(*Error(Ask(Request(Kind::kCheckRun, *copied.out.FindString("id")))),
            automation::kChangedOutsideMessage);
}

// The editor's save of a workflow it opened while it was changed outside,
// whose file is gone or unreadable by then: refused, never saved as a new
// workflow (what the page was shown, a draft with its schedule, would be
// saved as one the browser vouches for, neither a draft nor reviewed).
TEST_F(AgentStudioStorageTest, ASaveOfAWorkflowGoneSinceItWasOpenedIsRefused) {
  UseRecord();
  using Spoil = void (*)(const base::FilePath& file);
  const std::pair<const char*, Spoil> kSpoils[] = {
      {"deleted",
       [](const base::FilePath& file) { ASSERT_TRUE(base::DeleteFile(file)); }},
      {"unreadable",
       [](const base::FilePath& file) {
         ASSERT_TRUE(base::WriteFile(file, "not a workflow"));
       }},
      {"over the cap",
       [](const base::FilePath& file) {
         ASSERT_TRUE(base::WriteFile(
             file, std::string(automation::kMaxScriptFileBytes + 1, ' ')));
       }},
  };
  for (const auto& [what, spoil] : kSpoils) {
    Plant(Claiming("wf_1"));
    Answer get = Ask(Request(Kind::kGet, "wf_1"));
    ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken"))) << what;
    spoil(places_.shared_dir.AppendASCII("wf_1.molt"));
    // What the page sends back: the steps, name and schedule it was shown,
    // no security.
    Script shown = Workflow("wf_1", "Prices");
    shown.trigger = Claiming("wf_1").trigger;
    Answer saved =
        SaveSharedWorkflow(places_, Save(std::move(shown)), Holds::kNothing);
    ASSERT_TRUE(Error(saved)) << what;
    EXPECT_EQ(*Error(saved),
              "This workflow was deleted or changed since you opened it. "
              "Open it again.")
        << what;
    EXPECT_FALSE(saved.out.FindString("id")) << what;
    EXPECT_FALSE(Shared().Load("wf_1")) << what;
  }
}

// A new workflow's id was made for it: it is saved under a free one, never
// over a file that is there by then.
TEST_F(AgentStudioStorageTest, ANewWorkflowIsNeverSavedOverAnother) {
  UseRecord();
  Plant(Claiming("wf_1"));
  const std::string bytes = Bytes("wf_1");
  Answer saved = SaveSharedWorkflow(
      places_, New(Workflow("wf_1", "Mine"), /*recorded_here=*/true),
      Holds::kNothing);
  ASSERT_TRUE(saved.out.FindString("id"));
  const std::string id = *saved.out.FindString("id");
  EXPECT_NE(id, "wf_1");
  EXPECT_EQ(*saved.out.FindString("trust"), "approved");
  EXPECT_EQ(Bytes("wf_1"), bytes);
  std::optional<Script> mine = Shared().Load(id);
  ASSERT_TRUE(mine);
  EXPECT_EQ(mine->name, "Mine");
  EXPECT_FALSE(mine->security.changed_outside);
  EXPECT_EQ(mine->security.trust, TrustLevel::APPROVED);
  EXPECT_TRUE(Shared().Load("wf_1")->security.changed_outside);
}

// Only a save that names no workflow is a new one. An id the page sends is
// an edit even when it is one stopRecording made in this panel and not yet
// saved (it is predictable): a file put under it by another program, then
// opened and saved (or renamed) in the editor, stays a draft, and no new
// workflow is vouched for. A recording's own first save sends that id as
// its recordedId, and no id.
TEST_F(AgentStudioStorageTest, AnIdSentIsAnEditEvenOneARecordingWasGiven) {
  UseRecord();
  const std::set<std::string> minted = {"wf_1_rec"};
  for (const char* recorded : {"", "wf_1_rec"}) {
    const SaveKind edit = KindOfSave("wf_1_rec", recorded, minted);
    EXPECT_EQ(edit.id, "wf_1_rec") << recorded;
    EXPECT_FALSE(edit.new_workflow) << recorded;
    EXPECT_FALSE(edit.recorded_here) << recorded;
  }
  const SaveKind first = KindOfSave("", "wf_1_rec", minted);
  EXPECT_EQ(first.id, "wf_1_rec");
  EXPECT_TRUE(first.new_workflow);
  EXPECT_TRUE(first.recorded_here);
  for (const char* recorded : {"", "wf_2_rec"}) {
    const SaveKind blank = KindOfSave("", recorded, minted);
    EXPECT_EQ(blank.id, "") << recorded;
    EXPECT_TRUE(blank.new_workflow) << recorded;
    EXPECT_FALSE(blank.recorded_here) << recorded;
  }
  // Planted under the id, then opened and saved as the editor shows it.
  Plant(Claiming("wf_1_rec"));
  Script shown = Workflow("wf_1_rec", "Prices");
  shown.trigger = Claiming("wf_1_rec").trigger;
  const SaveKind kind = KindOfSave("wf_1_rec", "", minted);
  SaveRequest request = Save(std::move(shown));
  request.new_workflow = kind.new_workflow;
  request.recorded_here = kind.recorded_here;
  Answer saved =
      SaveSharedWorkflow(places_, std::move(request), Holds::kNothing);
  ASSERT_TRUE(saved.out.FindString("id"));
  EXPECT_EQ(*saved.out.FindString("id"), "wf_1_rec");
  EXPECT_EQ(*saved.out.FindString("trust"), "casual");
  std::optional<Script> stored = Shared().Load("wf_1_rec");
  ASSERT_TRUE(stored);
  EXPECT_TRUE(stored->security.changed_outside);
  EXPECT_TRUE(stored->security.sites_unconfirmed);
  EXPECT_EQ(Shared().ListIds(), std::vector<std::string>{"wf_1_rec"});
  EXPECT_EQ(*Error(Ask(Request(Kind::kCheckRun, "wf_1_rec"))),
            automation::kChangedOutsideMessage);
}

// saveSchedule answers with the trigger it saved, as the file holds it (an
// interval in seconds, its count started): the editor keeps it, so its
// next save keeps the schedule (the sheet's own "daily" or "30m" isn't a
// trigger: saved as it is, the schedule would be lost).
TEST_F(AgentStudioStorageTest, AScheduleIsAnsweredAsSaved) {
  UseRecord();
  ASSERT_TRUE(Shared().Save(Workflow("wf_1", "Prices")));
  const int64_t before = base::Time::Now().ToTimeT();
  WorkflowRequest schedule = Request(Kind::kSchedule, "wf_1");
  schedule.trigger.type = automation::TriggerType::INTERVAL;
  schedule.trigger.expression = "1800";
  Answer scheduled = Ask(schedule);
  const base::DictValue* trigger = scheduled.out.FindDict("trigger");
  ASSERT_TRUE(trigger);
  EXPECT_EQ(*trigger->FindString("type"), "interval");
  EXPECT_EQ(*trigger->FindString("expression"), "1800");
  ASSERT_TRUE(trigger->FindDouble("last_fired_unix"));
  EXPECT_GE(*trigger->FindDouble("last_fired_unix"),
            static_cast<double>(before));
  // The editor's next save, with that trigger: the schedule is kept.
  base::DictValue page = Workflow("wf_1", "Prices, renamed").ToJSON();
  page.Set("trigger", trigger->Clone());
  std::optional<Script> edit = Script::FromJSON(page);
  ASSERT_TRUE(edit);
  Answer edited =
      SaveSharedWorkflow(places_, Save(std::move(*edit)), Holds::kNothing);
  EXPECT_FALSE(Error(edited));
  EXPECT_TRUE(edited.out.FindString("id"));
  std::optional<Script> stored = Shared().Load("wf_1");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->name, "Prices, renamed");
  EXPECT_EQ(stored->trigger.type, automation::TriggerType::INTERVAL);
  EXPECT_EQ(stored->trigger.expression, "1800");
  EXPECT_GE(stored->trigger.last_fired_unix, before);
  // A cron one, as the handler makes it of "daily at 09:00".
  schedule.trigger.type = automation::TriggerType::CRON;
  schedule.trigger.expression = "00 09 * * *";
  scheduled = Ask(schedule);
  trigger = scheduled.out.FindDict("trigger");
  ASSERT_TRUE(trigger);
  EXPECT_EQ(*trigger->FindString("type"), "cron");
  EXPECT_EQ(*trigger->FindString("expression"), "00 09 * * *");
  EXPECT_FALSE(trigger->FindDouble("last_fired_unix"));
}

// A workflow this browser fired, changed outside (its schedule too), then
// reviewed and confirmed: its schedule starts over, as one saved afresh.
// Counted from the fire recorded before the change, a daily schedule's
// next time after it would be past, so it would run at once.
TEST_F(AgentStudioStorageTest,
       ConfirmingAChangedWorkflowStartsItsScheduleOver) {
  UseRecord();
  const int64_t two_days_ago = base::Time::Now().ToTimeT() - 2 * 86400;
  for (const bool interval : {false, true}) {
    const std::string id = interval ? "wf_interval" : "wf_cron";
    Script weekly = Workflow(id, "Prices");
    weekly.trigger.type = automation::TriggerType::CRON;
    weekly.trigger.expression = "0 9 * * 1";
    ASSERT_TRUE(Shared().Save(weekly));
    ASSERT_TRUE(Shared().RecordFire(id, two_days_ago));
    // Changed outside: every day, or every hour.
    Script changed = Workflow(id, "Prices");
    changed.trigger.type = interval ? automation::TriggerType::INTERVAL
                                    : automation::TriggerType::CRON;
    changed.trigger.expression = interval ? "3600" : "0 8 * * *";
    Plant(changed);
    Answer get = Ask(Request(Kind::kGet, id));
    ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken"))) << id;
    const int64_t before = base::Time::Now().ToTimeT();
    WorkflowRequest confirm = Request(Kind::kConfirmSites, id);
    confirm.sites = {"news.example"};
    confirm.review_token = *get.out.FindString("reviewToken");
    Answer confirmed = Ask(confirm);
    ASSERT_EQ(confirmed.out.FindBool("ok"), true) << id;
    std::optional<Script> stored = Shared().Load(id);
    ASSERT_TRUE(stored) << id;
    EXPECT_FALSE(stored->security.changed_outside) << id;
    if (interval) {
      EXPECT_GE(stored->trigger.last_fired_unix, before) << id;
    } else {
      EXPECT_EQ(stored->trigger.last_fired_unix, 0) << id;
    }
    // Answered with the trigger as confirmed, which the editor keeps for
    // its next save: the one it opened carries the fire from before the
    // change, which written back would run a cron schedule at once.
    const base::DictValue* trigger = confirmed.out.FindDict("trigger");
    ASSERT_TRUE(trigger) << id;
    const base::DictValue* opened = get.out.FindDict("trigger");
    ASSERT_TRUE(opened) << id;
    EXPECT_EQ(opened->FindDouble("last_fired_unix"),
              std::optional<double>(two_days_ago))
        << id;
    base::DictValue page = changed.ToJSON();
    page.Set("trigger", trigger->Clone());
    std::optional<Script> edit = Script::FromJSON(page);
    ASSERT_TRUE(edit) << id;
    edit->name = "Prices, renamed";
    EXPECT_FALSE(Error(
        SaveSharedWorkflow(places_, Save(std::move(*edit)), Holds::kNothing)))
        << id;
    stored = Shared().Load(id);
    ASSERT_TRUE(stored) << id;
    EXPECT_EQ(stored->name, "Prices, renamed") << id;
    EXPECT_FALSE(stored->security.sites_unconfirmed) << id;
    if (interval) {
      EXPECT_GE(stored->trigger.last_fired_unix, before) << id;
    } else {
      EXPECT_EQ(stored->trigger.last_fired_unix, 0) << id;
    }
  }
}

// An interval schedule the studio writes with no last fire counts from
// then (the scheduler counts one with none from each listing, so it would
// never fire): saved afresh, or confirmed after a review.
TEST_F(AgentStudioStorageTest, AnIntervalScheduleCountsFromWhenItIsWritten) {
  UseRecord();
  ASSERT_TRUE(Shared().Save(Workflow("wf_1", "Prices")));
  const int64_t before = base::Time::Now().ToTimeT();
  WorkflowRequest schedule = Request(Kind::kSchedule, "wf_1");
  schedule.trigger.type = automation::TriggerType::INTERVAL;
  schedule.trigger.expression = "3600";
  Answer scheduled = Ask(schedule);
  ASSERT_TRUE(scheduled.trigger);
  EXPECT_GE(scheduled.trigger->last_fired_unix, before);
  std::optional<Script> stored = Shared().Load("wf_1");
  ASSERT_TRUE(stored);
  EXPECT_GE(stored->trigger.last_fired_unix, before);
  EXPECT_LE(stored->trigger.last_fired_unix, base::Time::Now().ToTimeT());
  // A cron schedule saved afresh has none: no catch-up run.
  schedule.trigger.type = automation::TriggerType::CRON;
  schedule.trigger.expression = "0 9 * * *";
  ASSERT_TRUE(Ask(schedule).trigger);
  EXPECT_EQ(Shared().Load("wf_1")->trigger.last_fired_unix, 0);
  // A file changed outside (its own last fire not believed), confirmed.
  Plant(Claiming("wf_2"));
  Answer get = Ask(Request(Kind::kGet, "wf_2"));
  ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken")));
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_2");
  confirm.sites = {"news.example"};
  confirm.review_token = *get.out.FindString("reviewToken");
  ASSERT_EQ(Ask(confirm).out.FindBool("ok"), true);
  stored = Shared().Load("wf_2");
  ASSERT_TRUE(stored);
  EXPECT_FALSE(stored->security.changed_outside);
  EXPECT_GE(stored->trigger.last_fired_unix, before);
}

// The review of a file changed outside shows each step's type, target and
// value, not the file's fallback selectors, locators, text anchor, picture
// or description, which the runner would act on when the target shown
// misses, nor which workflow a RUN_SCRIPT step runs, nor a variable's
// default value: the page gets none of them, and the workflow confirmed
// with the token of what it showed has none.
TEST_F(AgentStudioStorageTest, AConfirmedChangeRunsOnlyOnTheTargetsShown) {
  UseRecord();
  Script planted = Claiming("wf_1");
  Step click;
  click.type = StepType::CLICK;
  click.target = "#search_btn";
  click.selector_fallbacks = {"#account button.danger"};
  click.description = "Click \xE2\x80\x9CSearch\xE2\x80\x9D";
  base::DictValue extra;
  extra.Set(automation::kStepHostKey, "news.example");
  extra.Set(automation::kStepTextAnchorKey, "Delete account");
  extra.Set(automation::kStepRefShotKey, "data:image/png;base64,AAAA");
  click.extra = base::Value(std::move(extra));
  planted.steps.push_back(std::move(click));
  // Another workflow, named by a variable whose default the review
  // wouldn't show.
  Step child;
  child.type = StepType::RUN_SCRIPT;
  child.target = "{{next}}";
  planted.steps.push_back(std::move(child));
  planted.default_variables["next"] = "pay_rent";
  Plant(planted);
  Answer get = Ask(Request(Kind::kGet, "wf_1"));
  ASSERT_TRUE(IsReviewToken(get.out.FindString("reviewToken")));
  std::optional<Script> shown = Script::FromJSON(get.out);
  ASSERT_TRUE(shown);
  ASSERT_EQ(shown->steps.size(), 2u);
  EXPECT_EQ(shown->steps[0].type, StepType::NAVIGATE);
  EXPECT_EQ(shown->steps[1].type, StepType::CLICK);
  ASSERT_TRUE(get.out.FindDict("variables"));
  ASSERT_TRUE(get.out.FindDict("variables")->FindString("next"));
  EXPECT_EQ(*get.out.FindDict("variables")->FindString("next"), "");
  EXPECT_EQ(shown->steps[1].target, "#search_btn");
  EXPECT_TRUE(shown->steps[1].selector_fallbacks.empty());
  EXPECT_TRUE(shown->steps[1].description.empty());
  ASSERT_TRUE(shown->steps[1].extra.is_dict());
  EXPECT_FALSE(
      shown->steps[1].extra.GetDict().Find(automation::kStepTextAnchorKey));
  EXPECT_FALSE(
      shown->steps[1].extra.GetDict().Find(automation::kStepRefShotKey));
  WorkflowRequest confirm = Request(Kind::kConfirmSites, "wf_1");
  confirm.sites = {"news.example"};
  confirm.review_token = *get.out.FindString("reviewToken");
  ASSERT_EQ(Ask(confirm).out.FindBool("ok"), true);
  // What is stored, and runs.
  std::optional<Script> stored = Script::FromJSONString(Bytes("wf_1"));
  ASSERT_TRUE(stored);
  ASSERT_EQ(stored->steps.size(), 2u);
  EXPECT_EQ(stored->steps[1].target, "#search_btn");
  EXPECT_TRUE(stored->steps[1].selector_fallbacks.empty());
  EXPECT_FALSE(stored->steps[1].locators);
  EXPECT_TRUE(stored->steps[1].description.empty());
  EXPECT_EQ(stored->default_variables.at("next"), "");
  EXPECT_EQ(Bytes("wf_1").find("Delete account"), std::string::npos);
  EXPECT_EQ(Bytes("wf_1").find("pay_rent"), std::string::npos);
  EXPECT_EQ(Bytes("wf_1").find("run_script"), std::string::npos);
  EXPECT_FALSE(Shared().Load("wf_1")->security.changed_outside);
}

// A new workflow (an import, a blank one, a recording) has never fired
// here: its schedule starts over, whatever last fire the page sent (an
// import's, from its file: long past, it would run at once; in 2100, it
// would never run). An interval counts from the save.
TEST_F(AgentStudioStorageTest, ANewWorkflowStartsItsScheduleOver) {
  UseRecord();
  Script weekly = Workflow("wf_weekly", "Weekly");
  weekly.trigger.type = automation::TriggerType::CRON;
  weekly.trigger.expression = "0 9 * * 1";
  weekly.trigger.last_fired_unix = 1;
  Script hourly = Workflow("wf_hourly", "Hourly");
  hourly.trigger.type = automation::TriggerType::INTERVAL;
  hourly.trigger.expression = "3600";
  hourly.trigger.last_fired_unix = 4'102'444'800;
  const int64_t before = base::Time::Now().ToTimeT();
  Answer cron =
      SaveSharedWorkflow(places_, New(std::move(weekly)), Holds::kNothing);
  Answer interval =
      SaveSharedWorkflow(places_, New(std::move(hourly)), Holds::kNothing);
  ASSERT_TRUE(cron.out.FindString("id"));
  ASSERT_TRUE(interval.out.FindString("id"));
  std::optional<Script> stored = Shared().Load(*cron.out.FindString("id"));
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->trigger.expression, "0 9 * * 1");
  EXPECT_EQ(stored->trigger.last_fired_unix, 0);
  stored = Shared().Load(*interval.out.FindString("id"));
  ASSERT_TRUE(stored);
  EXPECT_GE(stored->trigger.last_fired_unix, before);
  EXPECT_LE(stored->trigger.last_fired_unix, base::Time::Now().ToTimeT());
}

// molt://ai-automation's Snooze (a last fire still to come), then a save
// from an editor opened before it (the last fire it was shown, earlier):
// the snooze holds, so the workflow doesn't run during it.
TEST_F(AgentStudioStorageTest, AnEditorOpenedBeforeASnoozeKeepsIt) {
  UseRecord();
  Script interval = Workflow("wf_1", "Prices");
  interval.trigger.type = automation::TriggerType::INTERVAL;
  interval.trigger.expression = "900";
  ASSERT_TRUE(Shared().Save(interval));
  const int64_t now = base::Time::Now().ToTimeT();
  ASSERT_TRUE(Shared().RecordFire("wf_1", now - 600));
  std::optional<Script> editor = Shared().Load("wf_1");
  ASSERT_TRUE(editor);
  ASSERT_EQ(editor->trigger.last_fired_unix, now - 600);
  ASSERT_TRUE(Shared().Update("wf_1", [now](Script& script) {
    script.trigger.last_fired_unix = now + 3600;
    return true;
  }));
  editor->steps[0].description = "Open the news";
  Answer saved =
      SaveSharedWorkflow(places_, Save(std::move(*editor)), Holds::kNothing);
  EXPECT_EQ(*saved.out.FindString("id"), "wf_1");
  std::optional<Script> stored = Shared().Load("wf_1");
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->steps[0].description, "Open the news");
  EXPECT_EQ(stored->trigger.last_fired_unix, now + 3600);
}

}  // namespace
}  // namespace agent_studio
}  // namespace molt_ai
