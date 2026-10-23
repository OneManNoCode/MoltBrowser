// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// InferenceProcessHost against a fake, in-process MoltInferenceService that
// misbehaves on request: crash mid-stream, hang, flood, bad UTF-8, a bad
// ModelDescription, vision claimed without a projector, cancel while queued,
// browser-side stop sequences.

#include "chrome/browser/molt_ai/runtime/inference_process_host.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/services/molt_ai/inference/engine/gpu_pipeline_cache.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/base/big_buffer.h"
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
  enum class LoadBehavior {
    kOk,
    kFail,
    kHang,
    kBadDescription,
    kVision,
    // On the GPU (Vulkan), 33 layers.
    kGpu,
  };

  void Bind(mojo::PendingReceiver<mojom::MoltInferenceService> receiver) {
    service_receiver_.Bind(std::move(receiver));
  }

  // mojom::MoltInferenceService:
  void LoadModel(mojom::ModelLoadParamsPtr params,
                 mojo::PendingReceiver<mojom::Model> model,
                 LoadModelCallback callback) override {
    load_params_ = std::move(params);
    model_receiver_.Bind(std::move(model));
    pending_load_ = std::move(callback);
    // A GPU load asks the browser about the device first, as the real
    // service does (design section 11.10).
    if (load_params_->use_gpu && load_params_->gpu_client && report_device) {
      gpu_client_.Bind(std::move(load_params_->gpu_client));
      gpu_client_->OnGpuChosen(
          identity.Clone(),
          base::BindOnce(&FakeService::OnGpuAnswer, base::Unretained(this)));
      return;
    }
    FinishLoad();
  }

  // The GPU device a GPU load reports.
  static mojom::GpuDeviceIdentityPtr Identity() {
    auto key = mojom::GpuDriverKey::New(0x1002, 0x73ff, 3, 0x01802000,
                                        std::vector<uint8_t>(16, 0x11));
    return mojom::GpuDeviceIdentity::New(
        std::move(key), (1u << 22) | (3u << 12),
        std::vector<uint8_t>(16, 0xab), "AMD Radeon <b>RX</b> 6600");
  }

  void OnGpuAnswer(bool use_gpu, base::File cache) {
    gpu_answered = true;
    gpu_allowed = use_gpu;
    gpu_cache = std::move(cache);
    if (answer_twice) {
      gpu_client_->OnGpuChosen(identity.Clone(), base::DoNothing());
      return;
    }
    if (load_behavior != LoadBehavior::kHang) FinishLoad();
  }

  // The status a well-behaved service reports for a load on the CPU.
  mojom::GpuStatus HonestCpuStatus() const {
    if (!load_params_->use_gpu) return mojom::GpuStatus::kNotRequested;
    if (!gpu_answered) return mojom::GpuStatus::kNoDevice;
    return gpu_allowed ? mojom::GpuStatus::kLoadFailed
                       : mojom::GpuStatus::kTurnedOff;
  }

  void FinishLoad() {
    if (load_behavior == LoadBehavior::kHang) return;
    LoadModelCallback callback = std::move(pending_load_);
    switch (load_behavior) {
      case LoadBehavior::kOk:
      case LoadBehavior::kVision: {
        auto d = mojom::ModelDescription::New();
        d->architecture = "llama";
        d->backend = "cpu";
        d->context_tokens = 4096;
        d->train_context_tokens = 4096;
        d->vocab_size = 32000;
        d->vision = load_behavior == LoadBehavior::kVision;
        d->gpu_status = status_override.value_or(HonestCpuStatus());
        std::move(callback).Run(mojom::LoadModelStatus::kOk, std::move(d));
        break;
      }
      case LoadBehavior::kGpu: {
        // On the GPU when the browser allowed it (or, lying, when it
        // wasn't asked); the CPU otherwise.
        const bool on_gpu =
            gpu_allowed || !load_params_->use_gpu || !report_device;
        auto d = mojom::ModelDescription::New();
        d->architecture = "llama";
        d->backend = on_gpu ? "vulkan" : "cpu";
        d->context_tokens = 4096;
        d->train_context_tokens = 4096;
        d->vocab_size = 32000;
        d->gpu_status = status_override.value_or(
            on_gpu ? mojom::GpuStatus::kOn : HonestCpuStatus());
        d->gpu_layers = on_gpu ? 33 : 0;
        std::move(callback).Run(mojom::LoadModelStatus::kOk, std::move(d));
        break;
      }
      case LoadBehavior::kFail:
        std::move(callback).Run(mojom::LoadModelStatus::kInvalidModelFile,
                                nullptr);
        break;
      case LoadBehavior::kHang:
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
  // Embedding models have their own host (embedding_process_host_unittest).
  void LoadEmbeddingModel(mojom::EmbeddingModelLoadParamsPtr params,
                          mojo::PendingReceiver<mojom::EmbeddingModel> model,
                          LoadEmbeddingModelCallback callback) override {}

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
  // Answers from |pipeline_caches| (front first), null when it is empty.
  void GetPipelineCache(GetPipelineCacheCallback callback) override {
    ++pipeline_cache_requests;
    if (pipeline_caches.empty()) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    std::optional<mojo_base::BigBuffer> cache =
        std::move(pipeline_caches.front());
    pipeline_caches.erase(pipeline_caches.begin());
    std::move(callback).Run(std::move(cache));
  }

  // Simulates the process dying: every pipe closes.
  void Crash() {
    streamers_.clear();
    model_receiver_.reset();
    service_receiver_.reset();
    gpu_client_.reset();
  }

  mojo::Remote<mojom::GenerationStreamer>& streamer(size_t i) {
    return streamers_[i];
  }
  size_t streamer_count() const { return streamers_.size(); }
  bool model_connected() const { return model_receiver_.is_bound(); }
  const mojom::ModelLoadParamsPtr& load_params() const { return load_params_; }

  LoadBehavior load_behavior = LoadBehavior::kOk;
  // GPU loads: report |identity| before loading (a well-behaved service).
  bool report_device = true;
  mojom::GpuDeviceIdentityPtr identity = Identity();
  // Call OnGpuChosen again after the answer (a protocol violation).
  bool answer_twice = false;
  // Report this GpuStatus instead of the honest one.
  std::optional<mojom::GpuStatus> status_override;
  // The browser's answer.
  bool gpu_answered = false;
  bool gpu_allowed = false;
  base::File gpu_cache;
  // GetPipelineCache() answers, and how often it was asked.
  std::vector<std::optional<mojo_base::BigBuffer>> pipeline_caches;
  int pipeline_cache_requests = 0;

 private:
  mojo::Remote<mojom::GpuLoadClient> gpu_client_;
  mojo::Receiver<mojom::MoltInferenceService> service_receiver_{this};
  mojo::Receiver<mojom::Model> model_receiver_{this};
  mojom::ModelLoadParamsPtr load_params_;
  LoadModelCallback pending_load_;
  std::vector<mojo::Remote<mojom::GenerationStreamer>> streamers_;
};

class RecordingDelegate : public InferenceProcessHost::Delegate {
 public:
  void OnModelProcessLost(
      const std::string& model_id,
      const std::string& crash_key,
      CrashLoopGuard::Phase phase,
      bool gpu_failure,
      const std::optional<GpuDeviceReport>& gpu_device) override {
    lost.push_back(phase);
    gpu_failures.push_back(gpu_failure);
    lost_devices.push_back(gpu_device);
  }
  void OnGpuDeviceChosen(
      const GpuDeviceReport& device,
      base::OnceCallback<void(bool, base::File)> reply) override {
    chosen.push_back(device);
    base::File file;
    if (cache_path) {
      file = base::File(*cache_path,
                        base::File::FLAG_OPEN | base::File::FLAG_READ);
    }
    std::move(reply).Run(allow_gpu, std::move(file));
  }
  void OnPipelineCacheData(const GpuDeviceReport& device,
                           mojo_base::BigBuffer cache) override {
    stored_caches.push_back(cache.size());
  }
  void OnPipelineCacheDropped(const GpuDeviceReport& device) override {
    ++dropped_caches;
  }

  std::vector<CrashLoopGuard::Phase> lost;
  std::vector<bool> gpu_failures;
  std::vector<std::optional<GpuDeviceReport>> lost_devices;
  // OnGpuDeviceChosen: what was reported, and the answer.
  std::vector<GpuDeviceReport> chosen;
  bool allow_gpu = true;
  std::optional<base::FilePath> cache_path;
  std::vector<size_t> stored_caches;
  int dropped_caches = 0;
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
           base::OnceCallback<void(const base::Process&)> on_process,
           bool use_gpu) {
          self->launched_for_gpu_.push_back(use_gpu);
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

  scoped_refptr<InferenceMailbox> Load(bool with_projector = false,
                                       bool use_gpu = false) {
    InferenceProcessHost::LoadRequest r;
    r.model_id = "m";
    r.crash_key = "m:1:2";
    r.file_size = 1;
    r.max_context_tokens = 32768;
    r.use_gpu = use_gpu;
    if (with_projector) {
      EXPECT_TRUE(temp_dir_.IsValid() || temp_dir_.CreateUniqueTempDir());
      const base::FilePath path =
          temp_dir_.GetPath().AppendASCII("mmproj.gguf");
      EXPECT_TRUE(base::WriteFile(path, "GGUF-mmproj"));
      r.vision_projector =
          base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
      EXPECT_TRUE(r.vision_projector.IsValid());
      r.vision_projector_size = 11;
    }
    auto mailbox = base::MakeRefCounted<InferenceMailbox>();
    host_->Load(std::move(r), mailbox);
    task_environment_.RunUntilIdle();
    return mailbox;
  }

  // Starts a GPU load: the fake service exists (service()) but hasn't seen
  // LoadModel yet, so a test can script it before RunUntilIdle().
  scoped_refptr<InferenceMailbox> StartGpuLoad() {
    InferenceProcessHost::LoadRequest r;
    r.model_id = "m";
    r.crash_key = "m:1:2";
    r.file_size = 1;
    r.max_context_tokens = 32768;
    r.use_gpu = true;
    auto mailbox = base::MakeRefCounted<InferenceMailbox>();
    host_->Load(std::move(r), mailbox);
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
  base::ScopedTempDir temp_dir_;
  std::vector<std::string> bad_messages_;
  FakeService::LoadBehavior next_load_behavior_ = FakeService::LoadBehavior::kOk;
  std::vector<bool> launched_for_gpu_;
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

TEST_F(InferenceProcessHostTest, QueuedRequestIsNotKilledWhileItWaits) {
  Load();
  scoped_refptr<InferenceMailbox> first = Chat(1);
  scoped_refptr<InferenceMailbox> second = Chat(2);
  // The first streams for 10 minutes; the second waits silently.
  for (int i = 0; i < 10; ++i) {
    task_environment_.FastForwardBy(base::Seconds(60));
    service().streamer(0)->OnChunk("x");
    task_environment_.RunUntilIdle();
  }
  EXPECT_FALSE(Finished(second.get()));
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(first.get()));
  // Now the second runs: its liveness clock starts.
  task_environment_.FastForwardBy(base::Seconds(100));
  EXPECT_FALSE(Finished(second.get()));
  task_environment_.FastForwardBy(base::Seconds(25));
  ASSERT_TRUE(Finished(second.get()));
  EXPECT_EQ(second->outcome().kind, Kind::kTimedOut);
}

// The service answers a request whose output constraint it refuses at
// once, ahead of the one running: that one keeps running, unharmed.
TEST_F(InferenceProcessHostTest, RequestAnsweredBeforeItsTurn) {
  Load();
  scoped_refptr<InferenceMailbox> first = Chat(1);
  scoped_refptr<InferenceMailbox> second = Chat(2);
  auto refused = mojom::GenerationSummary::New();
  refused->status = mojom::GenerationStatus::kConstraintUnsupported;
  service().streamer(1)->OnComplete(std::move(refused));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(second.get()));
  EXPECT_EQ(second->outcome().kind, Kind::kOk);
  ASSERT_TRUE(second->outcome().has_summary);
  EXPECT_EQ(
      second->outcome().summary.status,
      static_cast<int32_t>(mojom::GenerationStatus::kConstraintUnsupported));
  EXPECT_FALSE(Finished(first.get()));
  service().streamer(0)->OnChunk("x");
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  std::string text;
  ASSERT_TRUE(Finished(first.get(), &text));
  EXPECT_EQ(text, "x");
  EXPECT_EQ(first->outcome().kind, Kind::kOk);
  EXPECT_TRUE(bad_messages_.empty());
  EXPECT_TRUE(delegate_.lost.empty());
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
  EXPECT_FALSE(delegate_.gpu_failures[0]);  // the GPU wasn't asked for
}

// GPU inference (docs/design/SANDBOXED_INFERENCE.md section 11).

TEST_F(InferenceProcessHostTest, GpuLoadIsLaunchedAndDescribedAsSuch) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  scoped_refptr<InferenceMailbox> load = Load(/*with_projector=*/false,
                                              /*use_gpu=*/true);
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_EQ(load->outcome().backend, "vulkan");
  EXPECT_EQ(load->outcome().gpu_status, mojom::GpuStatus::kOn);
  EXPECT_EQ(load->outcome().gpu_layers, 33u);
  // Launched with the GPU switch, and asked for the GPU.
  ASSERT_EQ(launched_for_gpu_.size(), 1u);
  EXPECT_TRUE(launched_for_gpu_[0]);
  EXPECT_TRUE(service().load_params()->use_gpu);
  // A CPU load is launched without it.
  next_load_behavior_ = FakeService::LoadBehavior::kOk;
  Load();
  ASSERT_EQ(launched_for_gpu_.size(), 2u);
  EXPECT_FALSE(launched_for_gpu_[1]);
  EXPECT_FALSE(service().load_params()->use_gpu);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, GpuDescriptionWithoutTheRequestIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  scoped_refptr<InferenceMailbox> load = Load();  // not asked for the GPU
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_EQ(bad_messages_.size(), 1u);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_FALSE(delegate_.gpu_failures[0]);  // a lie isn't a GPU failure
}

TEST_F(InferenceProcessHostTest, CrashOnTheGpuIsAGpuFailure) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kProcessGone);
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_TRUE(delegate_.gpu_failures[0]);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kGenerating);
}

TEST_F(InferenceProcessHostTest, IdleDeathOnTheGpuIsNotAGpuFailure) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  service().Crash();  // nothing running
  task_environment_.RunUntilIdle();
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_FALSE(delegate_.gpu_failures[0]);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kIdle);
}

TEST_F(InferenceProcessHostTest, CrashAfterTheCpuFallbackIsAModelCrash) {
  // Asked for the GPU, but the service loaded the model on the CPU.
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_FALSE(delegate_.gpu_failures[0]);
}

TEST_F(InferenceProcessHostTest, GpuFailureOnTheGpuEndsTheRequest) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  mojom::GenerationSummaryPtr s = Summary();
  s->status = mojom::GenerationStatus::kBackendFailed;
  service().streamer(0)->OnComplete(std::move(s));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  // The runtime records the GPU failure and ends the process.
  EXPECT_EQ(gen->outcome().kind, Kind::kOk);
  ASSERT_TRUE(gen->outcome().has_summary);
  EXPECT_TRUE(gen->outcome().summary.backend_failed);
  EXPECT_TRUE(bad_messages_.empty());
  EXPECT_TRUE(delegate_.lost.empty());
}

TEST_F(InferenceProcessHostTest, GpuFailureOffTheGpuIsAModelCrash) {
  // Asked for the GPU, but the service loaded the model on the CPU, where
  // llama.cpp doesn't throw: a reported GPU failure is the model's crash
  // (or a lie), counted by CrashLoopGuard, not GpuFallbackGuard.
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  mojom::GenerationSummaryPtr s = Summary();
  s->status = mojom::GenerationStatus::kBackendFailed;
  service().streamer(0)->OnComplete(std::move(s));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(gen.get()));
  EXPECT_EQ(gen->outcome().kind, Kind::kViolation);
  EXPECT_EQ(bad_messages_.size(), 1u);
  ASSERT_EQ(delegate_.lost.size(), 1u);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kGenerating);
  EXPECT_FALSE(delegate_.gpu_failures[0]);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
}

TEST_F(InferenceProcessHostTest, HangLoadingForTheGpuIsAGpuFailure) {
  next_load_behavior_ = FakeService::LoadBehavior::kHang;
  scoped_refptr<InferenceMailbox> hung =
      Load(/*with_projector=*/false, /*use_gpu=*/true);
  task_environment_.FastForwardBy(base::Minutes(11));
  ASSERT_TRUE(Finished(hung.get()));
  EXPECT_EQ(hung->outcome().kind, Kind::kTimedOut);
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_TRUE(delegate_.gpu_failures[0]);
  EXPECT_EQ(delegate_.lost[0], CrashLoopGuard::Phase::kLoading);
}

// The lasting GPU record (section 11.10): the service reports the device
// before it loads anything on it, and the browser answers.

TEST_F(InferenceProcessHostTest, GpuDeviceIsReportedBeforeTheLoad) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  InferenceProcessHost::LoadRequest r;
  r.model_id = "m";
  r.crash_key = "m:1:2";
  r.file_size = 1;
  r.max_context_tokens = 32768;
  r.use_gpu = true;
  GpuDriverKey off;
  off.identified = true;
  off.vendor_id = 0x10de;
  r.skip_gpu_drivers = {off, UnidentifiedGpuDriverKey()};
  auto load = base::MakeRefCounted<InferenceMailbox>();
  host_->Load(std::move(r), load);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  // The skip list went with the load (identified keys only).
  ASSERT_EQ(service().load_params()->skip_gpu_drivers.size(), 1u);
  EXPECT_EQ(service().load_params()->skip_gpu_drivers[0]->vendor_id, 0x10deu);
  // The delegate saw the device, its name sanitized.
  ASSERT_EQ(delegate_.chosen.size(), 1u);
  const GpuDeviceReport& device = delegate_.chosen[0];
  EXPECT_TRUE(device.key.identified);
  EXPECT_EQ(device.key.vendor_id, 0x1002u);
  EXPECT_EQ(device.key.driver_version, 0x01802000u);
  EXPECT_EQ(device.key.driver_uuid[15], 0x11);
  EXPECT_EQ(device.cache_device.uuid[0], 0xab);
  EXPECT_EQ(device.name, "AMD Radeon bRX/b 6600");
  // ... and the load's outcome carries it.
  ASSERT_TRUE(load->outcome().gpu_device);
  EXPECT_EQ(load->outcome().gpu_device->key, device.key);
  EXPECT_TRUE(service().gpu_allowed);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, RefusedGpuLoadsOnTheCpu) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  delegate_.allow_gpu = false;
  scoped_refptr<InferenceMailbox> load =
      Load(/*with_projector=*/false, /*use_gpu=*/true);
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_EQ(load->outcome().gpu_status, mojom::GpuStatus::kTurnedOff);
  EXPECT_EQ(load->outcome().backend, "cpu");
  EXPECT_FALSE(service().gpu_allowed);
  EXPECT_FALSE(service().gpu_cache.IsValid());
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, CrashLoadingAfterARefusalIsAModelCrash) {
  next_load_behavior_ = FakeService::LoadBehavior::kHang;
  delegate_.allow_gpu = false;
  scoped_refptr<InferenceMailbox> load =
      Load(/*with_projector=*/false, /*use_gpu=*/true);
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  // The CPU was loading: CrashLoopGuard counts it.
  EXPECT_FALSE(delegate_.gpu_failures[0]);
}

TEST_F(InferenceProcessHostTest, GpuFailureCarriesTheReportedDevice) {
  // Crashes while loading on the reported device: a GPU failure on it.
  next_load_behavior_ = FakeService::LoadBehavior::kHang;
  StartGpuLoad();
  task_environment_.RunUntilIdle();
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(delegate_.gpu_failures.size(), 1u);
  EXPECT_TRUE(delegate_.gpu_failures[0]);
  ASSERT_TRUE(delegate_.lost_devices[0]);
  EXPECT_EQ(delegate_.lost_devices[0]->key.vendor_id, 0x1002u);
  // Crashes before it reported one (while listing devices): a GPU failure
  // the runtime records against an unidentified driver.
  StartGpuLoad();
  service().report_device = false;
  task_environment_.RunUntilIdle();
  service().Crash();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(delegate_.gpu_failures.size(), 2u);
  EXPECT_TRUE(delegate_.gpu_failures[1]);
  EXPECT_FALSE(delegate_.lost_devices[1]);
}

TEST_F(InferenceProcessHostTest, GpuStatusMustFitWhatTheBrowserSaw) {
  // On the GPU without having reported a device.
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  scoped_refptr<InferenceMailbox> load = StartGpuLoad();
  service().report_device = false;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);

  // After the browser said no, a device lost "on it" is a lie.
  next_load_behavior_ = FakeService::LoadBehavior::kOk;
  delegate_.allow_gpu = false;
  load = StartGpuLoad();
  service().status_override = mojom::GpuStatus::kDeviceLost;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);

  // So is "turned off" after it said yes.
  delegate_.allow_gpu = true;
  load = StartGpuLoad();
  service().status_override = mojom::GpuStatus::kTurnedOff;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_EQ(bad_messages_.size(), 3u);

  // A device lost while loading on an allowed device is a fine outcome,
  // and it says which device.
  load = StartGpuLoad();
  service().status_override = mojom::GpuStatus::kDeviceLost;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_EQ(load->outcome().gpu_status, mojom::GpuStatus::kDeviceLost);
  ASSERT_TRUE(load->outcome().gpu_device);
  EXPECT_EQ(load->outcome().gpu_device->key.device_id, 0x73ffu);
  EXPECT_EQ(bad_messages_.size(), 3u);
}

TEST_F(InferenceProcessHostTest, SecondDeviceReportIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kHang;
  scoped_refptr<InferenceMailbox> load = StartGpuLoad();
  service().answer_twice = true;
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_EQ(bad_messages_.size(), 1u);
  EXPECT_EQ(delegate_.chosen.size(), 1u);
}

TEST_F(InferenceProcessHostTest, OverlongGpuNameIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  scoped_refptr<InferenceMailbox> load = StartGpuLoad();
  service().identity->name = std::string(256, 'x');
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_TRUE(delegate_.chosen.empty());
}

// The pipeline cache (section 11.12).

// A version-one cache for FakeService::Identity()'s device.
mojo_base::BigBuffer CacheFor(uint32_t vendor, size_t size = 64) {
  std::vector<uint8_t> bytes(size, 0x5a);
  auto put = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes[at + i] = (v >> (8 * i)) & 0xff;
  };
  put(0, 32);
  put(4, 1);
  put(8, vendor);
  put(12, 0x73ff);
  for (size_t i = 16; i < 32; ++i) bytes[i] = 0xab;
  return mojo_base::BigBuffer(bytes);
}

TEST_F(InferenceProcessHostTest, PipelineCacheAskedAfterAGpuGeneration) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  FakeService& s = service();
  s.pipeline_caches.push_back(CacheFor(0x1002));
  s.pipeline_caches.push_back(std::nullopt);  // nothing new
  for (uint64_t token = 1; token <= 6; ++token) {
    scoped_refptr<InferenceMailbox> gen = Chat(token);
    s.streamer(token - 1)->OnComplete(Summary());
    task_environment_.RunUntilIdle();
    ASSERT_TRUE(Finished(gen.get()));
  }
  // At most kMaxPipelineCacheRequests asks; the valid one was stored.
  EXPECT_EQ(s.pipeline_cache_requests, kMaxPipelineCacheRequests);
  EXPECT_EQ(delegate_.stored_caches, (std::vector<size_t>{64}));
  EXPECT_EQ(delegate_.dropped_caches, 0);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, BadPipelineCachesAreRefused) {
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  // Another device's header: refused, the stored copy dropped, no more
  // asks.
  service().pipeline_caches.push_back(CacheFor(0x10de));
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  gen = Chat(2);
  service().streamer(1)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(service().pipeline_cache_requests, 1);
  EXPECT_TRUE(delegate_.stored_caches.empty());
  EXPECT_EQ(delegate_.dropped_caches, 1);

  // Over the cap.
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  service().pipeline_caches.push_back(
      CacheFor(0x1002, kMaxPipelineCacheBytes + 1));
  gen = Chat(3);
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(delegate_.stored_caches.empty());
  EXPECT_EQ(delegate_.dropped_caches, 2);

  // Empty: the service's grew over the cap; dropped, asked again later.
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  service().pipeline_caches.push_back(mojo_base::BigBuffer());
  gen = Chat(4);
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate_.dropped_caches, 3);
  EXPECT_TRUE(bad_messages_.empty());  // not protocol violations
}

TEST_F(InferenceProcessHostTest, NoPipelineCacheOffTheGpu) {
  // On the CPU (a GPU load that fell back), and after a GPU failure.
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  scoped_refptr<InferenceMailbox> gen = Chat(1);
  service().streamer(0)->OnComplete(Summary());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(service().pipeline_cache_requests, 0);
  next_load_behavior_ = FakeService::LoadBehavior::kGpu;
  Load(/*with_projector=*/false, /*use_gpu=*/true);
  gen = Chat(2);
  mojom::GenerationSummaryPtr failed = Summary();
  failed->status = mojom::GenerationStatus::kBackendFailed;
  service().streamer(0)->OnComplete(std::move(failed));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(service().pipeline_cache_requests, 0);
}

TEST_F(InferenceProcessHostTest, BadDescriptionIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kBadDescription;
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
}

TEST_F(InferenceProcessHostTest, VisionNeedsTheProjectorTheBrowserSent) {
  // Sent a projector and the service says it sees images: recorded.
  next_load_behavior_ = FakeService::LoadBehavior::kVision;
  scoped_refptr<InferenceMailbox> load = Load(/*with_projector=*/true);
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_TRUE(load->outcome().vision);
  ASSERT_TRUE(service().load_params());
  EXPECT_TRUE(service().load_params()->vision_projector.IsValid());
  EXPECT_EQ(service().load_params()->vision_projector_size, 11u);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(InferenceProcessHostTest, TextModelSentNoProjector) {
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_FALSE(load->outcome().vision);
  ASSERT_TRUE(service().load_params());
  EXPECT_FALSE(service().load_params()->vision_projector.IsValid());
  EXPECT_EQ(service().load_params()->vision_projector_size, 0u);
}

TEST_F(InferenceProcessHostTest, ProjectorTheServiceDidNotUseIsNoVision) {
  scoped_refptr<InferenceMailbox> load = Load(/*with_projector=*/true);
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kOk);
  EXPECT_FALSE(load->outcome().vision);
}

TEST_F(InferenceProcessHostTest, VisionWithoutAProjectorIsAViolation) {
  next_load_behavior_ = FakeService::LoadBehavior::kVision;
  scoped_refptr<InferenceMailbox> load = Load();
  ASSERT_TRUE(Finished(load.get()));
  EXPECT_EQ(load->outcome().kind, Kind::kViolation);
  EXPECT_FALSE(load->outcome().vision);
  EXPECT_FALSE(host_->HasCurrentModelForTesting());
  EXPECT_EQ(bad_messages_.size(), 1u);
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
