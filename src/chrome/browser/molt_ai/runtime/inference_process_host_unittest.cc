// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// InferenceProcessHost against a fake, in-process MoltInferenceService that
// misbehaves on request: crash mid-stream, hang, flood, bad UTF-8, a bad
// ModelDescription, cancel while queued, browser-side stop sequences.

#include "chrome/browser/molt_ai/runtime/inference_process_host.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/functions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

using Kind = InferenceOutcome::Kind;

// The fake service process: one MoltInferenceService + Model, scripted by
// the test.
class FakeService : public mojom::MoltInferenceService, public mojom::Model {
 public:
  enum class LoadBehavior { kOk, kFail, kHang, kBadDescription };

  void Bind(mojo::PendingReceiver<mojom::MoltInferenceService> receiver) {
    service_receiver_.Bind(std::move(receiver));
  }

  // mojom::MoltInferenceService:
  void LoadModel(mojom::ModelLoadParamsPtr params,
                 mojo::PendingReceiver<mojom::Model> model,
                 LoadModelCallback callback) override {
    load_params_ = std::move(params);
    model_receiver_.Bind(std::move(model));
    switch (load_behavior) {
      case LoadBehavior::kOk: {
        auto d = mojom::ModelDescription::New();
        d->architecture = "llama";
        d->backend = "cpu";
        d->context_tokens = 4096;
        d->train_context_tokens = 4096;
        d->vocab_size = 32000;
        std::move(callback).Run(mojom::LoadModelStatus::kOk, std::move(d));
        break;
      }
      case LoadBehavior::kFail:
        std::move(callback).Run(mojom::LoadModelStatus::kInvalidModelFile,
                                nullptr);
        break;
      case LoadBehavior::kHang:
        pending_load_ = std::move(callback);
        break;
      case LoadBehavior::kBadDescription: {
        auto d = mojom::ModelDescription::New();
        d->architecture = "<script>";
        d->backend = "cpu";
        d->context_tokens = 4096;
        d->vocab_size = 10;
        std::move(callback).Run(mojom::LoadModelStatus::kOk, std::move(d));
        break;
      }
    }
  }

  // mojom::Model:
  void StreamChat(
      mojom::ChatRequestPtr request,
      mojom::GenerationParamsPtr params,
      mojo::PendingRemote<mojom::GenerationStreamer> streamer) override {
    streamers_.emplace_back(std::move(streamer));
  }
  void StreamPrompt(
      mojom::RawPromptRequestPtr request,
      mojom::GenerationParamsPtr params,
      mojo::PendingRemote<mojom::GenerationStreamer> streamer) override {
    streamers_.emplace_back(std::move(streamer));
  }
  void Tokenize(const std::string& text, TokenizeCallback callback) override {
    std::move(callback).Run(std::nullopt);
  }
  void CountTokens(const std::string& text,
                   CountTokensCallback callback) override {
    std::move(callback).Run(std::nullopt);
  }
  void ForgetConversation(const std::string& conversation_key) override {}

  // Simulates the process dying: every pipe closes.
  void Crash() {
    streamers_.clear();
    model_receiver_.reset();
    service_receiver_.reset();
  }

  mojo::Remote<mojom::GenerationStreamer>& streamer(size_t i) {
    return streamers_[i];
  }
  size_t streamer_count() const { return streamers_.size(); }
  bool model_connected() const { return model_receiver_.is_bound(); }

  LoadBehavior load_behavior = LoadBehavior::kOk;

 private:
  mojo::Receiver<mojom::MoltInferenceService> service_receiver_{this};
  mojo::Receiver<mojom::Model> model_receiver_{this};
  mojom::ModelLoadParamsPtr load_params_;
  LoadModelCallback pending_load_;
  std::vector<mojo::Remote<mojom::GenerationStreamer>> streamers_;
};

class RecordingDelegate : public InferenceProcessHost::Delegate {
 public:
  void OnModelProcessLost(const std::string& model_id,
                          const std::string& crash_key,
                          CrashLoopGuard::Phase phase) override {
    lost.push_back(phase);
  }
  std::vector<CrashLoopGuard::Phase> lost;
};

class InferenceProcessHostTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    static bool mojo_initialized = false;
    if (!mojo_initialized) {
      mojo::core::Init();
      mojo_initialized = true;
    }
  }

  void SetUp() override {
    mojo::SetDefaultProcessErrorHandler(base::BindRepeating(
        [](std::vector<std::string>* out, const std::string& error) {
          out->push_back(error);
        },
        base::Unretained(&bad_messages_)));
    host_ = std::make_unique<InferenceProcessHost>(&delegate_);
    InferenceProcessHost::Timeouts t;  // the real ones; mock time
    host_->SetTimeoutsForTesting(t);
    host_->SetLauncherForTesting(base::BindRepeating(
        [](InferenceProcessHostTest* self,
           mojo::PendingReceiver<mojom::MoltInferenceService> receiver,
           base::WeakPtr<content::ServiceProcessHost::Observer> observer,
           base::OnceCallback<void(const base::Process&)> on_process) {
          self->services_.push_back(std::make_unique<FakeService>());
          self->services_.back()->load_behavior = self->next_load_behavior_;
          self->services_.back()->Bind(std::move(receiver));
        },
        base::Unretained(this)));
  }

  void TearDown() override {
    host_.reset();
    mojo::SetDefaultProcessErrorHandler(base::NullCallback());
  }

  scoped_refptr<InferenceMailbox> Load() {
    InferenceProcessHost::LoadRequest r;
    r.model_id = "m";
    r.crash_key = "m:1:2";
    r.file_size = 1;
    r.max_context_tokens = 32768;
    auto mailbox = base::MakeRefCounted<InferenceMailbox>();
    host_->Load(std::move(r), mailbox);
    task_environment_.RunUntilIdle();
    return mailbox;
  }

  scoped_refptr<InferenceMailbox> Chat(uint64_t token,
                                       StreamLimits limits = StreamLimits()) {
    auto mailbox = base::MakeRefCounted<InferenceMailbox>();
    auto request = mojom::ChatRequest::New();
    request->user_message = "hi";
    host_->StartChat(token, std::move(request),
                     mojom::GenerationParams::New(), std::move(limits),
                     mailbox);
    task_environment_.RunUntilIdle();
    return mailbox;
  }

  // True once |m| has its outcome; appends whatever text it holds to
  // |text| (if given).
  static bool Finished(InferenceMailbox* m, std::string* text = nullptr) {
    std::string taken;
    const bool finished = m->Wait(std::chrono::milliseconds(0), &taken);
    if (text) text->append(taken);
    return finished;
  }

  static std::string TakeText(InferenceMailbox* m) {
    std::string text;
    Finished(m, &text);
    return text;
  }

  mojom::GenerationSummaryPtr Summary() {
    auto s = mojom::GenerationSummary::New();
    s->status = mojom::GenerationStatus::kEndOfGeneration;
    s->prompt_tokens = 10;
    s->generated_tokens = 3;
    return s;
  }

  FakeService& service() { return *services_.back(); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  RecordingDelegate delegate_;
  std::vector<std::string> bad_messages_;
  FakeService::LoadBehavior next_load_behavior_ = FakeService::LoadBehavior::kOk;
  std::vector<std::unique_ptr<FakeService>> services_;
  std::unique_ptr<InferenceProcessHost> host_;
};

TEST_F(InferenceProcessHostTest, LoadsAndStreams) {
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_EQ(load->outcome().context_tokens, 4096u);
  EXPECT_TRUE(host_->HasCurrentModelForTesting());

  scoped_refptr<InferenceMailbox> gen = Chat(1);
  ASSERT_EQ(service().streamer_count(), 1u);
  service().streamer(0)->OnProgress(5, 10);
  service().streamer(0)->OnChunk("Hel");
  service().streamer(0)->OnChunk("lo \xF0\x9F\x98\x80");
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  std::string text;
  ASSERT_TRUE(Finished(gen.get(), &text));
  EXPECT_EQ(text, "Hello \xF0\x9F\x98\x80");
  InferenceOutcome outcome = gen->outcome();
  EXPECT_EQ(outcome.kind, Kind::kOk);
  EXPECT_TRUE(outcome.has_summary);
  EXPECT_EQ(outcome.summary.generated_tokens, 3u);
  EXPECT_TRUE(bad_messages_.empty());
  EXPECT_TRUE(delegate_.lost.empty());
  // The service's pipe for the request is closed after OnComplete.
  service().streamer(0).FlushForTesting();
  EXPECT_FALSE(service().streamer(0).is_connected());
}

TEST_F(InferenceProcessHostTest, TextReachesTheMailbox) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnChunk("abc");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(TakeText(gen.get()), "abc");
  EXPECT_FALSE(Finished(gen.get()));
}

TEST_F(InferenceProcessHostTest, InvalidUtf8KillsTheProcess) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnChunk("ok");
  service().streamer(0)->OnChunk("\xFF\xFE");
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kViolation);
  EXPECT_EQ(bad_messages_.size(), 1u);
  ASSERT_EQ(delegate_.lost.size(), 1u);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kGenerating);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
  EXPECT_FALSE(service().model_connected());
}

TEST_F(InferenceProcessHostTest, CrashMidStreamFailsTheRequest) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnChunk("partial");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(TakeText(gen.get()), "partial");
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kProcessGone);
  ASSERT_EQ(delegate_.lost.size(), 1u);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
}

TEST_F(InferenceProcessHostTest, HangIsKilledAfterTheStallTimeout) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  task_environment_.FastForwardBy(base::Seconds(119));
  EXPECT_FALSE(Finished(gen.get()));
  task_environment_.FastForwardBy(base::Seconds(3));
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kTimedOut);
  EXPECT_EQ(delegate_.lost.size(), 1u);
}

TEST_F(InferenceProcessHostTest, ProgressKeepsALongPromptAlive) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  for (int i = 0; i < 10; ++i) {
    task_environment_.FastForwardBy(base::Seconds(60));
    service().streamer(0)->OnProgress(i, 10);
    task_environment_.RunUntilIdle();
  }
  EXPECT_FALSE(Finished(gen.get()));
  // ... but not past the 15-minute deadline.
  for (int i = 0; i < 6; ++i) {
    task_environment_.FastForwardBy(base::Seconds(60));
    service().streamer(0)->OnProgress(i, 10);
    task_environment_.RunUntilIdle();
  }
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kTimedOut);
}

TEST_F(InferenceProcessHostTest, FloodKillsTheProcess) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  for (int i = 0; i < 2100; ++i) service().streamer(0)->OnChunk("x");
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kViolation);
}

TEST_F(InferenceProcessHostTest, OversizedChunkKillsTheProcess) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnChunk(std::string(5000, 'a'));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kViolation);
}

TEST_F(InferenceProcessHostTest, ChunkAfterCompleteNeverReachesTheCaller) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnChunk("a");
  service().streamer(0)->OnComplete(Summary());
  service().streamer(0)->OnChunk("late");
  task_environment_.RunUntilIdle();
  std::string text;
  ASSERT_TRUE(Finished(gen.get(), &text));
  EXPECT_EQ(gen->outcome().kind, Kind::kOk);
  EXPECT_EQ(text, "a");
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, CancelWhileQueued) {
  Load();
  scoped_refptr<InferenceMailbox> first = Chat(1);
  scoped_refptr<InferenceMailbox> second = Chat(2);
  ASSERT_EQ(service().streamer_count(), 2u);
  host_->Cancel(2);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(second.get()));
  EXPECT_EQ(second->outcome().kind, Kind::kCancelled);
  // The service sees the streamer close (that cancels it there).
  service().streamer(1).FlushForTesting();
  EXPECT_FALSE(service().streamer(1).is_connected());
  EXPECT_FALSE(Finished(first.get()));
  EXPECT_TRUE(delegate_.lost.empty());
}

TEST_F(InferenceProcessHostTest, BrowserSideStopSequenceEndsTheRequest) {
  Load();
  StreamLimits limits;
  limits.stop_sequences = {"STOP"};
  scoped_refptr<InferenceMailbox> gen = Chat(1, limits);
  service().streamer(0)->OnChunk("abc ST");
  service().streamer(0)->OnChunk("OP injected");
  task_environment_.RunUntilIdle();
  std::string text;
  ASSERT_TRUE(Finished(gen.get(), &text));
  EXPECT_EQ(text, "abc ");
  EXPECT_EQ(gen->outcome().kind, Kind::kOk);
  EXPECT_TRUE(gen->outcome().stopped_by_browser);
  service().streamer(0).FlushForTesting();
  EXPECT_FALSE(service().streamer(0).is_connected());
}

TEST_F(InferenceProcessHostTest, LoadFailureAndLoadHang) {
  next_load_behavior_ = FakeService::LoadBehavior::kFail;
  scoped_refptr<InferenceMailbox> failed = Load();
  ASSERT_TRUE(Finished(failed.get()));
  EXPECT_EQ(failed->outcome().kind, Kind::kLoadFailed);
  EXPECT_EQ(failed->outcome().load_status,
            mojom::LoadModelStatus::kInvalidModelFile);
  EXPECT_TRUE(delegate_.lost.empty());  // a clean failure isn't a crash

  next_load_behavior_ = FakeService::LoadBehavior::kHang;
  scoped_refptr<InferenceMailbox> hung = Load();
  task_environment_.FastForwardBy(base::Minutes(9));
  EXPECT_FALSE(Finished(hung.get()));
  task_environment_.FastForwardBy(base::Minutes(2));
  ASSERT_TRUE(Finished(hung.get()));
  EXPECT_EQ(hung->outcome().kind, Kind::kTimedOut);
  ASSERT_EQ(delegate_.lost.size(), 1u);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kLoading);
}

TEST_F(InferenceProcessHostTest, BadDescriptionIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kBadDescription;
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
}

TEST_F(InferenceProcessHostTest, GracefulUnloadWaitsForRunningRequests) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  auto exited = base::MakeRefCounted<InferenceMailbox>();
  host_->Unload(InferenceProcessHost::UnloadMode::kGraceful, exited);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
  EXPECT_FALSE(Finished(exited.get()));
  // New requests are refused meanwhile.
  scoped_refptr<InferenceMailbox> late = Chat(2);
  ASSERT_TRUE(Finished(late.get()));
  EXPECT_EQ(late->outcome().kind, Kind::kNoModel);
  // The running one completes, then the process ends (no real process
  // here: the exit wait times out after the grace periods).
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kOk);
  task_environment_.FastForwardBy(base::Seconds(11));
  EXPECT_TRUE(Finished(exited.get()));
  EXPECT_EQ(host_->ProcessCountForTesting(), 0u);
}

TEST_F(InferenceProcessHostTest, SwapKeepsTheOldModelServingUntilLoaded) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  scoped_refptr<InferenceMailbox> second_load = Load();
  ASSERT_TRUE(Finished(second_load.get()));
  EXPECT_EQ(services_.size(), 2u);
  // The first process still streams its request.
  services_[0]->streamer(0)->OnChunk("old");
  services_[0]->streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kOk);
  // New requests go to the new process.
  Chat(2);
  EXPECT_EQ(services_[1]->streamer_count(), 1u);
}

TEST_F(InferenceProcessHostTest, ShutdownFailsEverything) {
  Load();
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  host_->Shutdown();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kShuttingDown);
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kShuttingDown);
}

}  // namespace
}  // namespace molt_ai
