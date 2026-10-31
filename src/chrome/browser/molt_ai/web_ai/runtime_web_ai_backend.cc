// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/runtime_web_ai_backend.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/molt_ai/runtime/model_image_prep.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_settings_switch.h"

namespace molt_ai::web_ai {

namespace {

constexpr base::TaskTraits kWorkerTraits = {
    base::MayBlock(), base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN};

struct WebAISettings {
  // The chat's default model ("default_model").
  std::string default_model;
  // "web_ai_apis": false turns the APIs off for every site.
  bool web_apis_enabled = true;
};

// Scaling a page's bitmaps: CPU only, no I/O.
constexpr base::TaskTraits kImageTraits = {
    base::TaskPriority::USER_VISIBLE,
    base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN};

// A page's bitmaps as model images (kWebImageMaxSide), or nullopt if any
// can't be made. Worker thread.
std::optional<std::vector<ModelImage>> PrepareImagesOnWorker(
    std::vector<SkBitmap> bitmaps) {
  std::vector<ModelImage> images;
  images.reserve(bitmaps.size());
  for (SkBitmap& bitmap : bitmaps) {
    std::optional<ModelImage> image =
        PrepareImageForModel(bitmap, kWebImageMaxSide);
    if (!image) {
      return std::nullopt;
    }
    images.push_back(std::move(*image));
    // The page's pixels go as soon as they are scaled.
    bitmap.reset();
  }
  return images;
}

// ~/.moltbrowser/settings.json, as the chat reads it. Worker thread.
WebAISettings ReadWebAISettings() {
  WebAISettings settings;
  // settings.json as its one writer reads it (common/molt_settings_file.h):
  // a file that can't be read leaves the APIs off (WebAIApisSwitchFrom()).
  const std::optional<base::DictValue> dict = ReadMoltSettings();
  settings.web_apis_enabled = WebAIApisSwitchFrom(dict);
  if (!dict) {
    return settings;
  }
  if (const std::string* model = dict->FindString("default_model")) {
    settings.default_model = *model;
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
    c.sees_images = info.sees_images;
    c.hears_audio = info.hears_audio;
    out.push_back(std::move(c));
  }
  return out;
}

// Now, for SiteDeviceFacts.
int64_t NowMicroseconds() {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMicroseconds();
}

// Loads the model pages use if none is loaded (one that sees images, if
// |wants_images|, and hears audio, if |wants_audio|, and one is
// downloaded). Worker thread (blocks while the model loads).
bool EnsureModelLoaded(BrowserAIRuntime* runtime,
                       uint32_t* context_tokens,
                       bool wants_images,
                       bool wants_audio) {
  if (!runtime->GetLocalInferenceUnsupportedReason().empty()) {
    return false;
  }
  if (runtime->GetLoadedModelId().empty()) {
    runtime->RefreshModelStatus();
    const std::string model_id = PickWebModel(
        /*loaded_model_id=*/"", ReadWebAISettings().default_model,
        Candidates(*runtime), wants_images, wants_audio);
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
  // Site keys are never empty.
  GetDeviceFactsForSite(std::string(), std::move(callback));
}

void RuntimeWebAIBackend::GetDeviceFactsForSite(const std::string& site_key,
                                                DeviceCallback callback) {
  // Per site (WEB_AI_APIS.md §8.1): a site is answered from its own facts,
  // or waits for a read of its own, never for or from another site's.
  const SiteDeviceFacts::Lookup lookup =
      site_facts_.Find(site_key, NowMicroseconds());
  switch (lookup.kind) {
    case SiteDeviceFacts::Lookup::Kind::kCached:
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), lookup.facts));
      return;
    case SiteDeviceFacts::Lookup::Kind::kJoin:
      device_waiters_[lookup.ticket].push_back(std::move(callback));
      return;
    case SiteDeviceFacts::Lookup::Kind::kRead:
      break;
  }
  device_waiters_[lookup.ticket].push_back(std::move(callback));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kWorkerTraits,
      base::BindOnce(&RuntimeWebAIBackend::ReadDeviceFacts,
                     base::Unretained(&runtime())),
      base::BindOnce(&RuntimeWebAIBackend::OnDeviceFacts,
                     weak_factory_.GetWeakPtr(), site_key, lookup.ticket));
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
  facts.audio_model_offered = AnyModelHearsAudio(models);
  facts.audio_model_installed = AnyInstalledModelHearsAudio(models);
  return facts;
}

bool RuntimeWebAIBackend::WebApisSwitchOn() {
  return web_apis_switch_;
}

void RuntimeWebAIBackend::SetWebApisSwitch(bool enabled) {
  InvalidateDeviceFacts();
  UpdateWebApisSwitch(enabled);
}

void RuntimeWebAIBackend::InvalidateDeviceFacts() {
  // Reads in flight still answer their waiters, but aren't kept.
  site_facts_.Clear();
}

base::CallbackListSubscription RuntimeWebAIBackend::ObserveWebApisSwitch(
    base::RepeatingClosure on_change) {
  return web_apis_switch_observers_.Add(std::move(on_change));
}

void RuntimeWebAIBackend::UpdateWebApisSwitch(bool enabled) {
  if (web_apis_switch_ == enabled) {
    return;
  }
  web_apis_switch_ = enabled;
  web_apis_switch_observers_.Notify();
}

void RuntimeWebAIBackend::OnDeviceFacts(const std::string& site_key,
                                        uint64_t ticket,
                                        DeviceFacts facts) {
  // A read that started before the switch was set or the facts were
  // invalidated is neither kept nor taken as the switch's value.
  if (site_facts_.Finish(site_key, ticket, facts, NowMicroseconds()) &&
      ticket > web_apis_switch_ticket_) {
    web_apis_switch_ticket_ = ticket;
    UpdateWebApisSwitch(facts.web_apis_enabled);
  }
  std::vector<DeviceCallback> waiters;
  auto it = device_waiters_.find(ticket);
  if (it != device_waiters_.end()) {
    waiters.swap(it->second);
    device_waiters_.erase(it);
  }
  for (DeviceCallback& callback : waiters) {
    std::move(callback).Run(facts);
  }
}

// ---- Model ----

void RuntimeWebAIBackend::PrepareModel(bool wants_images,
                                       bool wants_audio,
                                       PrepareCallback callback) {
  prepare_waiters_.push_back(std::move(callback));
  if (prepare_pending_) {
    // Joins the load in flight (a preference, not a requirement: create()
    // checks what was loaded).
    return;
  }
  prepare_pending_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kWorkerTraits,
      base::BindOnce(&RuntimeWebAIBackend::PrepareBlocking,
                     base::Unretained(&runtime()), wants_images, wants_audio),
      base::BindOnce(&RuntimeWebAIBackend::OnModelPrepared,
                     weak_factory_.GetWeakPtr()));
}

// static
RuntimeWebAIBackend::PrepareResult RuntimeWebAIBackend::PrepareBlocking(
    BrowserAIRuntime* runtime,
    bool wants_images,
    bool wants_audio) {
  PrepareResult result;
  result.ok = EnsureModelLoaded(runtime, &result.context_tokens, wants_images,
                                wants_audio);
  result.sees_images = result.ok && runtime->LoadedModelSeesImages();
  result.hears_audio = result.ok && runtime->LoadedModelHearsAudio();
  return result;
}

void RuntimeWebAIBackend::OnModelPrepared(PrepareResult result) {
  prepare_pending_ = false;
  // The device facts stay: loading a model changes none of them, and
  // dropping them here would make every other site's next call wait for a
  // read because this one loaded a model.
  std::vector<PrepareCallback> waiters;
  waiters.swap(prepare_waiters_);
  for (PrepareCallback& callback : waiters) {
    std::move(callback).Run(result.ok, result.context_tokens,
                            result.sees_images, result.hears_audio);
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
  bool wants_images = !job.user_images.empty();
  for (const std::vector<ModelImage>& images : job.history_images) {
    wants_images = wants_images || !images.empty();
  }
  bool wants_audio = !job.user_audio.empty();
  for (const std::vector<ModelAudio>& clips : job.history_audio) {
    wants_audio = wants_audio || !clips.empty();
  }
  uint32_t context_tokens = 0;
  if (!EnsureModelLoaded(runtime, &context_tokens, wants_images, wants_audio)) {
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
  for (size_t i = 0; i < job.history.size(); ++i) {
    auto& [role, text] = job.history[i];
    ChatMessage message;
    message.role = role == Role::kUser ? "user" : "assistant";
    message.content = std::move(text);
    // Images and audio go with user turns only (the runtime ignores
    // others').
    if (role == Role::kUser && i < job.history_images.size()) {
      message.images = std::move(job.history_images[i]);
    }
    if (role == Role::kUser && i < job.history_audio.size()) {
      message.audio = std::move(job.history_audio[i]);
    }
    chat.history.push_back(std::move(message));
  }
  chat.user_message = std::move(job.user_message);
  chat.user_images = std::move(job.user_images);
  chat.user_audio = std::move(job.user_audio);

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
  // Enforced by the inference service (constrained decoding); the reply is
  // checked to be complete JSON before it counts as a success.
  options.json_schema = std::move(job.json_schema);

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
  if (result.success) {
    return GenerationStatus::kOk;
  }
  if (result.constraint_unsupported) {
    return GenerationStatus::kConstraintUnsupported;
  }
  if (result.invalid_output) {
    return result.truncated ? GenerationStatus::kOutputTruncated
                            : GenerationStatus::kInvalidOutput;
  }
  return GenerationStatus::kFailed;
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

// ---- Images ----

void RuntimeWebAIBackend::PrepareImages(std::vector<SkBitmap> bitmaps,
                                        ImagesCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, kImageTraits,
      base::BindOnce(&PrepareImagesOnWorker, std::move(bitmaps)),
      std::move(callback));
}

bool RuntimeWebAIBackend::TryHoldImages(const std::string& site_key,
                                        size_t count) {
  return scheduler_.TryHoldImages(site_key, count);
}

void RuntimeWebAIBackend::ReleaseImages(const std::string& site_key,
                                        size_t count) {
  scheduler_.ReleaseImages(site_key, count);
}

}  // namespace molt_ai::web_ai
