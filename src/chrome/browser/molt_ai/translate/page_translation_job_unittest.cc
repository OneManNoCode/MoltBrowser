// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translation_job.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/translate/page_translate_script.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::page_translate {
namespace {

using web_ai::GenerationJob;
using web_ai::GenerationStatus;

// The web AI backend, answering what the test says.
class FakeBackend : public web_ai::WebAIBackend {
 public:
  struct Pending {
    GenerationJob job;
    ChunkCallback on_chunk;
    DoneCallback on_done;
  };

  FakeBackend() {
    facts.inference_supported = true;
    facts.model_installed = true;
    facts.some_model_fits = true;
    facts.web_apis_enabled = false;  // Not the page-translation switch.
  }

  // web_ai::WebAIBackend:
  void GetDeviceFacts(DeviceCallback callback) override {
    ++facts_calls;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), facts));
  }
  void PrepareModel(bool wants_images, PrepareCallback callback) override {
    ++prepare_calls;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), prepare_ok,
                                  uint32_t{4096}, false));
  }
  uint64_t Generate(GenerationJob job,
                    ChunkCallback on_chunk,
                    DoneCallback on_done) override {
    const uint64_t id = next_id_++;
    if (throttle > 0) {
      --throttle;
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(on_done), GenerationStatus::kThrottled));
      return id;
    }
    jobs[id] = {std::move(job), std::move(on_chunk), std::move(on_done)};
    ++generations;
    return id;
  }
  void Cancel(uint64_t id) override {
    cancelled.push_back(id);
    jobs.erase(id);
  }
  bool TryAddSession(const std::string& site_key) override { return true; }
  void RemoveSession(const std::string& site_key) override {}
  void PrepareImages(std::vector<SkBitmap> bitmaps,
                     ImagesCallback callback) override {}
  bool TryHoldImages(const std::string& site_key, size_t count) override {
    return true;
  }
  void ReleaseImages(const std::string& site_key, size_t count) override {}

  // The one running generation (the job sends one at a time).
  Pending& Running() {
    EXPECT_EQ(jobs.size(), 1u);
    return jobs.begin()->second;
  }
  // Answers the running generation with |reply| in two chunks.
  void Answer(const std::string& reply,
              GenerationStatus status = GenerationStatus::kOk) {
    Pending pending = std::move(Running());
    jobs.erase(jobs.begin());
    const size_t half = reply.size() / 2;
    pending.on_chunk.Run(reply.substr(0, half));
    pending.on_chunk.Run(reply.substr(half));
    std::move(pending.on_done).Run(status);
  }
  // Answers with |fn| applied to each text of the running batch.
  template <typename Fn>
  void Translate(Fn fn) {
    std::optional<base::Value> texts = base::JSONReader::Read(
        Running().job.user_message, base::JSON_PARSE_RFC);
    base::ListValue out;
    for (const base::Value& text : texts->GetList()) {
      out.Append(fn(text.GetString()));
    }
    base::DictValue reply;
    reply.Set(web_ai::kTranslatorBatchResponseKey, std::move(out));
    std::string json;
    base::JSONWriter::Write(reply, &json);
    Answer(json);
  }

  web_ai::DeviceFacts facts;
  bool prepare_ok = true;
  int throttle = 0;
  int facts_calls = 0;
  int prepare_calls = 0;
  int generations = 0;
  std::map<uint64_t, Pending> jobs;
  std::vector<uint64_t> cancelled;

 private:
  uint64_t next_id_ = 1;
};

// A document: its slots' values, changed only through the script's ops
// (read back from the JSON the job built), answered later.
class FakePage : public PageScriptRunner {
 public:
  struct State {
    std::vector<std::string> values;
    std::vector<bool> in_view;
    bool alive = true;
    bool garble_extract = false;
    bool lose_state = false;
    // Writes held back (the page is slow) until ReleaseWrites().
    bool hold_writes = false;
    std::vector<base::OnceClosure> held;
    int extracts = 0;
    int applies = 0;
    std::string id;
  };

  explicit FakePage(State* state) : state_(state) {}

  bool IsAlive() override { return state_->alive; }
  bool IsForeground() override { return true; }
  void Run(const std::string& script,
           base::OnceCallback<void(base::Value)> callback) override {
    const std::string prefix = std::string(kPageTranslateScript) + "(";
    EXPECT_EQ(script.substr(0, prefix.size()), prefix);
    std::optional<base::Value> args = base::JSONReader::Read(
        std::string_view(script).substr(prefix.size(),
                                        script.size() - prefix.size() - 1),
        base::JSON_PARSE_RFC);
    ASSERT_TRUE(args && args->is_dict());
    const base::DictValue& dict = args->GetDict();
    const std::string op = *dict.FindString("op");
    base::DictValue result;
    result.Set("v", 1);
    if (op == "extract") {
      ++state_->extracts;
      state_->id = *dict.FindString("id");
      result.Set("id", state_->id);
      result.Set("truncated", false);
      base::ListValue slots;
      for (size_t i = 0; i < state_->values.size(); ++i) {
        base::ListValue slot;
        slot.Append(0);
        slot.Append(i < state_->in_view.size() && state_->in_view[i]);
        slot.Append(state_->values[i]);
        slots.Append(std::move(slot));
      }
      if (state_->garble_extract) {
        result.Set("slots", "not a list");
      } else {
        result.Set("slots", std::move(slots));
      }
    } else if (op == "apply") {
      ++state_->applies;
      result.Set("id", *dict.FindString("id"));
      int applied = 0;
      for (const base::Value& entry : *dict.FindList("writes")) {
        const base::ListValue& write = entry.GetList();
        state_->values[static_cast<size_t>(write[0].GetInt())] =
            write[1].GetString();
        ++applied;
      }
      result.Set("applied", state_->lose_state ? -1 : applied);
    }
    base::OnceClosure answer = base::BindOnce(
        [](base::OnceCallback<void(base::Value)> reply,
           base::DictValue value) {
          std::move(reply).Run(base::Value(std::move(value)));
        },
        std::move(callback), std::move(result));
    if (op == "apply" && state_->hold_writes) {
      state_->held.push_back(std::move(answer));
      return;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(answer));
  }

 private:
  const raw_ptr<FakePage::State> state_;
};

std::string Upper(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

class PageTranslationJobTest : public testing::Test {
 protected:
  void Start(std::vector<std::string> values,
             std::vector<bool> in_view = {}) {
    page_.values = std::move(values);
    page_.in_view = std::move(in_view);
    job_ = std::make_unique<PageTranslationJob>(
        std::make_unique<FakePage>(&page_),
        *web_ai::ResolveTranslatorPair("de", "en"), backend_, "key",
        base::BindRepeating(
            [](std::vector<std::pair<size_t, size_t>>* progress, size_t s,
               size_t t) { progress->emplace_back(s, t); },
            &progress_),
        base::BindOnce(
            [](std::optional<JobResult>* out, JobResult result) {
              *out = result;
            },
            &result_));
    job_->Start();
    task_environment_.RunUntilIdle();
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeBackend backend_;
  FakePage::State page_;
  std::unique_ptr<PageTranslationJob> job_;
  std::vector<std::pair<size_t, size_t>> progress_;
  std::optional<JobResult> result_;
};

TEST_F(PageTranslationJobTest, TranslatesThePageAndWritesItBack) {
  Start({"  guten Tag ", "Mehr lesen", "2026"}, {false, true, false});
  ASSERT_EQ(backend_.jobs.size(), 1u);
  const GenerationJob& job = backend_.Running().job;
  // Visible text first; the digits never reach the model.
  EXPECT_EQ(job.user_message, "[\n\"Mehr lesen\",\n\"guten Tag\"\n]");
  EXPECT_EQ(job.json_schema, web_ai::TranslatorBatchResponseSchema(2));
  EXPECT_EQ(job.system_prompt,
            web_ai::BuildTranslatorBatchPrompt(
                *web_ai::ResolveTranslatorPair("de", "en"), {"x"})
                .system_prompt);
  EXPECT_EQ(job.site_key, "key");
  EXPECT_TRUE(job.foreground);

  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kFinished);
  EXPECT_EQ(result_->units, 2u);
  EXPECT_EQ(result_->written_slots, 2u);
  EXPECT_EQ(result_->failed_units, 0u);
  EXPECT_EQ(page_.values,
            (std::vector<std::string>{"  GUTEN TAG ", "MEHR LESEN", "2026"}));
  ASSERT_FALSE(progress_.empty());
  EXPECT_EQ(progress_.front(), (std::pair<size_t, size_t>(0, 2)));
  EXPECT_EQ(progress_.back(), (std::pair<size_t, size_t>(2, 2)));
}

TEST_F(PageTranslationJobTest, WithoutAModelNothingIsReadOrLoaded) {
  backend_.facts.model_installed = false;
  Start({"Hallo"});
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kNoModel);
  EXPECT_EQ(page_.extracts, 0);
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(ReportForJob(*result_).problem, Problem::kNoModel);
}

TEST_F(PageTranslationJobTest, AnUnsupportedDevice) {
  backend_.facts.inference_supported = false;
  Start({"Hallo"});
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kDeviceUnsupported);
  EXPECT_EQ(page_.extracts, 0);
}

TEST_F(PageTranslationJobTest, NothingToTranslateLoadsNoModel) {
  Start({"2026", " - ", "12:30"});
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kFinished);
  EXPECT_EQ(result_->units, 0u);
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(ReportForJob(*result_).problem, Problem::kNothingToTranslate);
}

TEST_F(PageTranslationJobTest, AModelThatDoesNotLoad) {
  backend_.prepare_ok = false;
  Start({"Hallo"});
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kModelFailed);
  EXPECT_EQ(backend_.generations, 0);
}

TEST_F(PageTranslationJobTest, AnUnreadablePage) {
  page_.garble_extract = true;
  Start({"Hallo"});
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kPageUnreadable);
  EXPECT_EQ(backend_.prepare_calls, 0);
}

TEST_F(PageTranslationJobTest, AnUnusableReplyIsTriedAgainInHalves) {
  Start({"eins", "zwei", "drei", "vier"});
  // Text around the JSON: not a reply.
  backend_.Answer("Sure! {\"translations\":[\"1\",\"2\",\"3\",\"4\"]}");
  task_environment_.RunUntilIdle();
  ASSERT_EQ(backend_.jobs.size(), 1u);
  EXPECT_EQ(backend_.Running().job.json_schema,
            web_ai::TranslatorBatchResponseSchema(2));
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kFinished);
  EXPECT_EQ(result_->written_slots, 4u);
}

TEST_F(PageTranslationJobTest, ModelThatKeepsFailingEndsTheJob) {
  std::vector<std::string> texts;
  for (int i = 0; i < 40; ++i) {
    texts.push_back("Wort " + std::to_string(i));
  }
  Start(texts);
  int answered = 0;
  while (!backend_.jobs.empty()) {
    backend_.Answer("", GenerationStatus::kInvalidOutput);
    task_environment_.RunUntilIdle();
    ++answered;
  }
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kModelErrors);
  EXPECT_EQ(answered, kMaxFailedBatchesInARow);
  EXPECT_EQ(result_->written_slots, 0u);
  EXPECT_EQ(ReportForJob(*result_).error, TranslateError::kTranslation);
}

TEST_F(PageTranslationJobTest, ThrottledBatchesGoAgain) {
  backend_.throttle = 2;
  Start({"Hallo"});
  // The retry waits a second each time.
  task_environment_.FastForwardBy(base::Seconds(1));
  task_environment_.FastForwardBy(base::Seconds(1));
  ASSERT_EQ(backend_.jobs.size(), 1u);
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kFinished);
}

TEST_F(PageTranslationJobTest, StopCancelsTheRunningGeneration) {
  Start({"Hallo"});
  ASSERT_EQ(backend_.jobs.size(), 1u);
  const uint64_t id = backend_.jobs.begin()->first;
  job_->Stop();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kStopped);
  EXPECT_EQ(backend_.cancelled, std::vector<uint64_t>{id});
  EXPECT_EQ(page_.values, std::vector<std::string>{"Hallo"});
  EXPECT_EQ(ReportForJob(*result_).error, TranslateError::kTranslation);
}

TEST_F(PageTranslationJobTest, StopWaitsForAWriteInFlight) {
  std::vector<std::string> texts;
  for (int i = 0; i < 20; ++i) {
    texts.push_back("Wort " + std::to_string(i));
  }
  Start(texts);
  page_.hold_writes = true;
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(page_.held.size(), 1u);
  job_->Stop();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result_);
  std::move(page_.held.front()).Run();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kStopped);
  // The first batch's writes count, so the page can be shown in its
  // original again.
  EXPECT_EQ(result_->written_slots, web_ai::kTranslatorMaxBatchTexts);
  const Report report = ReportForJob(*result_);
  EXPECT_EQ(report.error, TranslateError::kNone);
  EXPECT_TRUE(report.partial);
  // Nothing else was sent.
  EXPECT_TRUE(backend_.jobs.empty());
}

TEST_F(PageTranslationJobTest, StoppedBeforeReadingNothingElseHappens) {
  page_.values = {"Hallo"};
  job_ = std::make_unique<PageTranslationJob>(
      std::make_unique<FakePage>(&page_),
      *web_ai::ResolveTranslatorPair("de", "en"), backend_, "key",
      base::DoNothing(),
      base::BindOnce([](std::optional<JobResult>* out,
                        JobResult result) { *out = result; },
                     &result_));
  job_->Start();
  job_->Stop();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kStopped);
  // The device facts that arrive after Stop() start nothing.
  EXPECT_EQ(page_.extracts, 0);
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(backend_.generations, 0);
}

TEST_F(PageTranslationJobTest, TheDeadlineEndsTheJob) {
  Start({"Hallo"});
  ASSERT_EQ(backend_.jobs.size(), 1u);
  task_environment_.FastForwardBy(base::Minutes(kMaxTranslateMinutes));
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kTimedOut);
  EXPECT_EQ(backend_.cancelled.size(), 1u);
}

// A page that never answers a write (a busy or hung renderer) doesn't
// keep the job past its deadline.
TEST_F(PageTranslationJobTest, TheDeadlineEndsAJobWaitingForAWrite) {
  std::vector<std::string> texts;
  for (int i = 0; i < 20; ++i) {
    texts.push_back("Wort " + std::to_string(i));
  }
  Start(texts);
  page_.hold_writes = true;
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(page_.held.size(), 1u);
  EXPECT_FALSE(result_);
  task_environment_.FastForwardBy(base::Minutes(kMaxTranslateMinutes));
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kTimedOut);
  // The write may still land: it counts, so "Show original" is offered.
  EXPECT_EQ(result_->written_slots, web_ai::kTranslatorMaxBatchTexts);
  const Report report = ReportForJob(*result_);
  EXPECT_EQ(report.problem, Problem::kTimedOut);
  EXPECT_TRUE(report.partial);
  // Nothing else was sent.
  EXPECT_TRUE(backend_.jobs.empty());
  EXPECT_EQ(backend_.generations, 1);

  // The page answers at last: the job doesn't hear it, reports nothing
  // more and sends nothing more.
  result_.reset();
  std::move(page_.held.front()).Run();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result_);
  EXPECT_TRUE(backend_.jobs.empty());
  EXPECT_EQ(page_.applies, 1);

  // And the job can go away afterwards.
  job_.reset();
  task_environment_.RunUntilIdle();
}

TEST_F(PageTranslationJobTest, AStopWaitingForAWriteEndsAtTheDeadline) {
  std::vector<std::string> texts;
  for (int i = 0; i < 20; ++i) {
    texts.push_back("Wort " + std::to_string(i));
  }
  Start(texts);
  page_.hold_writes = true;
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(page_.held.size(), 1u);
  job_->Stop();
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_FALSE(result_);
  task_environment_.FastForwardBy(base::Minutes(kMaxTranslateMinutes));
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kStopped);
  EXPECT_EQ(result_->written_slots, web_ai::kTranslatorMaxBatchTexts);
  EXPECT_TRUE(backend_.jobs.empty());
}

TEST_F(PageTranslationJobTest, APageThatLosesTheJobEndsIt) {
  Start({"eins", "zwei"});
  page_.lose_state = true;
  backend_.Translate(Upper);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kPageUnreadable);
}

TEST_F(PageTranslationJobTest, AGonePageGetsNoMoreGenerations) {
  std::vector<std::string> texts;
  for (int i = 0; i < 20; ++i) {
    texts.push_back("Wort " + std::to_string(i));
  }
  Start(texts);
  backend_.Translate(Upper);
  page_.alive = false;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->end, JobEnd::kPageUnreadable);
  EXPECT_EQ(backend_.generations, 1);
}

TEST_F(PageTranslationJobTest, ADestroyedJobCancelsAndNeverReports) {
  Start({"Hallo"});
  ASSERT_EQ(backend_.jobs.size(), 1u);
  job_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result_);
  EXPECT_EQ(backend_.cancelled.size(), 1u);

  // Also once it has finished but not yet reported.
  backend_.facts.model_installed = false;
  page_ = FakePage::State();
  job_ = std::make_unique<PageTranslationJob>(
      std::make_unique<FakePage>(&page_),
      *web_ai::ResolveTranslatorPair("de", "en"), backend_, "key",
      base::DoNothing(),
      base::BindOnce([](std::optional<JobResult>* out,
                        JobResult result) { *out = result; },
                     &result_));
  job_->Start();
  job_->Stop();
  job_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result_);
}

// The driver's way in (StartPageTranslation()): in a MoltNet window no job
// is made, whatever TranslateManager asked, so the page isn't read and the
// backend isn't asked anything; the decision says why.
TEST_F(PageTranslationJobTest, AMoltNetPageNeverStarts) {
  page_.values = {"guten Tag", "Mehr lesen"};
  int made = 0;
  auto make_job = [&](const web_ai::TranslatorPair& pair) {
    ++made;
    return std::make_unique<PageTranslationJob>(
        std::make_unique<FakePage>(&page_), pair, backend_, "key",
        base::DoNothing(),
        base::BindOnce([](std::optional<JobResult>* out,
                          JobResult result) { *out = result; },
                       &result_));
  };

  PageTranslationStart started = StartPageTranslation(
      /*moltnet=*/true, ResolvePagePair("de", "en"), make_job);
  task_environment_.FastForwardBy(base::Minutes(kMaxTranslateMinutes + 1));
  EXPECT_FALSE(started.job);
  EXPECT_FALSE(started.decision.start);
  EXPECT_EQ(started.decision.report.error, TranslateError::kInitialization);
  EXPECT_EQ(started.decision.report.problem, Problem::kOffInMoltNet);
  EXPECT_EQ(made, 0);
  EXPECT_EQ(page_.extracts, 0);
  EXPECT_EQ(page_.applies, 0);
  EXPECT_EQ(backend_.facts_calls, 0);
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(backend_.generations, 0);
  EXPECT_FALSE(result_);
  EXPECT_EQ(page_.values[0], "guten Tag");

  // Not for a pair the table doesn't cover either, in any window.
  started = StartPageTranslation(/*moltnet=*/false,
                                 ResolvePagePair("de", "de"), make_job);
  EXPECT_FALSE(started.job);
  EXPECT_EQ(started.decision.report.problem, Problem::kIdentical);
  EXPECT_EQ(made, 0);

  // The same page in a regular window starts, and reads the page.
  started = StartPageTranslation(/*moltnet=*/false,
                                 ResolvePagePair("de", "en"), make_job);
  ASSERT_TRUE(started.job);
  EXPECT_TRUE(started.decision.start);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(made, 1);
  EXPECT_EQ(backend_.facts_calls, 1);
  EXPECT_EQ(page_.extracts, 1);
  EXPECT_EQ(backend_.generations, 1);
}

}  // namespace
}  // namespace molt_ai::page_translate
