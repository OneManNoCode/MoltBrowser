// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"

#include <algorithm>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

namespace molt_ai::web_ai {

namespace {

constexpr base::TaskTraits kWorkerTraits = {
    base::MayBlock(), base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN};

// settings.json is at most a few KiB; anything bigger isn't ours.
constexpr size_t kMaxSettingsBytes = 1 << 20;

struct WebAISettings {
  // The chat's default model ("default_model").
  std::string default_model;
  // "web_ai_apis": false turns the APIs off for every site.
  bool web_apis_enabled = true;
};

// ~/.moltbrowser/settings.json, as the chat reads it. Worker thread.
WebAISettings ReadWebAISettings() {
  WebAISettings settings;
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return settings;
  }
  const base::FilePath path =
      home.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
          .Append(base::FilePath::FromUTF8Unsafe("settings.json"));
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(path, &contents,
                                         kMaxSettingsBytes)) {
    return settings;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(contents, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return settings;
  }
  const auto& dict = parsed->GetDict();
  if (const std::string* model = dict.FindString("default_model")) {
    settings.default_model = *model;
  }
  if (std::optional<bool> enabled = dict.FindBool("web_ai_apis")) {
    settings.web_apis_enabled = *enabled;
  }
  return settings;
}

std::vector<ModelCandidate> Candidates(const BrowserAIRuntime& runtime) {
  std::vector<ModelCandidate> out;
  for (const ModelInfo& info : runtime.GetAvailableModels()) {
    ModelCandidate c;
    c.model_id = info.model_id;
    c.downloaded = info.is_downloaded;
    c.ram_required_bytes = info.ram_required_bytes;
    out.push_back(std::move(c));
  }
  return out;
}

// Loads the model pages use if none is loaded. Worker thread (blocks while
// the model loads).
bool EnsureModelLoaded(BrowserAIRuntime* runtime, uint32_t* context_tokens) {
  if (!runtime->GetLocalInferenceUnsupportedReason().empty()) {
    return false;
  }
  if (runtime->GetLoadedModelId().empty()) {
    runtime->RefreshModelStatus();
    const std::string model_id = PickWebModel(
        /*loaded_model_id=*/"", ReadWebAISettings().default_model,
        Candidates(*runtime));
    if (model_id.empty()) {
      return false;
    }
    std::string error;
    if (!runtime->LoadModel(model_id, &error)) {
      // The reason (memory, file) is for the user, not for the page.
      VLOG(1) << "[MoltAI] web AI APIs: couldn't load " << model_id;
      return false;
    }
  }
  *context_tokens = runtime->GetLoadedContextTokens();
  return true;
}

}  // namespace

struct RuntimeWebAIBackend::Job {
  GenerationJob job;
  ChunkCallback on_chunk;
  DoneCallback on_done;
  // The runtime's cancellation handle, once running.
  uint64_t request_id = 0;
  // Cancelled by the page: no more callbacks; the entry stays until the
  // worker returns (the runtime is busy until then).
  bool cancelled = false;
  bool timed_out = false;
  base::OneShotTimer deadline;
};

// static
RuntimeWebAIBackend* RuntimeWebAIBackend::GetInstance() {
  static base::NoDestructor<RuntimeWebAIBackend> instance;
  return instance.get();
}

RuntimeWebAIBackend::RuntimeWebAIBackend() = default;
RuntimeWebAIBackend::~RuntimeWebAIBackend() = default;

BrowserAIRuntime& RuntimeWebAIBackend::runtime() {
  BrowserAIRuntime& rt = BrowserAIRuntime::GetInstance();
  if (!runtime_initialized_) {
    // Idempotent; the chat does the same (MoltAIChatHandler).
    rt.Initialize();
    rt.StartMemoryPressureMonitoring();
    runtime_initialized_ = true;
  }
  return rt;
}

// ---- Device facts ----

void RuntimeWebAIBackend::GetDeviceFacts(DeviceCallback callback) {
  if (device_facts_ &&
      base::TimeTicks::Now() - device_facts_time_ < kDeviceFactsLifetime) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), *device_facts_));
    return;
  }
  device_waiters_.push_back(std::move(callback));
  if (device_facts_pending_) {
    return;
  }
  device_facts_pending_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kWorkerTraits,
      base::BindOnce(&RuntimeWebAIBackend::ReadDeviceFacts,
                     base::Unretained(&runtime())),
      base::BindOnce(&RuntimeWebAIBackend::OnDeviceFacts,
                     weak_factory_.GetWeakPtr()));
}

// static
DeviceFacts RuntimeWebAIBackend::ReadDeviceFacts(BrowserAIRuntime* runtime) {
  DeviceFacts facts;
  facts.inference_supported =
      runtime->GetLocalInferenceUnsupportedReason().empty();
  runtime->RefreshModelStatus();
  const std::vector<ModelCandidate> models = Candidates(*runtime);
  facts.model_installed = AnyModelInstalled(models);
  facts.some_model_fits =
      AnyModelFits(models, runtime->GetHardwareCapability().total_ram_bytes);
  facts.web_apis_enabled = ReadWebAISettings().web_apis_enabled;
  return facts;
}

void RuntimeWebAIBackend::OnDeviceFacts(DeviceFacts facts) {
  device_facts_pending_ = false;
  device_facts_ = facts;
  device_facts_time_ = base::TimeTicks::Now();
  std::vector<DeviceCallback> waiters;
  waiters.swap(device_waiters_);
  for (DeviceCallback& callback : waiters) {
    std::move(callback).Run(facts);
  }
}

// ---- Model ----

void RuntimeWebAIBackend::PrepareModel(PrepareCallback callback) {
  prepare_waiters_.push_back(std::move(callback));
  if (prepare_pending_) {
    return;
  }
  prepare_pending_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kWorkerTraits,
      base::BindOnce(&RuntimeWebAIBackend::PrepareBlocking,
                     base::Unretained(&runtime())),
      base::BindOnce(&RuntimeWebAIBackend::OnModelPrepared,
                     weak_factory_.GetWeakPtr()));
}

// static
RuntimeWebAIBackend::PrepareResult RuntimeWebAIBackend::PrepareBlocking(
    BrowserAIRuntime* runtime) {
  PrepareResult result;
  result.ok = EnsureModelLoaded(runtime, &result.context_tokens);
  return result;
}

void RuntimeWebAIBackend::OnModelPrepared(PrepareResult result) {
  prepare_pending_ = false;
  // A model was just loaded (or failed to): the cached facts may be stale.
  device_facts_.reset();
  std::vector<PrepareCallback> waiters;
  waiters.swap(prepare_waiters_);
  for (PrepareCallback& callback : waiters) {
    std::move(callback).Run(result.ok, result.context_tokens);
  }
}

// ---- Generations ----

uint64_t RuntimeWebAIBackend::Generate(GenerationJob job,
                                       ChunkCallback on_chunk,
                                       DoneCallback on_done) {
  const uint64_t id = next_id_++;
  const GenerationScheduler::Admission admission =
      scheduler_.Enqueue(id, job.site_key, job.foreground);
  auto entry = std::make_unique<Job>();
  entry->job = std::move(job);
  entry->on_chunk = std::move(on_chunk);
  entry->on_done = std::move(on_done);
  jobs_[id] = std::move(entry);
  if (admission != GenerationScheduler::Admission::kAdmitted) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&RuntimeWebAIBackend::OnJobDone,
                       weak_factory_.GetWeakPtr(), id,
                       GenerationStatus::kThrottled));
    return id;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&RuntimeWebAIBackend::MaybeStartNext,
                                weak_factory_.GetWeakPtr()));
  return id;
}

void RuntimeWebAIBackend::Cancel(uint64_t id) {
  auto it = jobs_.find(id);
  if (it == jobs_.end()) {
    return;
  }
  Job& job = *it->second;
  if (scheduler_.IsRunning(id)) {
    job.cancelled = true;
    job.on_chunk.Reset();
    job.on_done.Reset();
    if (job.request_id != 0) {
      runtime().CancelRequest(job.request_id);
    }
    return;
  }
  scheduler_.Finish(id);
  jobs_.erase(it);
}

void RuntimeWebAIBackend::MaybeStartNext() {
  const uint64_t id = scheduler_.StartNext();
  if (id == 0) {
    return;
  }
  auto it = jobs_.find(id);
  if (it == jobs_.end()) {
    scheduler_.Finish(id);
    MaybeStartNext();
    return;
  }
  Job& job = *it->second;
  BrowserAIRuntime& rt = runtime();
  job.request_id = rt.NewRequestId();
  job.deadline.Start(FROM_HERE, kGenerationDeadline,
                     base::BindOnce(&RuntimeWebAIBackend::OnJobDeadline,
                                    weak_factory_.GetWeakPtr(), id));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kWorkerTraits,
      base::BindOnce(&RuntimeWebAIBackend::RunJob, base::Unretained(&rt),
                     std::move(job.job), job.request_id,
                     base::SequencedTaskRunner::GetCurrentDefault(),
                     weak_factory_.GetWeakPtr(), id),
      base::BindOnce(&RuntimeWebAIBackend::OnJobDone,
                     weak_factory_.GetWeakPtr(), id));
}

// static
GenerationStatus RuntimeWebAIBackend::RunJob(
    BrowserAIRuntime* runtime,
    GenerationJob job,
    uint64_t request_id,
    scoped_refptr<base::SequencedTaskRunner> ui_task_runner,
    base::WeakPtr<RuntimeWebAIBackend> backend,
    uint64_t id) {
  if (runtime->IsRequestCancelled(request_id)) {
    return GenerationStatus::kCancelled;
  }
  uint32_t context_tokens = 0;
  if (!EnsureModelLoaded(runtime, &context_tokens)) {
    return GenerationStatus::kFailed;
  }
  if (runtime->IsRequestCancelled(request_id)) {
    return GenerationStatus::kCancelled;
  }

  // The trust labels carry over: system_prompt and context_preamble are
  // MoltBrowser's; context, history and user_message are the page's, which
  // the inference service neutralizes.
  ChatPrompt chat;
  chat.system_prompt = std::move(job.system_prompt);
  chat.context_preamble = std::move(job.context_preamble);
  chat.context = std::move(job.context);
  for (auto& [role, text] : job.history) {
    chat.history.push_back(
        {role == Role::kUser ? "user" : "assistant", std::move(text)});
  }
  chat.user_message = std::move(job.user_message);

  PromptOptions options;
  options.max_tokens =
      static_cast<int>(std::clamp<uint32_t>(job.max_output_tokens, 1, 4096));
  options.max_reasoning_tokens = static_cast<int>(kMaxReasoningTokens);
  options.reasoning = ReasoningMode::kOff;
  options.temperature = job.sampling.temperature;
  options.top_k = static_cast<int>(job.sampling.top_k);
  options.top_p = 0.9f;
  options.stream = true;
  options.request_id = request_id;
  options.conversation_key = job.conversation_key;

  const GenerationResult result = runtime->StreamChat(
      chat,
      [&ui_task_runner, &backend, id](const std::string& text, bool is_done) {
        if (is_done || text.empty()) {
          return;
        }
        // Delivered on the UI thread before OnJobDone (same sequence).
        ui_task_runner->PostTask(
            FROM_HERE, base::BindOnce(&RuntimeWebAIBackend::OnJobChunk,
                                      backend, id, text));
      },
      options);
  if (result.was_cancelled) {
    return GenerationStatus::kCancelled;
  }
  return result.success ? GenerationStatus::kOk : GenerationStatus::kFailed;
}

void RuntimeWebAIBackend::OnJobChunk(uint64_t id, std::string text) {
  auto it = jobs_.find(id);
  if (it == jobs_.end() || it->second->cancelled ||
      !it->second->on_chunk) {
    return;
  }
  it->second->on_chunk.Run(text);
}

void RuntimeWebAIBackend::OnJobDone(uint64_t id, GenerationStatus status) {
  scheduler_.Finish(id);
  auto it = jobs_.find(id);
  if (it != jobs_.end()) {
    std::unique_ptr<Job> job = std::move(it->second);
    jobs_.erase(it);
    job->deadline.Stop();
    if (job->request_id != 0) {
      runtime().FinishRequest(job->request_id);
    }
    if (!job->cancelled && job->on_done) {
      if (job->timed_out) {
        status = GenerationStatus::kFailed;
      }
      std::move(job->on_done).Run(status);
    }
  }
  MaybeStartNext();
}

void RuntimeWebAIBackend::OnJobDeadline(uint64_t id) {
  auto it = jobs_.find(id);
  if (it == jobs_.end() || it->second->request_id == 0) {
    return;
  }
  it->second->timed_out = true;
  runtime().CancelRequest(it->second->request_id);
}

// ---- Sessions ----

bool RuntimeWebAIBackend::TryAddSession(const std::string& site_key) {
  return scheduler_.TryAddSession(site_key);
}

void RuntimeWebAIBackend::RemoveSession(const std::string& site_key) {
  scheduler_.RemoveSession(site_key);
}

}  // namespace molt_ai::web_ai
