// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// EmbeddingProcessHost against a fake, in-process MoltInferenceService +
// EmbeddingModel that misbehaves on request: wrong counts, unnormalized or
// non-finite vectors, a lying description, crashes, hangs.

#include "chrome/browser/molt_ai/runtime/embedding_process_host.h"

#include <cmath>
#include <limits>
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
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/system/functions.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr uint32_t kDims = 4;

class FakeService : public mojom::MoltInferenceService,
                    public mojom::EmbeddingModel {
 public:
  enum class Reply {
    kGood,
    kNull,
    kWrongCount,
    kUnnormalized,
    kNaN,
    kHang,
  };

  void Bind(mojo::PendingReceiver<mojom::MoltInferenceService> receiver) {
    service_receiver_.Bind(std::move(receiver));
  }

  // mojom::MoltInferenceService:
  void LoadModel(mojom::ModelLoadParamsPtr params,
                 mojo::PendingReceiver<mojom::Model> model,
                 LoadModelCallback callback) override {
    service_receiver_.ReportBadMessage("not a chat model process");
  }
  void LoadEmbeddingModel(mojom::EmbeddingModelLoadParamsPtr params,
                          mojo::PendingReceiver<mojom::EmbeddingModel> model,
                          LoadEmbeddingModelCallback callback) override {
    load_params = std::move(params);
    model_receiver_.Bind(std::move(model));
    if (load_status != mojom::LoadModelStatus::kOk) {
      std::move(callback).Run(load_status, nullptr);
      return;
    }
    auto d = mojom::EmbeddingModelDescription::New();
    d->architecture = architecture;
    d->backend = "cpu";
    d->native_dimensions = 16;
    d->dimensions = load_params->dimensions;
    d->max_tokens_per_text = load_params->max_tokens_per_text;
    d->vocab_size = 1000;
    std::move(callback).Run(mojom::LoadModelStatus::kOk, std::move(d));
  }

  // mojom::EmbeddingModel:
  void Embed(const std::vector<std::string>& texts,
             EmbedCallback callback) override {
    received.push_back(texts);
    const size_t dims = load_params->dimensions;
    std::vector<float> values;
    for (size_t i = 0; i < texts.size(); ++i) {
      std::vector<float> row(dims, 0.0f);
      row[i % dims] = 1.0f;
      values.insert(values.end(), row.begin(), row.end());
    }
    switch (reply) {
      case Reply::kGood:
        break;
      case Reply::kNull:
        std::move(callback).Run(std::nullopt);
        return;
      case Reply::kWrongCount:
        values.pop_back();
        break;
      case Reply::kUnnormalized:
        values[0] = 3.0f;
        break;
      case Reply::kNaN:
        values[0] = std::numeric_limits<float>::quiet_NaN();
        break;
      case Reply::kHang:
        pending = std::move(callback);
        return;
    }
    std::move(callback).Run(values);
  }

  // Simulates the process dying: every pipe closes.
  void Crash() {
    model_receiver_.reset();
    service_receiver_.reset();
  }

  mojom::LoadModelStatus load_status = mojom::LoadModelStatus::kOk;
  std::string architecture = "qwen3";
  Reply reply = Reply::kGood;
  mojom::EmbeddingModelLoadParamsPtr load_params;
  std::vector<std::vector<std::string>> received;
  EmbedCallback pending;

 private:
  mojo::Receiver<mojom::MoltInferenceService> service_receiver_{this};
  mojo::Receiver<mojom::EmbeddingModel> model_receiver_{this};
};

struct Result {
  bool done = false;
  EmbeddingStatus status = EmbeddingStatus::kFailed;
  std::vector<std::vector<float>> vectors;
};

class EmbeddingProcessHostTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    static bool mojo_initialized = false;
    if (!mojo_initialized) {
      mojo::core::Init();
      mojo_initialized = true;
    }
  }

  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    model_path_ = temp_dir_.GetPath().AppendASCII("model.gguf");
    ASSERT_TRUE(base::WriteFile(model_path_, "GGUF-test"));
    mojo::SetDefaultProcessErrorHandler(base::BindRepeating(
        [](std::vector<std::string>* out, const std::string& error) {
          out->push_back(error);
        },
        base::Unretained(&bad_messages_)));
    MakeHost();
  }

  // A fresh host (no crash history) with the fake opener and launcher.
  void MakeHost() {
    host_ = std::make_unique<EmbeddingProcessHost>();
    host_->SetOpenerForTesting(base::BindRepeating(
        [](EmbeddingProcessHostTest* self, const EmbeddingModelConfig& c) {
          EmbeddingProcessHost::OpenedModel opened;
          if (self->file_ok_) {
            opened.file = base::File(c.path, base::File::FLAG_OPEN |
                                                 base::File::FLAG_READ);
          }
          opened.crash_key = "m:9:1";
          opened.enough_memory = self->enough_memory_;
          return opened;
        },
        base::Unretained(this)));
    host_->SetLauncherForTesting(base::BindRepeating(
        [](EmbeddingProcessHostTest* self,
           mojo::PendingReceiver<mojom::MoltInferenceService> receiver,
           base::WeakPtr<content::ServiceProcessHost::Observer> observer,
           base::OnceCallback<void(const base::Process&)> on_process) {
          auto service = std::make_unique<FakeService>();
          service->load_status = self->next_load_status_;
          service->architecture = self->next_architecture_;
          service->Bind(std::move(receiver));
          self->services_.push_back(std::move(service));
        },
        base::Unretained(this)));
  }

  void TearDown() override {
    host_.reset();
    mojo::SetDefaultProcessErrorHandler(base::NullCallback());
  }

  EmbeddingModelConfig Config() const {
    EmbeddingModelConfig c;
    c.model_id = "m";
    c.path = model_path_;
    c.file_size = 9;
    c.pooling = mojom::EmbeddingPooling::kLast;
    c.max_tokens_per_text = 512;
    c.terminator = "<|endoftext|>";
    c.dimensions = kDims;
    c.native_dimensions = 16;
    c.architecture = "qwen3";
    return c;
  }

  Result* Embed(std::vector<std::string> texts,
                std::optional<EmbeddingModelConfig> config = std::nullopt,
                bool urgent = false) {
    results_.push_back(std::make_unique<Result>());
    Result* r = results_.back().get();
    host_->Embed(config ? *config : Config(), std::move(texts),
                 base::BindOnce(
                     [](Result* r, EmbeddingStatus status,
                        std::vector<std::vector<float>> vectors) {
                       r->done = true;
                       r->status = status;
                       r->vectors = std::move(vectors);
                     },
                     base::Unretained(r)),
                 urgent);
    task_environment_.RunUntilIdle();
    return r;
  }

  FakeService& service() { return *services_.back(); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_dir_;
  base::FilePath model_path_;
  bool file_ok_ = true;
  bool enough_memory_ = true;
  mojom::LoadModelStatus next_load_status_ = mojom::LoadModelStatus::kOk;
  std::string next_architecture_ = "qwen3";
  std::vector<std::string> bad_messages_;
  std::vector<std::unique_ptr<FakeService>> services_;
  std::vector<std::unique_ptr<Result>> results_;
  std::unique_ptr<EmbeddingProcessHost> host_;
};

TEST_F(EmbeddingProcessHostTest, LoadsAndEmbeds) {
  Result* r = Embed({"a", "b"});
  ASSERT_TRUE(r->done);
  EXPECT_EQ(r->status, EmbeddingStatus::kOk);
  ASSERT_EQ(r->vectors.size(), 2u);
  EXPECT_EQ(r->vectors[0], (std::vector<float>{1, 0, 0, 0}));
  EXPECT_EQ(r->vectors[1], (std::vector<float>{0, 1, 0, 0}));
  ASSERT_EQ(services_.size(), 1u);
  const mojom::EmbeddingModelLoadParams& p = *service().load_params;
  EXPECT_EQ(p.file_size, 9u);
  EXPECT_EQ(p.pooling, mojom::EmbeddingPooling::kLast);
  EXPECT_EQ(p.terminator, "<|endoftext|>");
  EXPECT_EQ(p.dimensions, kDims);
  EXPECT_GE(p.cpu_threads, 1u);
  EXPECT_TRUE(bad_messages_.empty());

  // The same process serves the next call.
  Result* again = Embed({"c"});
  EXPECT_EQ(again->status, EmbeddingStatus::kOk);
  EXPECT_EQ(services_.size(), 1u);
}

TEST_F(EmbeddingProcessHostTest, EmptyTextsAreZeroVectorsAndNeverSent) {
  Result* r = Embed({"", "x", ""});
  ASSERT_EQ(r->status, EmbeddingStatus::kOk);
  ASSERT_EQ(r->vectors.size(), 3u);
  EXPECT_EQ(r->vectors[0], std::vector<float>(kDims, 0.0f));
  EXPECT_EQ(r->vectors[1], (std::vector<float>{1, 0, 0, 0}));
  EXPECT_EQ(r->vectors[2], std::vector<float>(kDims, 0.0f));
  ASSERT_EQ(service().received.size(), 1u);
  EXPECT_EQ(service().received[0], std::vector<std::string>{"x"});

  // Nothing to send: no process at all.
  MakeHost();
  Result* none = Embed({"", ""});
  EXPECT_EQ(none->status, EmbeddingStatus::kOk);
  EXPECT_FALSE(host_->HasProcessForTesting());
}

TEST_F(EmbeddingProcessHostTest, TextsAreMadeValidAndCapped) {
  const std::string huge(mojom::kMaxEmbedTextBytes + 100, 'a');
  Result* r = Embed({std::string("a\xff\x00z", 4), huge});
  ASSERT_EQ(r->status, EmbeddingStatus::kOk);
  const std::vector<std::string>& sent = service().received[0];
  EXPECT_EQ(sent[0], "a\xEF\xBF\xBD\xEF\xBF\xBDz");
  EXPECT_EQ(sent[1].size(), mojom::kMaxEmbedTextBytes);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(EmbeddingProcessHostTest, TooManyTextsFail) {
  Result* r = Embed(std::vector<std::string>(mojom::kMaxEmbedTexts + 1, "x"));
  EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
  EXPECT_TRUE(services_.empty());
}

TEST_F(EmbeddingProcessHostTest, BadRepliesKillTheProcess) {
  for (FakeService::Reply bad :
       {FakeService::Reply::kWrongCount, FakeService::Reply::kUnnormalized,
        FakeService::Reply::kNaN}) {
    bad_messages_.clear();
    Result* ok = Embed({"warm up"});
    ASSERT_EQ(ok->status, EmbeddingStatus::kOk);
    service().reply = bad;
    Result* r = Embed({"a", "b"});
    EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
    EXPECT_EQ(bad_messages_.size(), 1u);
    EXPECT_FALSE(host_->HasProcessForTesting());
    // Each violation counts against the model; start afresh.
    MakeHost();
  }
}

// A search goes ahead of queued background work.
TEST_F(EmbeddingProcessHostTest, UrgentCallsGoFirst) {
  Embed({"warm up"});
  service().reply = FakeService::Reply::kHang;
  Result* running = Embed({"background 1"});
  service().reply = FakeService::Reply::kGood;
  Result* queued = Embed({"background 2"});
  Result* search = Embed({"query"}, std::nullopt, /*urgent=*/true);
  std::move(service().pending).Run(std::vector<float>{1, 0, 0, 0});
  task_environment_.RunUntilIdle();
  EXPECT_EQ(running->status, EmbeddingStatus::kOk);
  EXPECT_EQ(search->status, EmbeddingStatus::kOk);
  EXPECT_EQ(queued->status, EmbeddingStatus::kOk);
  ASSERT_EQ(service().received.size(), 4u);
  EXPECT_EQ(service().received[2], std::vector<std::string>{"query"});
  EXPECT_EQ(service().received[3], std::vector<std::string>{"background 2"});
}

TEST_F(EmbeddingProcessHostTest, ModelFailureKeepsTheProcess) {
  Embed({"warm up"});
  service().reply = FakeService::Reply::kNull;
  Result* r = Embed({"a"});
  EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
  EXPECT_TRUE(bad_messages_.empty());
  EXPECT_TRUE(host_->HasProcessForTesting());
}

TEST_F(EmbeddingProcessHostTest, WrongModelIsRefused) {
  next_architecture_ = "llama";
  Result* r = Embed({"a"});
  EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
  EXPECT_EQ(bad_messages_.size(), 1u);
  EXPECT_FALSE(host_->HasProcessForTesting());
}

TEST_F(EmbeddingProcessHostTest, LoadFailures) {
  next_load_status_ = mojom::LoadModelStatus::kOutOfMemory;
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kNotEnoughMemory);
  next_load_status_ = mojom::LoadModelStatus::kInvalidModelFile;
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kUnavailable);
  next_load_status_ = mojom::LoadModelStatus::kUnsupportedCpu;
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kUnavailable);
  EXPECT_TRUE(bad_messages_.empty());
}

TEST_F(EmbeddingProcessHostTest, NoFileOrNoMemoryLaunchesNothing) {
  file_ok_ = false;
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kUnavailable);
  file_ok_ = true;
  enough_memory_ = false;
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kNotEnoughMemory);
  EXPECT_TRUE(services_.empty());
}

TEST_F(EmbeddingProcessHostTest, CrashesFailAndBackOff) {
  for (int i = 0; i < 3; ++i) {
    Embed({"warm up"});
    service().reply = FakeService::Reply::kHang;
    Result* r = Embed({"a"});
    EXPECT_FALSE(r->done);
    service().Crash();
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(r->done);
    EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
    task_environment_.FastForwardBy(base::Seconds(31));  // past the back-off
  }
  // Three crashes while embedding within ten minutes: refused for now.
  const size_t launched = services_.size();
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kBackingOff);
  EXPECT_EQ(services_.size(), launched);
}

TEST_F(EmbeddingProcessHostTest, HangHitsTheDeadline) {
  Embed({"warm up"});
  service().reply = FakeService::Reply::kHang;
  Result* r = Embed({"a"});
  task_environment_.FastForwardBy(base::Minutes(3) + base::Seconds(1));
  EXPECT_TRUE(r->done);
  EXPECT_EQ(r->status, EmbeddingStatus::kFailed);
  EXPECT_FALSE(host_->HasProcessForTesting());
  std::move(service().pending).Run(std::nullopt);  // late: ignored
  task_environment_.RunUntilIdle();
}

TEST_F(EmbeddingProcessHostTest, IdleProcessEnds) {
  Embed({"a"});
  EXPECT_TRUE(host_->HasProcessForTesting());
  task_environment_.FastForwardBy(base::Minutes(4));
  EXPECT_TRUE(host_->HasProcessForTesting());
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_FALSE(host_->HasProcessForTesting());
  // Relaunched on demand.
  EXPECT_EQ(Embed({"a"})->status, EmbeddingStatus::kOk);
  EXPECT_EQ(services_.size(), 2u);
}

// The omnibox's @memory shortcut: EmbedIfLoaded() uses a process that has
// the model loaded, and never launches, loads or replaces one.
TEST_F(EmbeddingProcessHostTest, EmbedIfLoadedNeverStartsTheModel) {
  auto embed_if_loaded = [this](const EmbeddingModelConfig& config) {
    results_.push_back(std::make_unique<Result>());
    Result* r = results_.back().get();
    host_->EmbedIfLoaded(config, {"query"},
                         base::BindOnce(
                             [](Result* r, EmbeddingStatus status,
                                std::vector<std::vector<float>> vectors) {
                               r->done = true;
                               r->status = status;
                               r->vectors = std::move(vectors);
                             },
                             base::Unretained(r)));
    EXPECT_FALSE(r->done) << "never synchronously";
    task_environment_.RunUntilIdle();
    return r;
  };

  // No process: nothing is launched.
  Result* r = embed_if_loaded(Config());
  EXPECT_TRUE(r->done);
  EXPECT_EQ(r->status, EmbeddingStatus::kNotLoaded);
  EXPECT_TRUE(services_.empty());
  EXPECT_FALSE(host_->HasProcessForTesting());

  // Loaded (by background work): used.
  Embed({"warm up"});
  ASSERT_EQ(services_.size(), 1u);
  r = embed_if_loaded(Config());
  EXPECT_EQ(r->status, EmbeddingStatus::kOk);
  ASSERT_EQ(r->vectors.size(), 1u);

  // Another model is not this one: the loaded process stays.
  EmbeddingModelConfig other = Config();
  other.dimensions = 8;
  r = embed_if_loaded(other);
  EXPECT_EQ(r->status, EmbeddingStatus::kNotLoaded);
  EXPECT_EQ(services_.size(), 1u);
  EXPECT_TRUE(host_->HasProcessForTesting());

  // Once the idle process has ended, typing doesn't bring it back.
  task_environment_.FastForwardBy(base::Minutes(6));
  ASSERT_FALSE(host_->HasProcessForTesting());
  r = embed_if_loaded(Config());
  EXPECT_EQ(r->status, EmbeddingStatus::kNotLoaded);
  EXPECT_EQ(services_.size(), 1u);
}

TEST_F(EmbeddingProcessHostTest, AnotherModelReplacesTheProcess) {
  Embed({"a"});
  EmbeddingModelConfig other = Config();
  other.dimensions = 8;
  Result* r = Embed({"a"}, other);
  EXPECT_EQ(r->status, EmbeddingStatus::kOk);
  EXPECT_EQ(r->vectors[0].size(), 8u);
  EXPECT_EQ(services_.size(), 2u);
}

TEST_F(EmbeddingProcessHostTest, UnloadWaitsAndFailsQueuedCalls) {
  Embed({"warm up"});
  service().reply = FakeService::Reply::kHang;
  Result* r = Embed({"a"});
  bool unloaded = false;
  host_->Unload(
      base::BindOnce([](bool* b) { *b = true; }, base::Unretained(&unloaded)));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(r->done);
  EXPECT_EQ(r->status, EmbeddingStatus::kUnavailable);
  EXPECT_FALSE(host_->HasProcessForTesting());
  // The fake reports no exit: given up on after the grace periods.
  task_environment_.FastForwardBy(base::Seconds(11));
  EXPECT_TRUE(unloaded);
}

TEST_F(EmbeddingProcessHostTest, ShutdownFailsEverything) {
  Embed({"warm up"});
  service().reply = FakeService::Reply::kHang;
  Result* r = Embed({"a"});
  host_->Shutdown();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(r->status, EmbeddingStatus::kShuttingDown);
  EXPECT_EQ(Embed({"b"})->status, EmbeddingStatus::kShuttingDown);
}

}  // namespace
}  // namespace molt_ai
