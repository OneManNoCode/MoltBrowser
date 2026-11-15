// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationScheduler (automation_scheduler.h) over a real shared workflow
// directory with a workflow record (workflow_record.h), on mock time: it
// fires a catch-up of a workflow the browser saved, once, and its next
// fire an interval later; never one whose file was changed outside the
// browser, whether that is found when it is listed or when its timer's
// reload comes, nor one its timer's reload finds fired since; a forged
// earlier last fire forces no catch-up run (fired here or not), a later
// one still to come (another browser's Snooze, or forged) forces none
// either and holds the schedule back at most an interval, and a schedule
// saved afresh starts over.

#include "chrome/browser/molt_ai/automation/automation_scheduler.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_script.h"
#include "chrome/browser/molt_ai/automation/automation_storage.h"
#include "chrome/browser/molt_ai/automation/workflow_record.h"
#include "chrome/browser/molt_ai/automation/workflow_record_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace automation {
namespace {

int64_t Unix(base::Time time) {
  return static_cast<int64_t>(time.InSecondsFSinceUnixEpoch());
}

// A workflow that runs every |seconds|, last fired at |last_fired|.
Script Interval(const std::string& id, int seconds, int64_t last_fired) {
  Script script;
  script.id = id;
  script.name = id;
  script.trigger.type = TriggerType::INTERVAL;
  script.trigger.expression = base::NumberToString(seconds);
  script.trigger.last_fired_unix = last_fired;
  Step step;
  step.type = StepType::NAVIGATE;
  step.target = "https://news.example/";
  script.steps.push_back(std::move(step));
  return script;
}

// The mock clock's wall time when each test starts. Mock time starts at
// the Unix epoch plus the host's uptime, so times hours back would be
// negative on a host booted less than that ago (a fresh CI runner).
constexpr int64_t kStart = 1'800'000'000;

class AutomationSchedulerTest : public testing::Test {
 protected:
  void SetUp() override {
    task_environment_.AdvanceClock(
        base::Time::FromSecondsSinceUnixEpoch(kStart) - base::Time::Now());
    ASSERT_EQ(Unix(base::Time::Now()), kStart);
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    dir_ = temp_.GetPath().AppendASCII("automations");
    record_.emplace(dir_, temp_.GetPath().Append(kWorkflowRecordFileName),
                    /*may_seed=*/false);
    storage_.emplace(dir_);
    scheduler_.SetStorageDir(dir_);
    scheduler_.SetFireCallback(base::BindRepeating(
        &AutomationSchedulerTest::OnFired, base::Unretained(this)));
  }
  void TearDown() override {
    scheduler_.Stop();
    task_environment_.RunUntilIdle();
  }

  // As the scheduler's service does: the fire kept, then the run.
  void OnFired(const Script& script, base::Time fired_at) {
    fired_.push_back(script.id);
    storage_->RecordFire(script.id, Unix(fired_at));
  }
  // A listing now, and what it fires.
  void Tick() {
    scheduler_.TickForTesting(base::Time::Now());
    task_environment_.RunUntilIdle();
  }
  // Another program edits the workflow's steps.
  void ChangeOutside(const std::string& id) {
    const base::FilePath file = dir_.AppendASCII(id + ".molt");
    std::string bytes;
    ASSERT_TRUE(base::ReadFileToString(file, &bytes));
    std::optional<base::Value> value =
        base::JSONReader::Read(bytes, base::JSON_ALLOW_TRAILING_COMMAS);
    ASSERT_TRUE(value && value->is_dict());
    (*value->GetDict().FindList("steps"))[0].GetDict().Set(
        "target", "https://evil.example/");
    ASSERT_TRUE(base::JSONWriter::Write(*value, &bytes));
    ASSERT_TRUE(base::WriteFile(file, bytes));
  }
  // Another program writes the workflow's last fire time (bookkeeping: the
  // file is still the browser's).
  void SetLastFiredOutside(const std::string& id, int64_t last_fired) {
    const base::FilePath file = dir_.AppendASCII(id + ".molt");
    std::string bytes;
    ASSERT_TRUE(base::ReadFileToString(file, &bytes));
    std::optional<base::Value> value =
        base::JSONReader::Read(bytes, base::JSON_ALLOW_TRAILING_COMMAS);
    ASSERT_TRUE(value && value->is_dict());
    value->GetDict().FindDict("trigger")->Set("last_fired_unix",
                                              static_cast<double>(last_fired));
    ASSERT_TRUE(base::JSONWriter::Write(*value, &bytes));
    ASSERT_TRUE(base::WriteFile(file, bytes));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_;
  base::FilePath dir_;
  std::optional<ScopedWorkflowRecordForTesting> record_;
  std::optional<AutomationStorage> storage_;
  AutomationScheduler scheduler_;
  std::vector<std::string> fired_;
};

// The control: a workflow the browser saved, whose run was missed, fires
// once, then an interval after that run.
TEST_F(AutomationSchedulerTest, FiresAVouchedCatchUp) {
  const int64_t now = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 3600, now - 7200)));
  scheduler_.Start();
  Tick();
  // Not again by a timer: counted from the last fire before the catch-up,
  // the next one would be overdue.
  task_environment_.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
  // Its fire was kept: not again by the next listing.
  Tick();
  EXPECT_EQ(fired_.size(), 1u);
  task_environment_.FastForwardBy(base::Hours(1));
  EXPECT_EQ(fired_.size(), 2u);
}

TEST_F(AutomationSchedulerTest, NeverFiresAWorkflowChangedOutside) {
  const int64_t now = Unix(base::Time::Now());
  Script planted = Interval("wf_1", 60, now - 7200);
  planted.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(base::CreateDirectory(dir_));
  ASSERT_TRUE(
      base::WriteFile(dir_.AppendASCII("wf_1.molt"), planted.ToJSONString()));
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(5));
  EXPECT_TRUE(fired_.empty());
}

TEST_F(AutomationSchedulerTest, ATimerReloadThatFindsAChangeDoesNotFire) {
  const int64_t now = Unix(base::Time::Now());
  // Due in 70 s and in 80 s; nothing missed.
  ASSERT_TRUE(storage_->Save(Interval("wf_a", 90, now - 20)));
  ASSERT_TRUE(storage_->Save(Interval("wf_b", 90, now - 10)));
  scheduler_.Start();
  Tick();
  task_environment_.FastForwardBy(base::Seconds(65));
  EXPECT_TRUE(fired_.empty());
  // Changed after its last listing (the heartbeat's, at 60 s), before its
  // timer: the reload finds it changed, and it doesn't fire.
  ChangeOutside("wf_a");
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_TRUE(fired_.empty());
  // The other one's timer still fires it.
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_b"});
}

// A timer's reload that finds the workflow fired since its last listing
// (by another browser on the same folder) doesn't fire it again; an
// interval after that run, it fires.
TEST_F(AutomationSchedulerTest, ATimerReloadOfAWorkflowFiredSinceDoesNotFire) {
  const int64_t now = Unix(base::Time::Now());
  // Due in 70 s.
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 90, now - 20)));
  scheduler_.Start();
  Tick();
  task_environment_.FastForwardBy(base::Seconds(65));
  EXPECT_TRUE(fired_.empty());
  // Run elsewhere after the heartbeat's listing at 60 s, before the timer.
  SetLastFiredOutside("wf_1", now + 65);
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Seconds(85));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

TEST_F(AutomationSchedulerTest, AForgedEarlierLastFiredForcesNoCatchUp) {
  const int64_t now = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 3600, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", now));
  // Another program writes a last fire a day back.
  SetLastFiredOutside("wf_1", now - 86400);
  std::optional<Script> loaded = storage_->Load("wf_1");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->security.changed_outside);
  EXPECT_EQ(loaded->trigger.last_fired_unix, now);
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
}

// The same for workflows saved here that this browser hasn't fired yet
// (no fire of its own to hold on to): a weekly one, and an hourly one.
TEST_F(AutomationSchedulerTest,
       AForgedEarlierLastFiredOfAWorkflowNeverFiredHereForcesNoCatchUp) {
  Script weekly = Interval("wf_weekly", 3600, 0);
  weekly.trigger.type = TriggerType::CRON;
  weekly.trigger.expression = "0 9 * * 1";
  weekly.security.trust = TrustLevel::TRUSTED;
  ASSERT_TRUE(storage_->Save(weekly));
  ASSERT_TRUE(storage_->Save(Interval("wf_hourly", 3600, 0)));
  SetLastFiredOutside("wf_weekly", 1);
  SetLastFiredOutside("wf_hourly", 1);
  for (const char* id : {"wf_weekly", "wf_hourly"}) {
    std::optional<Script> loaded = storage_->Load(id);
    ASSERT_TRUE(loaded) << id;
    EXPECT_FALSE(loaded->security.changed_outside) << id;
    EXPECT_EQ(loaded->trigger.last_fired_unix, 0) << id;
  }
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
}

// A last fire far in the future written outside counts as a fire when it
// was first read: no catch-up run of the overdue workflow, which fires an
// interval later (the heartbeat's listings don't push it back), not when
// that time comes; its fire replaces it.
TEST_F(AutomationSchedulerTest, AForgedLaterLastFiredDoesNotStallTheSchedule) {
  const int64_t now = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 3600, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", now - 7200));
  SetLastFiredOutside("wf_1", 4102444800);
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(59));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
  task_environment_.FastForwardBy(base::Hours(1));
  EXPECT_EQ(fired_.size(), 2u);
}

// Another browser on the same folder ran the workflow (seen here), then
// the user snoozed it there (its last fire an hour ahead): this browser
// runs no catch-up from its own old fire, nothing while the snooze lasts,
// and runs it an interval after the snoozed time, as the other browser
// does.
TEST_F(AutomationSchedulerTest, AnotherBrowsersSnoozeHoldsTheScheduleHere) {
  const int64_t saved = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 3600, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", saved));
  // Three hours on, the other browser's run half an hour ago.
  task_environment_.FastForwardBy(base::Hours(3));
  const int64_t now = Unix(base::Time::Now());
  SetLastFiredOutside("wf_1", now - 1800);
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  // Snoozed there for an hour.
  SetLastFiredOutside("wf_1", now + 3600);
  Tick();
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(119));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

// A schedule saved afresh (the Agent studio's Schedule writes a new
// trigger, an interval counted from then) starts over: the old schedule's
// last fire, five hours back, forces no catch-up run of the new hourly
// one, which fires an hour later (the heartbeat's listings don't push it
// back).
TEST_F(AutomationSchedulerTest, AScheduleSavedAfreshForcesNoCatchUp) {
  const int64_t now = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 86400, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", now - 5 * 3600));
  const Trigger hourly = Interval("wf_1", 3600, now).trigger;
  ASSERT_TRUE(storage_->Update("wf_1", [&hourly](Script& script) {
    script.trigger = hourly;
    return true;
  }));
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(59));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

}  // namespace
}  // namespace automation
}  // namespace molt_ai
