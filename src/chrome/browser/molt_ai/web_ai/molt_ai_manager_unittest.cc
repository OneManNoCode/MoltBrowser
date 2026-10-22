// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltAIManager and its sessions over real mojo pipes, with a fake document
// environment and a fake on-device runtime.

#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/functions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/ai/ai_common.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_rewriter.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_summarizer.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_writer.mojom.h"
#include "third_party/blink/public/mojom/ai/model_streaming_responder.mojom.h"

namespace molt_ai::web_ai {
namespace {

using blink::mojom::AIManagerCreateClientError;
using blink::mojom::AILanguageModelPromptRole;
using blink::mojom::ModelAvailabilityCheckResult;
using blink::mojom::ModelStreamingResponseStatus;

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

struct EnvironmentState {
  ContextFacts facts;
  bool policy_allowed = true;
  SiteConsent consent = SiteConsent::kAsk;
  // What the user answers when asked (kAsk: closes the prompt).
  SiteConsent answer = SiteConsent::kAllowed;
  int prompts = 0;
  bool last_needs_model = false;
  bool foreground = true;
  std::string site_key = "1|https://a.test";
};

class FakeEnvironment : public WebAIEnvironment {
 public:
  explicit FakeEnvironment(EnvironmentState* state) : state_(state) {}

  ContextFacts GetContextFacts(WebAIApi api) override { return state_->facts; }
  bool IsAllowedByPermissionsPolicy(WebAIApi api) override {
    return state_->policy_allowed;
  }
  SiteConsent GetConsent() override { return state_->consent; }
  void RequestConsent(bool needs_model,
                      base::OnceCallback<void(SiteConsent)> callback) override {
    ++state_->prompts;
    state_->last_needs_model = needs_model;
    if (state_->answer != SiteConsent::kAsk) {
      state_->consent = state_->answer;  // what the consent store does
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), state_->answer));
  }
  std::string GetSiteKey() override { return state_->site_key; }
  bool IsForeground() override { return state_->foreground; }

 private:
  raw_ptr<EnvironmentState> state_;
};

class FakeBackend : public WebAIBackend {
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
    facts.web_apis_enabled = true;
  }

  // WebAIBackend:
  void GetDeviceFacts(DeviceCallback callback) override {
    ++device_calls;
    if (hold_device_facts) {
      held_device_facts.push_back(std::move(callback));
      return;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), facts));
  }
  void ReleaseDeviceFacts() {
    hold_device_facts = false;
    std::vector<DeviceCallback> held;
    held.swap(held_device_facts);
    for (DeviceCallback& callback : held) {
      std::move(callback).Run(facts);
    }
  }
  void PrepareModel(PrepareCallback callback) override {
    ++prepare_calls;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), prepare_ok, context_tokens));
  }
  uint64_t Generate(GenerationJob job,
                    ChunkCallback on_chunk,
                    DoneCallback on_done) override {
    const uint64_t id = next_id_++;
    if (throttle) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(on_done), GenerationStatus::kThrottled));
      return id;
    }
    Pending pending;
    pending.job = std::move(job);
    pending.on_chunk = std::move(on_chunk);
    pending.on_done = std::move(on_done);
    jobs.emplace(id, std::move(pending));
    return id;
  }
  void Cancel(uint64_t id) override {
    cancelled.push_back(id);
    jobs.erase(id);
  }
  bool TryAddSession(const std::string& site_key) override {
    if (sessions[site_key] >= session_cap) {
      return false;
    }
    ++sessions[site_key];
    return true;
  }
  void RemoveSession(const std::string& site_key) override {
    --sessions[site_key];
  }

  // The only generation waiting.
  uint64_t OnlyJob() {
    EXPECT_EQ(jobs.size(), 1u);
    return jobs.empty() ? 0 : jobs.begin()->first;
  }
  const GenerationJob& Job(uint64_t id) { return jobs.at(id).job; }
  void Reply(uint64_t id,
             const std::vector<std::string>& chunks,
             GenerationStatus status = GenerationStatus::kOk) {
    Pending pending = std::move(jobs.at(id));
    jobs.erase(id);
    for (const std::string& chunk : chunks) {
      pending.on_chunk.Run(chunk);
    }
    std::move(pending.on_done).Run(status);
  }

  DeviceFacts facts;
  bool prepare_ok = true;
  uint32_t context_tokens = 8192;
  bool throttle = false;
  bool hold_device_facts = false;
  std::vector<DeviceCallback> held_device_facts;
  size_t session_cap = kMaxSessionsPerSite;
  int device_calls = 0;
  int prepare_calls = 0;
  std::map<uint64_t, Pending> jobs;
  std::vector<uint64_t> cancelled;
  std::map<std::string, size_t> sessions;

 private:
  uint64_t next_id_ = 1;
};

class TestResponder : public blink::mojom::ModelStreamingResponder {
 public:
  mojo::PendingRemote<blink::mojom::ModelStreamingResponder> Remote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  // AbortSignal: Blink drops the pipe.
  void Abort() { receiver_.reset(); }
  bool done() const { return completed || error.has_value(); }

  // blink::mojom::ModelStreamingResponder:
  void OnCompletion(
      blink::mojom::ModelExecutionContextInfoPtr context_info) override {
    completed = true;
    if (context_info) {
      usage = context_info->current_tokens;
    }
  }
  void OnError(ModelStreamingResponseStatus status,
               blink::mojom::QuotaErrorInfoPtr quota_error_info) override {
    error = status;
    if (quota_error_info) {
      quota_requested = quota_error_info->requested;
      quota = quota_error_info->quota;
    }
  }
  void OnStreaming(const std::string& chunk) override {
    text += chunk;
    ++chunks;
  }
  void OnToolCalls(std::vector<blink::mojom::ToolCallPtr> tool_calls) override {
  }
  void OnContextOverflow() override { ++overflows; }

  bool completed = false;
  std::optional<uint64_t> usage;
  std::optional<ModelStreamingResponseStatus> error;
  std::optional<uint32_t> quota_requested;
  std::optional<uint32_t> quota;
  std::string text;
  int chunks = 0;
  int overflows = 0;

 private:
  mojo::Receiver<blink::mojom::ModelStreamingResponder> receiver_{this};
};

class LanguageModelClient
    : public blink::mojom::AIManagerCreateLanguageModelClient {
 public:
  mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
  Remote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void Abort() { receiver_.reset(); }

  void OnResult(mojo::PendingRemote<blink::mojom::AILanguageModel> remote,
                blink::mojom::AILanguageModelInstanceInfoPtr instance_info)
      override {
    session.Bind(std::move(remote));
    info = std::move(instance_info);
  }
  void OnError(AIManagerCreateClientError create_error,
               blink::mojom::QuotaErrorInfoPtr quota_error_info) override {
    error = create_error;
    quota = std::move(quota_error_info);
  }

  mojo::Remote<blink::mojom::AILanguageModel> session;
  blink::mojom::AILanguageModelInstanceInfoPtr info;
  std::optional<AIManagerCreateClientError> error;
  blink::mojom::QuotaErrorInfoPtr quota;

 private:
  mojo::Receiver<blink::mojom::AIManagerCreateLanguageModelClient> receiver_{
      this};
};

template <typename ClientInterface, typename SessionInterface>
class WritingClient : public ClientInterface {
 public:
  mojo::PendingRemote<ClientInterface> Remote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnResult(mojo::PendingRemote<SessionInterface> remote) override {
    session.Bind(std::move(remote));
  }
  void OnError(AIManagerCreateClientError create_error,
               blink::mojom::QuotaErrorInfoPtr quota_error_info) override {
    error = create_error;
    quota = std::move(quota_error_info);
  }

  mojo::Remote<SessionInterface> session;
  std::optional<AIManagerCreateClientError> error;
  blink::mojom::QuotaErrorInfoPtr quota;

 private:
  mojo::Receiver<ClientInterface> receiver_{this};
};

using SummarizerClient =
    WritingClient<blink::mojom::AIManagerCreateSummarizerClient,
                  blink::mojom::AISummarizer>;
using WriterClient = WritingClient<blink::mojom::AIManagerCreateWriterClient,
                                   blink::mojom::AIWriter>;
using RewriterClient =
    WritingClient<blink::mojom::AIManagerCreateRewriterClient,
                  blink::mojom::AIRewriter>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

blink::mojom::AILanguageModelPromptPtr Message(AILanguageModelPromptRole role,
                                               const std::string& text) {
  std::vector<blink::mojom::AILanguageModelPromptContentPtr> content;
  content.push_back(blink::mojom::AILanguageModelPromptContent::NewText(text));
  return blink::mojom::AILanguageModelPrompt::New(role, std::move(content),
                                                  /*is_prefix=*/false);
}

std::vector<blink::mojom::AILanguageModelPromptPtr> User(
    const std::string& text) {
  std::vector<blink::mojom::AILanguageModelPromptPtr> prompts;
  prompts.push_back(Message(AILanguageModelPromptRole::kUser, text));
  return prompts;
}

ContextFacts Eligible() {
  ContextFacts facts;
  facts.feature_enabled = true;
  facts.enterprise_allowed = true;
  facts.user_enabled = true;
  facts.moltnet = false;
  facts.is_document = true;
  facts.http_top_level = true;
  return facts;
}

blink::mojom::AISummarizerCreateOptionsPtr MakeSummarizerOptions(
    const std::optional<std::string>& shared_context = std::nullopt) {
  auto options = blink::mojom::AISummarizerCreateOptions::New();
  options->shared_context = shared_context;
  options->type = blink::mojom::AISummarizerType::kKeyPoints;
  options->format = blink::mojom::AISummarizerFormat::kMarkDown;
  options->length = blink::mojom::AISummarizerLength::kMedium;
  options->preference = blink::mojom::PerformancePreference::kAuto;
  options->output_language = blink::mojom::AILanguageCode::New("es");
  return options;
}

blink::mojom::AIWriterCreateOptionsPtr MakeWriterOptions() {
  auto options = blink::mojom::AIWriterCreateOptions::New();
  options->tone = blink::mojom::AIWriterTone::kFormal;
  options->format = blink::mojom::AIWriterFormat::kPlainText;
  options->length = blink::mojom::AIWriterLength::kShort;
  options->output_language = blink::mojom::AILanguageCode::New("");
  return options;
}

blink::mojom::AIRewriterCreateOptionsPtr MakeRewriterOptions() {
  auto options = blink::mojom::AIRewriterCreateOptions::New();
  options->tone = blink::mojom::AIRewriterTone::kMoreCasual;
  options->format = blink::mojom::AIRewriterFormat::kAsIs;
  options->length = blink::mojom::AIRewriterLength::kShorter;
  options->output_language = blink::mojom::AILanguageCode::New("");
  return options;
}

class MoltAIManagerTest : public testing::Test {
 protected:
  MoltAIManagerTest() {
    env_.facts = Eligible();
    mojo::SetDefaultProcessErrorHandler(base::BindRepeating(
        [](std::vector<std::string>* out, const std::string& error) {
          out->push_back(error);
        },
        base::Unretained(&bad_messages_)));
    NewManager();
  }
  ~MoltAIManagerTest() override {
    remote_.reset();
    manager_.reset();
    mojo::SetDefaultProcessErrorHandler(base::NullCallback());
  }

  void NewManager() {
    remote_.reset();
    manager_ = std::make_unique<MoltAIManager>(
        std::make_unique<FakeEnvironment>(&env_), &backend_);
    manager_->set_stream_interval_for_testing(base::TimeDelta());
    manager_->AddReceiver(remote_.BindNewPipeAndPassReceiver());
  }

  void RunUntilIdle() { task_environment_.RunUntilIdle(); }

  ModelAvailabilityCheckResult LanguageModelAvailability(
      blink::mojom::AILanguageModelCreateOptionsPtr options =
          blink::mojom::AILanguageModelCreateOptions::New()) {
    base::test::TestFuture<ModelAvailabilityCheckResult> future;
    remote_->CanCreateLanguageModel(std::move(options), future.GetCallback());
    return future.Get();
  }

  std::unique_ptr<LanguageModelClient> CreateLanguageModel(
      blink::mojom::AILanguageModelCreateOptionsPtr options =
          blink::mojom::AILanguageModelCreateOptions::New()) {
    auto client = std::make_unique<LanguageModelClient>();
    remote_->CreateLanguageModel(client->Remote(), std::move(options),
                                 mojo::NullRemote());
    RunUntilIdle();
    return client;
  }

  // A session for a site the user already allowed.
  std::unique_ptr<LanguageModelClient> AllowedLanguageModel(
      blink::mojom::AILanguageModelCreateOptionsPtr options =
          blink::mojom::AILanguageModelCreateOptions::New()) {
    env_.consent = SiteConsent::kAllowed;
    auto client = CreateLanguageModel(std::move(options));
    EXPECT_TRUE(client->session.is_bound());
    return client;
  }

  void Prompt(LanguageModelClient& client,
              const std::string& text,
              TestResponder& responder) {
    client.session->Prompt(User(text), nullptr, responder.Remote());
    RunUntilIdle();
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  EnvironmentState env_;
  FakeBackend backend_;
  std::vector<std::string> bad_messages_;
  std::unique_ptr<MoltAIManager> manager_;
  mojo::Remote<blink::mojom::AIManager> remote_;
};

// ---------------------------------------------------------------------------
// availability()
// ---------------------------------------------------------------------------

TEST_F(MoltAIManagerTest, OnlyAnAllowedSiteSeesAvailable) {
  env_.consent = SiteConsent::kAsk;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kDownloadable);
  env_.consent = SiteConsent::kAllowed;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kAvailable);
  env_.consent = SiteConsent::kBlocked;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

TEST_F(MoltAIManagerTest, UnaskedSitesCantTellWhetherAModelIsInstalled) {
  env_.consent = SiteConsent::kAsk;
  backend_.facts.model_installed = true;
  const ModelAvailabilityCheckResult with = LanguageModelAvailability();
  backend_.facts.model_installed = false;
  EXPECT_EQ(LanguageModelAvailability(), with);
  EXPECT_EQ(with, ModelAvailabilityCheckResult::kDownloadable);
}

TEST_F(MoltAIManagerTest, MoltNetWorkersAndOtherSchemesAreUnavailable) {
  env_.consent = SiteConsent::kAllowed;
  env_.facts.moltnet = true;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
  env_.facts = Eligible();
  env_.facts.is_document = false;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
  env_.facts = Eligible();
  env_.facts.http_top_level = false;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
  // None of them asked the runtime anything.
  EXPECT_EQ(backend_.device_calls, 0);
}

TEST_F(MoltAIManagerTest, PolicyAndDeviceReasons) {
  env_.consent = SiteConsent::kAllowed;
  env_.facts.enterprise_allowed = false;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableEnterprisePolicyDisabled);
  env_.facts = Eligible();
  backend_.facts.inference_supported = false;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableModelNotEligible);
  backend_.facts.inference_supported = true;
  backend_.facts.web_apis_enabled = false;
  EXPECT_EQ(LanguageModelAvailability(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

TEST_F(MoltAIManagerTest, OnlyTextOptionsAreAvailable) {
  env_.consent = SiteConsent::kAllowed;
  auto image = blink::mojom::AILanguageModelCreateOptions::New();
  image->expected_inputs.emplace();
  image->expected_inputs->push_back(blink::mojom::AILanguageModelExpected::New(
      blink::mojom::AILanguageModelPromptType::kImage, std::nullopt));
  EXPECT_EQ(
      LanguageModelAvailability(std::move(image)),
      ModelAvailabilityCheckResult::kUnavailableModelAdaptationNotAvailable);

  auto language = blink::mojom::AILanguageModelCreateOptions::New();
  language->expected_outputs.emplace();
  std::vector<blink::mojom::AILanguageCodePtr> codes;
  codes.push_back(blink::mojom::AILanguageCode::New("not a tag!"));
  language->expected_outputs->push_back(
      blink::mojom::AILanguageModelExpected::New(
          blink::mojom::AILanguageModelPromptType::kText, std::move(codes)));
  EXPECT_EQ(LanguageModelAvailability(std::move(language)),
            ModelAvailabilityCheckResult::kUnavailableUnsupportedLanguage);
}

TEST_F(MoltAIManagerTest, WritingAssistanceAvailability) {
  env_.consent = SiteConsent::kAllowed;
  base::test::TestFuture<ModelAvailabilityCheckResult> summarizer;
  remote_->CanCreateSummarizer(MakeSummarizerOptions(), summarizer.GetCallback());
  EXPECT_EQ(summarizer.Get(), ModelAvailabilityCheckResult::kAvailable);
  base::test::TestFuture<ModelAvailabilityCheckResult> writer;
  remote_->CanCreateWriter(MakeWriterOptions(), writer.GetCallback());
  EXPECT_EQ(writer.Get(), ModelAvailabilityCheckResult::kAvailable);
  auto bad = MakeRewriterOptions();
  bad->output_language = blink::mojom::AILanguageCode::New("x");
  base::test::TestFuture<ModelAvailabilityCheckResult> rewriter;
  remote_->CanCreateRewriter(std::move(bad), rewriter.GetCallback());
  EXPECT_EQ(rewriter.Get(),
            ModelAvailabilityCheckResult::kUnavailableUnsupportedLanguage);
  // Not served at all.
  base::test::TestFuture<ModelAvailabilityCheckResult> embedder;
  remote_->CanCreateSemanticEmbedder(embedder.GetCallback());
  EXPECT_EQ(embedder.Get(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

TEST_F(MoltAIManagerTest, PermissionsPolicyViolationIsABadMessage) {
  env_.policy_allowed = false;
  remote_->CanCreateLanguageModel(
      blink::mojom::AILanguageModelCreateOptions::New(), base::DoNothing());
  RunUntilIdle();
  ASSERT_EQ(bad_messages_.size(), 1u);
}

TEST_F(MoltAIManagerTest, ParamsAreUniformAndOffInMoltNet) {
  base::test::TestFuture<blink::mojom::AILanguageModelParamsPtr> params;
  remote_->GetLanguageModelParams(params.GetCallback());
  const blink::mojom::AILanguageModelParamsPtr& p = params.Get();
  ASSERT_TRUE(p);
  EXPECT_EQ(p->default_sampling_params->top_k, kDefaultSampling.top_k);
  EXPECT_EQ(p->max_sampling_params->top_k, kMaxSampling.top_k);
  EXPECT_FLOAT_EQ(p->max_sampling_params->temperature,
                  kMaxSampling.temperature);
  env_.facts.moltnet = true;
  base::test::TestFuture<blink::mojom::AILanguageModelParamsPtr> moltnet;
  remote_->GetLanguageModelParams(moltnet.GetCallback());
  EXPECT_FALSE(moltnet.Get());
}

// ---------------------------------------------------------------------------
// create(): consent and the model
// ---------------------------------------------------------------------------

TEST_F(MoltAIManagerTest, CreateAsksTheUserThenLoadsTheModel) {
  env_.consent = SiteConsent::kAsk;
  env_.answer = SiteConsent::kAllowed;
  auto client = CreateLanguageModel();
  EXPECT_EQ(env_.prompts, 1);
  EXPECT_FALSE(env_.last_needs_model);
  EXPECT_EQ(backend_.prepare_calls, 1);
  ASSERT_TRUE(client->session.is_bound());
  ASSERT_TRUE(client->info);
  EXPECT_EQ(client->info->input_quota, kLargeInputQuota);
  EXPECT_EQ(client->info->input_usage, 0u);
  EXPECT_EQ(manager_->session_count_for_testing(), 1u);
  // Asked once: the answer is remembered.
  auto second = CreateLanguageModel();
  EXPECT_EQ(env_.prompts, 1);
  EXPECT_TRUE(second->session.is_bound());
}

TEST_F(MoltAIManagerTest, BlockingOrClosingThePromptFailsCreate) {
  for (SiteConsent answer : {SiteConsent::kBlocked, SiteConsent::kAsk}) {
    env_.consent = SiteConsent::kAsk;
    env_.answer = answer;
    auto client = CreateLanguageModel();
    EXPECT_EQ(client->error, AIManagerCreateClientError::kUnableToCreateSession);
    EXPECT_FALSE(client->session.is_bound());
  }
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(manager_->session_count_for_testing(), 0u);
}

TEST_F(MoltAIManagerTest, WithoutAModelThePromptOffersThePickerAndCreateFails) {
  backend_.facts.model_installed = false;
  env_.consent = SiteConsent::kAsk;
  env_.answer = SiteConsent::kAllowed;
  auto client = CreateLanguageModel();
  EXPECT_EQ(env_.prompts, 1);
  EXPECT_TRUE(env_.last_needs_model);
  EXPECT_EQ(client->error, AIManagerCreateClientError::kUnableToCreateSession);
  // Nothing was downloaded or loaded for the page.
  EXPECT_EQ(backend_.prepare_calls, 0);
}

TEST_F(MoltAIManagerTest, CreateInMoltNetNeverAsks) {
  env_.facts.moltnet = true;
  env_.consent = SiteConsent::kAsk;
  auto client = CreateLanguageModel();
  EXPECT_EQ(client->error, AIManagerCreateClientError::kUnableToCreateSession);
  EXPECT_EQ(env_.prompts, 0);
  EXPECT_EQ(backend_.prepare_calls, 0);
}

TEST_F(MoltAIManagerTest, AbortedCreateStopsBeforeThePrompt) {
  env_.consent = SiteConsent::kAsk;
  backend_.hold_device_facts = true;
  auto client = std::make_unique<LanguageModelClient>();
  remote_->CreateLanguageModel(client->Remote(),
                               blink::mojom::AILanguageModelCreateOptions::New(),
                               mojo::NullRemote());
  RunUntilIdle();
  EXPECT_EQ(manager_->pending_create_count_for_testing(), 1u);
  // create()'s AbortSignal fires: Blink drops the client pipe.
  client->Abort();
  RunUntilIdle();
  backend_.ReleaseDeviceFacts();
  RunUntilIdle();
  EXPECT_EQ(env_.prompts, 0);
  EXPECT_EQ(backend_.prepare_calls, 0);
  EXPECT_EQ(manager_->pending_create_count_for_testing(), 0u);
}

TEST_F(MoltAIManagerTest, AModelThatWontLoadFailsCreate) {
  env_.consent = SiteConsent::kAllowed;
  backend_.prepare_ok = false;
  auto client = CreateLanguageModel();
  EXPECT_EQ(client->error, AIManagerCreateClientError::kUnableToCreateSession);
}

TEST_F(MoltAIManagerTest, SmallContextGetsTheSmallQuota) {
  env_.consent = SiteConsent::kAllowed;
  backend_.context_tokens = 2048;
  auto options = blink::mojom::AILanguageModelCreateOptions::New();
  // ~1300 estimated tokens: fits the large bucket, not the small one.
  options->initial_prompts.push_back(Message(AILanguageModelPromptRole::kUser,
                                             std::string(3900, 'x')));
  auto client = CreateLanguageModel(std::move(options));
  EXPECT_EQ(client->error, AIManagerCreateClientError::kInitialInputTooLarge);
  ASSERT_TRUE(client->quota);
  EXPECT_EQ(client->quota->quota, kSmallInputQuota);
  EXPECT_GT(client->quota->requested, kSmallInputQuota);

  auto small = AllowedLanguageModel();
  EXPECT_EQ(small->info->input_quota, kSmallInputQuota);
}

TEST_F(MoltAIManagerTest, HugeInitialPromptsFailBeforeAnyPrompt) {
  env_.consent = SiteConsent::kAsk;
  auto options = blink::mojom::AILanguageModelCreateOptions::New();
  options->initial_prompts.push_back(Message(AILanguageModelPromptRole::kUser,
                                             std::string(1 << 20, 'x')));
  auto client = CreateLanguageModel(std::move(options));
  EXPECT_EQ(client->error, AIManagerCreateClientError::kInitialInputTooLarge);
  EXPECT_EQ(env_.prompts, 0);
}

TEST_F(MoltAIManagerTest, ASystemMessageAfterTheFirstFailsCreate) {
  env_.consent = SiteConsent::kAllowed;
  auto options = blink::mojom::AILanguageModelCreateOptions::New();
  options->initial_prompts.push_back(
      Message(AILanguageModelPromptRole::kUser, "hi"));
  options->initial_prompts.push_back(
      Message(AILanguageModelPromptRole::kSystem, "late"));
  auto client = CreateLanguageModel(std::move(options));
  EXPECT_EQ(client->error, AIManagerCreateClientError::kUnableToCreateSession);
}

TEST_F(MoltAIManagerTest, TooManySessionsForTheSite) {
  env_.consent = SiteConsent::kAllowed;
  backend_.session_cap = 1;
  auto first = AllowedLanguageModel();
  auto second = CreateLanguageModel();
  EXPECT_EQ(second->error, AIManagerCreateClientError::kUnableToCreateSession);
  // Destroying the first frees its slot.
  first->session->Destroy();
  RunUntilIdle();
  EXPECT_EQ(manager_->session_count_for_testing(), 0u);
  auto third = CreateLanguageModel();
  EXPECT_TRUE(third->session.is_bound());
}

// ---------------------------------------------------------------------------
// LanguageModel sessions
// ---------------------------------------------------------------------------

TEST_F(MoltAIManagerTest, PromptStreamsAndKeepsTheConversation) {
  auto options = blink::mojom::AILanguageModelCreateOptions::New();
  options->initial_prompts.push_back(
      Message(AILanguageModelPromptRole::kSystem, "Answer like a pirate."));
  auto client = AllowedLanguageModel(std::move(options));
  const uint64_t system_usage = client->info->input_usage;
  EXPECT_EQ(system_usage, EstimateMessageTokens("Answer like a pirate."));

  TestResponder first;
  Prompt(*client, "Hello?", first);
  uint64_t id = backend_.OnlyJob();
  {
    const GenerationJob& job = backend_.Job(id);
    // MoltBrowser's instructions are the system prompt; the page's are page
    // text, in the context the inference service neutralizes.
    EXPECT_EQ(job.system_prompt.find("pirate"), std::string::npos);
    EXPECT_EQ(job.context, "Answer like a pirate.");
    EXPECT_FALSE(job.context_preamble.empty());
    EXPECT_EQ(job.user_message, "Hello?");
    EXPECT_TRUE(job.history.empty());
    EXPECT_EQ(job.max_output_tokens, kLargeMaxReplyTokens);
    EXPECT_EQ(job.site_key, env_.site_key);
    EXPECT_FALSE(job.conversation_key.empty());
  }
  backend_.Reply(id, {"Ahoy", ", matey"});
  RunUntilIdle();
  EXPECT_EQ(first.text, "Ahoy, matey");
  EXPECT_TRUE(first.completed);
  ASSERT_TRUE(first.usage);
  EXPECT_EQ(*first.usage, system_usage + EstimateMessageTokens("Hello?") +
                              EstimateMessageTokens("Ahoy, matey"));

  TestResponder second;
  Prompt(*client, "Again", second);
  id = backend_.OnlyJob();
  const GenerationJob& job = backend_.Job(id);
  ASSERT_EQ(job.history.size(), 2u);
  EXPECT_EQ(job.history[0].first, Role::kUser);
  EXPECT_EQ(job.history[0].second, "Hello?");
  EXPECT_EQ(job.history[1].first, Role::kAssistant);
  EXPECT_EQ(job.history[1].second, "Ahoy, matey");
  EXPECT_EQ(job.user_message, "Again");
}

TEST_F(MoltAIManagerTest, StreamingIsCoalesced) {
  manager_->set_stream_interval_for_testing(base::Milliseconds(200));
  auto client = AllowedLanguageModel();
  TestResponder responder;
  Prompt(*client, "Hi", responder);
  const uint64_t id = backend_.OnlyJob();
  FakeBackend::Pending pending = std::move(backend_.jobs.at(id));
  backend_.jobs.erase(id);
  pending.on_chunk.Run("a");
  pending.on_chunk.Run("b");
  task_environment_.AdvanceClock(base::Milliseconds(250));
  pending.on_chunk.Run("c");  // flushes "abc"
  pending.on_chunk.Run("d");
  std::move(pending.on_done).Run(GenerationStatus::kOk);  // flushes "d"
  RunUntilIdle();
  EXPECT_EQ(responder.text, "abcd");
  EXPECT_EQ(responder.chunks, 2);
  EXPECT_TRUE(responder.completed);
}

TEST_F(MoltAIManagerTest, PromptOverTheQuotaIsRefused) {
  backend_.context_tokens = 2048;  // small bucket
  auto client = AllowedLanguageModel();
  TestResponder responder;
  Prompt(*client, std::string(3 * 2000, 'x'), responder);
  EXPECT_EQ(responder.error, ModelStreamingResponseStatus::kErrorInputTooLarge);
  EXPECT_EQ(responder.quota, kSmallInputQuota);
  EXPECT_GT(responder.quota_requested.value_or(0), kSmallInputQuota);
  EXPECT_TRUE(backend_.jobs.empty());
  // A huge prompt is refused by its size alone.
  TestResponder huge;
  Prompt(*client, std::string(8 << 20, 'x'), huge);
  EXPECT_EQ(huge.error, ModelStreamingResponseStatus::kErrorInputTooLarge);
}

// Under the size shortcut, the estimate decides: non-ASCII text counts
// more per byte.
TEST_F(MoltAIManagerTest, PromptOverTheQuotaByEstimate) {
  backend_.context_tokens = 2048;  // quota 1024
  auto client = AllowedLanguageModel();
  std::string text;
  for (int i = 0; i < 800; ++i) {
    text += "\xC3\xA9";  // 1600 bytes, 1067 tokens
  }
  TestResponder responder;
  Prompt(*client, text, responder);
  EXPECT_EQ(responder.error, ModelStreamingResponseStatus::kErrorInputTooLarge);
  EXPECT_EQ(responder.quota_requested, EstimateMessageTokens(text));
  EXPECT_TRUE(backend_.jobs.empty());
}

TEST_F(MoltAIManagerTest, OverflowEvictsTheOldestTurns) {
  backend_.context_tokens = 2048;  // quota 1024
  auto client = AllowedLanguageModel();
  const std::string chunk(3 * 296, 'x');  // 300 tokens with the overhead
  for (int i = 0; i < 3; ++i) {
    TestResponder responder;
    Prompt(*client, chunk, responder);
    backend_.Reply(backend_.OnlyJob(), {"ok"});
    RunUntilIdle();
    EXPECT_EQ(responder.overflows, 0) << i;
  }
  TestResponder responder;
  Prompt(*client, chunk, responder);
  const GenerationJob& job = backend_.Job(backend_.OnlyJob());
  // The first exchange was dropped to make room.
  EXPECT_EQ(job.history.size(), 4u);
  backend_.Reply(backend_.OnlyJob(), {"ok"});
  RunUntilIdle();
  EXPECT_EQ(responder.overflows, 1);
  ASSERT_TRUE(responder.usage);
  EXPECT_LE(*responder.usage, kSmallInputQuota);
}

TEST_F(MoltAIManagerTest, AppendAddsContextWithoutTheModel) {
  auto client = AllowedLanguageModel();
  TestResponder appended;
  client->session->Append(User("Remember: 42."), appended.Remote());
  RunUntilIdle();
  EXPECT_TRUE(appended.completed);
  EXPECT_EQ(appended.usage, EstimateMessageTokens("Remember: 42."));
  EXPECT_TRUE(backend_.jobs.empty());
  TestResponder responder;
  Prompt(*client, "What number?", responder);
  // Consecutive user turns are merged for the chat template.
  EXPECT_EQ(backend_.Job(backend_.OnlyJob()).user_message,
            "Remember: 42.\n\nWhat number?");
}

TEST_F(MoltAIManagerTest, AbortCancelsTheGenerationAndKeepsTheContext) {
  auto client = AllowedLanguageModel();
  TestResponder aborted;
  Prompt(*client, "Long question", aborted);
  const uint64_t id = backend_.OnlyJob();
  TestResponder next;
  Prompt(*client, "Next", next);  // waits behind the first
  EXPECT_EQ(backend_.jobs.size(), 1u);
  aborted.Abort();
  RunUntilIdle();
  ASSERT_EQ(backend_.cancelled, std::vector<uint64_t>{id});
  // The next one runs, without the aborted turn.
  const GenerationJob& job = backend_.Job(backend_.OnlyJob());
  EXPECT_TRUE(job.history.empty());
  EXPECT_EQ(job.user_message, "Next");
}

TEST_F(MoltAIManagerTest, DestroyCancelsEverything) {
  auto client = AllowedLanguageModel();
  TestResponder running;
  TestResponder queued;
  Prompt(*client, "one", running);
  Prompt(*client, "two", queued);
  const uint64_t id = backend_.OnlyJob();
  client->session->Destroy();
  RunUntilIdle();
  EXPECT_EQ(backend_.cancelled, std::vector<uint64_t>{id});
  EXPECT_EQ(running.error, ModelStreamingResponseStatus::kErrorSessionDestroyed);
  EXPECT_EQ(queued.error, ModelStreamingResponseStatus::kErrorSessionDestroyed);
  EXPECT_EQ(manager_->session_count_for_testing(), 0u);
  EXPECT_EQ(backend_.sessions[env_.site_key], 0u);
}

TEST_F(MoltAIManagerTest, DroppingTheSessionPipeEndsIt) {
  auto client = AllowedLanguageModel();
  TestResponder running;
  Prompt(*client, "one", running);
  client->session.reset();  // garbage-collected in the page
  RunUntilIdle();
  EXPECT_EQ(backend_.cancelled.size(), 1u);
  EXPECT_EQ(manager_->session_count_for_testing(), 0u);
}

// The document navigated away or the frame went away: MoltAIManager is
// destroyed with the document's user data.
TEST_F(MoltAIManagerTest, DocumentGoneCancelsEverything) {
  auto client = AllowedLanguageModel();
  TestResponder running;
  Prompt(*client, "one", running);
  const uint64_t id = backend_.OnlyJob();
  manager_.reset();
  RunUntilIdle();
  EXPECT_EQ(backend_.cancelled, std::vector<uint64_t>{id});
  EXPECT_TRUE(running.done());
  EXPECT_EQ(backend_.sessions[env_.site_key], 0u);
  EXPECT_FALSE(client->session.is_connected());
}

TEST_F(MoltAIManagerTest, CloneCopiesTheCommittedContext) {
  auto client = AllowedLanguageModel();
  TestResponder responder;
  Prompt(*client, "Hello", responder);
  backend_.Reply(backend_.OnlyJob(), {"Hi"});
  RunUntilIdle();

  LanguageModelClient clone;
  client->session->Fork(clone.Remote());
  RunUntilIdle();
  ASSERT_TRUE(clone.session.is_bound());
  EXPECT_EQ(clone.info->input_usage, *responder.usage);
  EXPECT_EQ(clone.info->input_quota, client->info->input_quota);

  TestResponder original_next;
  Prompt(*client, "A", original_next);
  const std::string original_key =
      backend_.Job(backend_.OnlyJob()).conversation_key;
  backend_.Reply(backend_.OnlyJob(), {"a"});
  RunUntilIdle();

  TestResponder clone_next;
  clone.session->Prompt(User("B"), nullptr, clone_next.Remote());
  RunUntilIdle();
  const GenerationJob& job = backend_.Job(backend_.OnlyJob());
  ASSERT_EQ(job.history.size(), 2u);  // Hello / Hi, not A / a
  EXPECT_EQ(job.history[0].second, "Hello");
  EXPECT_EQ(job.user_message, "B");
  EXPECT_NE(job.conversation_key, original_key);
}

TEST_F(MoltAIManagerTest, MeasureInputUsage) {
  auto client = AllowedLanguageModel();
  base::test::TestFuture<std::optional<uint32_t>> usage;
  client->session->MeasureInputUsage(User("abcdef"), usage.GetCallback());
  EXPECT_EQ(usage.Get(), EstimateMessageTokens("abcdef"));
}

TEST_F(MoltAIManagerTest, UnsupportedPromptsAreRefused) {
  auto client = AllowedLanguageModel();
  // A regular expression constraint can't be applied.
  TestResponder regex;
  client->session->Prompt(
      User("hi"), on_device_model::mojom::ResponseConstraint::NewRegex("a+"),
      regex.Remote());
  // A system message can only come first, at creation.
  TestResponder system;
  std::vector<blink::mojom::AILanguageModelPromptPtr> prompts;
  prompts.push_back(Message(AILanguageModelPromptRole::kSystem, "x"));
  client->session->Prompt(std::move(prompts), nullptr, system.Remote());
  // An assistant prefix isn't supported.
  TestResponder prefix;
  std::vector<blink::mojom::AILanguageModelPromptPtr> prefixed = User("hi");
  prefixed.push_back(Message(AILanguageModelPromptRole::kAssistant, "Sure"));
  prefixed.back()->is_prefix = true;
  client->session->Prompt(std::move(prefixed), nullptr, prefix.Remote());
  RunUntilIdle();
  for (TestResponder* r : {&regex, &system, &prefix}) {
    EXPECT_EQ(r->error, ModelStreamingResponseStatus::kErrorInvalidRequest);
  }
  EXPECT_TRUE(backend_.jobs.empty());
  // A JSON schema is best effort: Blink already put it in the prompt.
  TestResponder json;
  client->session->Prompt(
      User("hi"),
      on_device_model::mojom::ResponseConstraint::NewJsonSchema("{}"),
      json.Remote());
  RunUntilIdle();
  EXPECT_EQ(backend_.jobs.size(), 1u);
}

TEST_F(MoltAIManagerTest, ThrottledAndFailedGenerationsReportAnError) {
  auto client = AllowedLanguageModel();
  TestResponder failed;
  Prompt(*client, "one", failed);
  backend_.Reply(backend_.OnlyJob(), {"partial"}, GenerationStatus::kFailed);
  RunUntilIdle();
  EXPECT_EQ(failed.error, ModelStreamingResponseStatus::kErrorGenericFailure);
  backend_.throttle = true;
  TestResponder throttled;
  Prompt(*client, "two", throttled);
  EXPECT_EQ(throttled.error,
            ModelStreamingResponseStatus::kErrorGenericFailure);
  // Neither changed the context.
  LanguageModelClient clone;
  client->session->Fork(clone.Remote());
  RunUntilIdle();
  EXPECT_EQ(clone.info->input_usage, 0u);
}

TEST_F(MoltAIManagerTest, QueuePerSessionIsBounded) {
  auto client = AllowedLanguageModel();
  std::vector<std::unique_ptr<TestResponder>> responders;
  for (size_t i = 0; i < kMaxQueuedPerSession + 2; ++i) {
    responders.push_back(std::make_unique<TestResponder>());
    client->session->Prompt(User("q"), nullptr, responders.back()->Remote());
  }
  RunUntilIdle();
  // One running, kMaxQueuedPerSession waiting, the last one refused.
  EXPECT_EQ(responders.back()->error,
            ModelStreamingResponseStatus::kErrorGenericFailure);
  EXPECT_FALSE(responders[kMaxQueuedPerSession]->error.has_value());
}

// ---------------------------------------------------------------------------
// Summarizer, Writer, Rewriter
// ---------------------------------------------------------------------------

TEST_F(MoltAIManagerTest, SummarizerBuildsItsPromptFromTheOptions) {
  env_.consent = SiteConsent::kAllowed;
  SummarizerClient client;
  remote_->CreateSummarizer(client.Remote(),
                            MakeSummarizerOptions("An article about boats."),
                            mojo::NullRemote());
  RunUntilIdle();
  ASSERT_TRUE(client.session.is_bound());
  TestResponder responder;
  client.session->Summarize("Boats float.", "For kids.", responder.Remote());
  RunUntilIdle();
  const GenerationJob& job = backend_.Job(backend_.OnlyJob());
  EXPECT_NE(job.system_prompt.find("at most 5 points"), std::string::npos);
  EXPECT_NE(job.system_prompt.find("Spanish"), std::string::npos);
  EXPECT_EQ(job.system_prompt.find("Boats"), std::string::npos);
  EXPECT_EQ(job.context, "An article about boats.\n\nFor kids.");
  EXPECT_EQ(job.user_message, "Boats float.");
  EXPECT_EQ(job.max_output_tokens, 320u);
  EXPECT_TRUE(job.conversation_key.empty());
  backend_.Reply(backend_.OnlyJob(), {"- Los barcos flotan."});
  RunUntilIdle();
  EXPECT_EQ(responder.text, "- Los barcos flotan.");
  EXPECT_TRUE(responder.completed);
  EXPECT_FALSE(responder.usage);
}

TEST_F(MoltAIManagerTest, WriterAndRewriter) {
  env_.consent = SiteConsent::kAllowed;
  WriterClient writer;
  remote_->CreateWriter(writer.Remote(), MakeWriterOptions(), mojo::NullRemote());
  RewriterClient rewriter;
  remote_->CreateRewriter(rewriter.Remote(), MakeRewriterOptions(),
                          mojo::NullRemote());
  RunUntilIdle();
  ASSERT_TRUE(writer.session.is_bound());
  ASSERT_TRUE(rewriter.session.is_bound());

  TestResponder written;
  writer.session->Write("A thank-you note", std::nullopt, written.Remote());
  RunUntilIdle();
  {
    const GenerationJob& job = backend_.Job(backend_.OnlyJob());
    EXPECT_NE(job.system_prompt.find("formal tone"), std::string::npos);
    EXPECT_EQ(job.user_message, "A thank-you note");
    EXPECT_TRUE(job.context.empty());
  }
  backend_.Reply(backend_.OnlyJob(), {"Thank you."});
  RunUntilIdle();
  EXPECT_EQ(written.text, "Thank you.");

  TestResponder rewritten;
  rewriter.session->Rewrite("Dear Sir", "an email", rewritten.Remote());
  RunUntilIdle();
  const GenerationJob& job = backend_.Job(backend_.OnlyJob());
  EXPECT_NE(job.system_prompt.find("more casual"), std::string::npos);
  EXPECT_EQ(job.context, "an email");
  EXPECT_EQ(job.user_message, "Dear Sir");
}

TEST_F(MoltAIManagerTest, WritingInputOverTheQuotaIsRefused) {
  env_.consent = SiteConsent::kAllowed;
  WriterClient writer;
  remote_->CreateWriter(writer.Remote(), MakeWriterOptions(), mojo::NullRemote());
  RunUntilIdle();
  TestResponder responder;
  writer.session->Write(std::string(3 * 9300, 'x'), std::nullopt,
                        responder.Remote());
  RunUntilIdle();
  EXPECT_EQ(responder.error, ModelStreamingResponseStatus::kErrorInputTooLarge);
  EXPECT_EQ(responder.quota, kWritingAssistanceInputQuota);
  EXPECT_TRUE(backend_.jobs.empty());

  // Under the size shortcut: 7000 two-byte characters estimate to 9334.
  std::string accents;
  for (int i = 0; i < 7000; ++i) {
    accents += "\xC3\xA9";
  }
  TestResponder by_estimate;
  writer.session->Write(accents, std::nullopt, by_estimate.Remote());
  RunUntilIdle();
  EXPECT_EQ(by_estimate.error,
            ModelStreamingResponseStatus::kErrorInputTooLarge);
  EXPECT_EQ(by_estimate.quota_requested, EstimateTokens(accents));
  EXPECT_TRUE(backend_.jobs.empty());

  base::test::TestFuture<std::optional<uint32_t>> usage;
  writer.session->MeasureUsage("abc", "def", usage.GetCallback());
  EXPECT_EQ(usage.Get(), 2u);
}

TEST_F(MoltAIManagerTest, OversizedSharedContextFailsCreate) {
  env_.consent = SiteConsent::kAsk;
  SummarizerClient client;
  remote_->CreateSummarizer(client.Remote(),
                            MakeSummarizerOptions(std::string(3 * 9300, 'x')),
                            mojo::NullRemote());
  RunUntilIdle();
  EXPECT_EQ(client.error, AIManagerCreateClientError::kInitialInputTooLarge);
  ASSERT_TRUE(client.quota);
  EXPECT_EQ(client.quota->quota, kWritingAssistanceInputQuota);
  EXPECT_EQ(env_.prompts, 0);
}

TEST_F(MoltAIManagerTest, SharedContextOverTheQuotaByEstimate) {
  env_.consent = SiteConsent::kAllowed;
  std::string accents;
  for (int i = 0; i < 7000; ++i) {
    accents += "\xC3\xA9";
  }
  SummarizerClient client;
  remote_->CreateSummarizer(client.Remote(), MakeSummarizerOptions(accents),
                            mojo::NullRemote());
  RunUntilIdle();
  EXPECT_EQ(client.error, AIManagerCreateClientError::kInitialInputTooLarge);
  ASSERT_TRUE(client.quota);
  EXPECT_EQ(client.quota->requested, EstimateTokens(accents));
  EXPECT_EQ(backend_.prepare_calls, 0);
}

TEST_F(MoltAIManagerTest, WritingCreateAsksLikeTheLanguageModel) {
  env_.consent = SiteConsent::kAsk;
  env_.answer = SiteConsent::kBlocked;
  RewriterClient client;
  remote_->CreateRewriter(client.Remote(), MakeRewriterOptions(),
                          mojo::NullRemote());
  RunUntilIdle();
  EXPECT_EQ(env_.prompts, 1);
  EXPECT_EQ(client.error, AIManagerCreateClientError::kUnableToCreateSession);
  // Now blocked: unavailable, and no more prompts.
  base::test::TestFuture<ModelAvailabilityCheckResult> availability;
  remote_->CanCreateRewriter(MakeRewriterOptions(), availability.GetCallback());
  EXPECT_EQ(availability.Get(),
            ModelAvailabilityCheckResult::kUnavailableFeatureNotEnabled);
}

}  // namespace
}  // namespace molt_ai::web_ai
