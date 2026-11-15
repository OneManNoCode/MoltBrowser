// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AutomationScheduler (automation_scheduler.h) over a real shared workflow
// directory with a workflow record (workflow_record.h), on mock time: it
// fires a catch-up of a workflow the browser saved, once, and its next
// fire an interval later; never one whose file was changed outside the
// browser, whether that is found when it is listed or when its timer's
// reload comes, nor one its timer's reload finds snoozed since. It fires
// on this browser's own last fire only: a last fire written into the file
// outside (another browser's run or Snooze, or a forged one, earlier or
// later) changes nothing, a Snooze saved here holds, a restart keeps the
// recorded fire, and a schedule saved afresh starts over.

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
  // The browser restarted: the record loaded again from its file, and the
  // scheduler's first listing.
  void Restart() {
    scheduler_.Stop();
    task_environment_.RunUntilIdle();
    record_.reset();
    record_.emplace(dir_, temp_.GetPath().Append(kWorkflowRecordFileName),
                    /*may_seed=*/false);
    storage_.emplace(dir_);
    scheduler_.Start();
    Tick();
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
  // Another program (another browser's run or Snooze) writes the
  // workflow's last fire time (bookkeeping: the file is still the
  // browser's).
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

// A timer's reload that finds the workflow snoozed here since its last
// listing (molt://ai-automation's Snooze writes a last fire still to come)
// doesn't fire it; an interval after the snoozed time, it fires.
TEST_F(AutomationSchedulerTest,
       ATimerReloadOfAWorkflowSnoozedSinceDoesNotFire) {
  const int64_t now = Unix(base::Time::Now());
  // Due in 70 s.
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 90, now - 20)));
  scheduler_.Start();
  Tick();
  task_environment_.FastForwardBy(base::Seconds(65));
  EXPECT_TRUE(fired_.empty());
  // Snoozed here for an hour, after the heartbeat's listing at 60 s and
  // before the timer: next due at 65 + 3600 + 90 s.
  ASSERT_TRUE(storage_->Update("wf_1", [now](Script& script) {
    script.trigger.last_fired_unix = now + 65 + 3600;
    return true;
  }));
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_TRUE(fired_.empty());
  // At 3754 s.
  task_environment_.FastForwardBy(base::Seconds(3600 + 90 - 10 - 1));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

// Another browser on the same folder runs the workflow (its fire written
// into the file) just before this browser's timer: this browser still
// fires it on its own schedule.
TEST_F(AutomationSchedulerTest, AnotherBrowsersFireDoesNotMoveThisOnes) {
  const int64_t now = Unix(base::Time::Now());
  // Due in 70 s.
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 90, now - 20)));
  scheduler_.Start();
  Tick();
  task_environment_.FastForwardBy(base::Seconds(65));
  EXPECT_TRUE(fired_.empty());
  SetLastFiredOutside("wf_1", now + 65);
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

// A last fire written outside, earlier or later than this browser's own,
// changes nothing: no catch-up run (an earlier one would force one), no
// schedule stalled (a later one would hold it back); each fires an
// interval after this browser's own fire.
TEST_F(AutomationSchedulerTest, AForgedLastFiredChangesNothing) {
  const int64_t now = Unix(base::Time::Now());
  for (const char* id : {"wf_earlier", "wf_later"}) {
    ASSERT_TRUE(storage_->Save(Interval(id, 3600, 0)));
    ASSERT_TRUE(storage_->RecordFire(id, now - 600));
  }
  SetLastFiredOutside("wf_earlier", now - 86400);
  SetLastFiredOutside("wf_later", 4102444800);
  for (const char* id : {"wf_earlier", "wf_later"}) {
    std::optional<Script> loaded = storage_->Load(id);
    ASSERT_TRUE(loaded) << id;
    EXPECT_FALSE(loaded->security.changed_outside) << id;
    EXPECT_EQ(loaded->trigger.last_fired_unix, now - 600) << id;
  }
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(49));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_.size(), 2u);
}

// The same for workflows saved here that this browser hasn't fired yet
// (no fire of its own): an earlier last fire written outside forces no
// catch-up run of a weekly one, nor of an hourly one.
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

// Another browser on the same folder ran the workflow and then snoozed it
// there (its last fire an hour ahead in the file): this browser runs the
// catch-up its own last fire calls for at once, and its next run an
// interval after that.
TEST_F(AutomationSchedulerTest, AnotherBrowsersRunAndSnoozeDoNotHoldThisOne) {
  const int64_t saved = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 3600, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", saved));
  // Three hours on, the other browser's run half an hour ago, then its
  // Snooze.
  task_environment_.FastForwardBy(base::Hours(3));
  const int64_t now = Unix(base::Time::Now());
  SetLastFiredOutside("wf_1", now - 1800);
  SetLastFiredOutside("wf_1", now + 3600);
  scheduler_.Start();
  Tick();
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
  task_environment_.FastForwardBy(base::Minutes(59));
  EXPECT_EQ(fired_.size(), 1u);
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_.size(), 2u);
}

// A Snooze saved here (a browser write: the record takes its later last
// fire) holds the schedule for the whole snooze, and the workflow runs an
// interval after the snoozed time.
TEST_F(AutomationSchedulerTest, ASnoozeSavedHereHolds) {
  const int64_t saved = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 900, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", saved));
  task_environment_.FastForwardBy(base::Minutes(10));
  const int64_t snoozed = Unix(base::Time::Now()) + 3600;
  ASSERT_TRUE(storage_->Update("wf_1", [snoozed](Script& script) {
    script.trigger.last_fired_unix = snoozed;
    return true;
  }));
  scheduler_.Start();
  Tick();
  task_environment_.FastForwardBy(base::Minutes(60 + 14));
  EXPECT_TRUE(fired_.empty());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_EQ(fired_, std::vector<std::string>{"wf_1"});
}

// A restart keeps the recorded fire: a daily workflow of a browser
// restarted every eight hours runs a day after its last fire here, not
// after the later one another browser wrote into the file.
TEST_F(AutomationSchedulerTest, ARestartKeepsTheRecordedFire) {
  const int64_t now = Unix(base::Time::Now());
  ASSERT_TRUE(storage_->Save(Interval("wf_1", 86400, 0)));
  ASSERT_TRUE(storage_->RecordFire("wf_1", now - 3600));
  SetLastFiredOutside("wf_1", now - 1800);
  scheduler_.Start();
  Tick();
  EXPECT_TRUE(fired_.empty());
  for (int i = 0; i < 2; ++i) {
    task_environment_.FastForwardBy(base::Hours(8));
    Restart();
    EXPECT_TRUE(fired_.empty());
  }
  task_environment_.FastForwardBy(base::Hours(6) + base::Minutes(59));
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
