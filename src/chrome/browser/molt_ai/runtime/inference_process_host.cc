// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/inference_process_host.h"

#include <algorithm>
#include <deque>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/process/process_metrics.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "content/public/browser/service_process_info.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

#if BUILDFLAG(IS_MAC)
#include "content/public/browser/browser_child_process_host.h"
#endif

namespace molt_ai {

namespace {

// Kills right away (SIGKILL on POSIX; base sends SIGTERM for other codes):
// the process misbehaved or ignored the request to exit.
constexpr int kKilledExitCode = base::Process::kResultCodeKilledBadMessage;

// Private (anonymous, unshared) memory of |process|, which is what grows if
// the process misbehaves; the read-only model mapping isn't counted. Blocks
// (reads /proc on Linux).
std::optional<uint64_t> SamplePrivateFootprint(base::Process process) {
  if (!process.IsValid()) return std::nullopt;
#if BUILDFLAG(IS_MAC)
  std::unique_ptr<base::ProcessMetrics> metrics =
      base::ProcessMetrics::CreateProcessMetrics(
          process.Handle(),
          content::BrowserChildProcessHost::GetPortProvider());
#else
  std::unique_ptr<base::ProcessMetrics> metrics =
      base::ProcessMetrics::CreateProcessMetrics(process.Handle());
#endif
  if (!metrics) return std::nullopt;
  auto info = metrics->GetMemoryInfo();
  if (!info.has_value()) return std::nullopt;
#if BUILDFLAG(IS_MAC)
  return info->physical_footprint_bytes;
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) || BUILDFLAG(IS_ANDROID)
  return info->rss_anon_bytes + info->vm_swap_bytes;
#elif BUILDFLAG(IS_WIN)
  return info->private_bytes;
#else
  return info->resident_set_bytes;
#endif
}

void LaunchUtilityProcess(
    mojo::PendingReceiver<mojom::MoltInferenceService> receiver,
    base::WeakPtr<content::ServiceProcessHost::Observer> observer,
    base::OnceCallback<void(const base::Process&)> on_process,
    bool use_gpu) {
  // The sandbox (kOnDeviceModelExecution) comes from the interface's
  // [ServiceSandbox] attribute.
  content::ServiceProcessHost::Options options;
  options.WithDisplayName(std::string("On-Device AI Model"))
      .WithObserver(std::move(observer))
      .WithProcessCallback(std::move(on_process));
  if (use_gpu) {
    options.WithExtraCommandLineSwitches({kMoltInferenceGpuSwitch});
  }
  content::ServiceProcessHost::Launch(std::move(receiver), options.Pass());
}

// Kinds of process loss that count as a GPU failure when the GPU was in use
// (GpuFallbackGuard); a protocol violation or a launch failure doesn't.
bool IsGpuFailureKind(InferenceOutcome::Kind kind) {
  return kind == InferenceOutcome::Kind::kProcessGone ||
         kind == InferenceOutcome::Kind::kTimedOut ||
         kind == InferenceOutcome::Kind::kMemoryExceeded;
}

InferenceOutcome::Kind KindForSummary(int32_t status) {
  return status == static_cast<int32_t>(mojom::GenerationStatus::kCancelled)
             ? InferenceOutcome::Kind::kCancelled
             : InferenceOutcome::Kind::kOk;
}

ReportedGpuStatus ToReported(mojom::GpuStatus status) {
  switch (status) {
    case mojom::GpuStatus::kNotRequested:
      return ReportedGpuStatus::kNotRequested;
    case mojom::GpuStatus::kNotBuilt:
      return ReportedGpuStatus::kNotBuilt;
    case mojom::GpuStatus::kNoDevice:
      return ReportedGpuStatus::kNoDevice;
    case mojom::GpuStatus::kNoUsableDevice:
      return ReportedGpuStatus::kNoUsableDevice;
    case mojom::GpuStatus::kTooLittleMemory:
      return ReportedGpuStatus::kTooLittleMemory;
    case mojom::GpuStatus::kLoadFailed:
      return ReportedGpuStatus::kLoadFailed;
    case mojom::GpuStatus::kOn:
      return ReportedGpuStatus::kOn;
    case mojom::GpuStatus::kTurnedOff:
      return ReportedGpuStatus::kTurnedOff;
    case mojom::GpuStatus::kDeviceLost:
      return ReportedGpuStatus::kDeviceLost;
  }
  return ReportedGpuStatus::kNotRequested;
}

// The service's device report, checked (lengths) and sanitized; nullopt for
// one that breaks the mojom's caps.
std::optional<GpuDeviceReport> ToReport(const mojom::GpuDeviceIdentity& device) {
  if (!device.key || device.key->driver_uuid.size() != 16 ||
      device.pipeline_cache_uuid.size() != kPipelineCacheUuidBytes ||
      device.name.size() > mojom::kMaxGpuNameBytes) {
    return std::nullopt;
  }
  GpuDeviceReport report;
  report.key.identified = true;
  report.key.vendor_id = device.key->vendor_id;
  report.key.device_id = device.key->device_id;
  report.key.driver_id = device.key->driver_id;
  report.key.driver_version = device.key->driver_version;
  std::copy(device.key->driver_uuid.begin(), device.key->driver_uuid.end(),
            report.key.driver_uuid.begin());
  report.cache_device.vendor_id = device.key->vendor_id;
  report.cache_device.device_id = device.key->device_id;
  std::copy(device.pipeline_cache_uuid.begin(),
            device.pipeline_cache_uuid.end(), report.cache_device.uuid.begin());
  report.api_version = device.api_version;
  report.name = SanitizeGpuName(device.name);
  return report;
}

mojom::GpuDriverKeyPtr ToMojo(const GpuDriverKey& key) {
  return mojom::GpuDriverKey::New(
      key.vendor_id, key.device_id, key.driver_id, key.driver_version,
      std::vector<uint8_t>(key.driver_uuid.begin(), key.driver_uuid.end()));
}

}  // namespace

GpuDeviceReport::GpuDeviceReport() = default;
GpuDeviceReport::~GpuDeviceReport() = default;
GpuDeviceReport::GpuDeviceReport(const GpuDeviceReport&) = default;
GpuDeviceReport& GpuDeviceReport::operator=(const GpuDeviceReport&) = default;

// ---- InferenceOutcome / InferenceMailbox -----------------------------------

InferenceOutcome::InferenceOutcome() = default;
InferenceOutcome::InferenceOutcome(Kind k) : kind(k) {}
InferenceOutcome::~InferenceOutcome() = default;
InferenceOutcome::InferenceOutcome(const InferenceOutcome&) = default;
InferenceOutcome& InferenceOutcome::operator=(const InferenceOutcome&) =
    default;

InferenceMailbox::InferenceMailbox() : heartbeat_(Clock::now()) {}
InferenceMailbox::~InferenceMailbox() = default;

void InferenceMailbox::PushText(std::string_view text) {
  if (text.empty()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_) return;
    text_.append(text);
    heartbeat_ = Clock::now();
  }
  cv_.notify_all();
}

void InferenceMailbox::Heartbeat() {
  std::lock_guard<std::mutex> lock(mutex_);
  heartbeat_ = Clock::now();
}

void InferenceMailbox::Finish(InferenceOutcome outcome) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_) return;
    finished_ = true;
    outcome_ = std::move(outcome);
    heartbeat_ = Clock::now();
  }
  cv_.notify_all();
}

bool InferenceMailbox::Wait(std::chrono::milliseconds timeout,
                            std::string* text) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout,
               [&]() { return finished_ || woken_ || !text_.empty(); });
  woken_ = false;
  if (text) {
    text->append(text_);
  }
  text_.clear();
  return finished_;
}

void InferenceMailbox::Wake() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    woken_ = true;
  }
  cv_.notify_all();
}

InferenceOutcome InferenceMailbox::outcome() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outcome_;
}

InferenceMailbox::Clock::time_point InferenceMailbox::last_heartbeat() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return heartbeat_;
}

InferenceProcessHost::LoadRequest::LoadRequest() = default;
InferenceProcessHost::LoadRequest::~LoadRequest() = default;
InferenceProcessHost::LoadRequest::LoadRequest(LoadRequest&&) = default;
InferenceProcessHost::LoadRequest&
InferenceProcessHost::LoadRequest::operator=(LoadRequest&&) = default;

// ---- ModelProcess -----------------------------------------------------------

class InferenceProcessHost::ModelProcess
    : public content::ServiceProcessHost::Observer,
      public mojom::GpuLoadClient {
 public:
  enum class State { kLoading, kReady, kRetired, kEnding };

  ModelProcess(InferenceProcessHost* host,
               std::string model_id,
               std::string crash_key,
               uint32_t max_context_tokens,
               uint64_t footprint_limit_bytes,
               bool vision_projector_sent,
               bool gpu_requested)
      : host_(host),
        model_id_(std::move(model_id)),
        crash_key_(std::move(crash_key)),
        max_context_tokens_(max_context_tokens),
        footprint_limit_bytes_(footprint_limit_bytes),
        vision_projector_sent_(vision_projector_sent),
        gpu_requested_(gpu_requested) {}
  ~ModelProcess() override = default;

  // content::ServiceProcessHost::Observer (UI thread):
  void OnServiceProcessTerminatedNormally(
      const content::ServiceProcessInfo& info) override {
    host_->OnProcessExited(this, /*crashed_before_ipc=*/false);
  }
  void OnServiceProcessCrashed(
      const content::ServiceProcessInfo& info) override {
    host_->OnProcessExited(this, info.crashed_pre_ipc().value_or(false));
  }

  // mojom::GpuLoadClient (UI thread):
  void OnGpuChosen(mojom::GpuDeviceIdentityPtr device,
                   OnGpuChosenCallback callback) override {
    host_->OnGpuChosen(this, std::move(device), std::move(callback));
  }

  base::WeakPtr<ModelProcess> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  const std::string& model_id() const { return model_id_; }
  const std::string& crash_key() const { return crash_key_; }
  // The cap the browser sent; a described context above it is a lie.
  uint32_t max_context_tokens() const { return max_context_tokens_; }
  uint64_t footprint_limit_bytes() const { return footprint_limit_bytes_; }
  // A process given no projector that claims vision is lying.
  bool vision_projector_sent() const { return vision_projector_sent_; }
  // Launched for the GPU; one not asked to use it that claims it is lying.
  bool gpu_requested() const { return gpu_requested_; }
  // Its loss is a GPU failure: it was loading with the GPU requested (and
  // the browser hadn't said no to the device it reported), or runs on the
  // GPU.
  bool gpu_in_use() const {
    return gpu_requested_ &&
           ((state == State::kLoading && !(gpu_answered && !gpu_allowed)) ||
            on_gpu);
  }

  State state = State::kLoading;
  // The model runs on the GPU (backend "vulkan").
  bool on_gpu = false;
  // GPU loads (design section 11.10): the device the service reported, and
  // the browser's answer.
  mojo::Receiver<mojom::GpuLoadClient> gpu_client{this};
  std::optional<GpuDeviceReport> gpu_device;
  bool gpu_answered = false;
  bool gpu_allowed = false;
  // Pipeline caches still to ask for (section 11.12).
  int pipeline_cache_requests_left = kMaxPipelineCacheRequests;
  mojo::Remote<mojom::MoltInferenceService> service;
  mojo::Remote<mojom::Model> model;
  base::Process process;
  uint32_t context_tokens = 0;
  uint32_t vocab_size = 0;
  scoped_refptr<InferenceMailbox> load_mailbox;
  std::map<uint64_t, std::unique_ptr<Generation>> generations;
  // Tokens in the order the service received them. It serves them one at a
  // time, so only the first is running; the others have no clock yet.
  std::deque<uint64_t> queue_order;
  std::vector<scoped_refptr<InferenceMailbox>> exit_waiters;
  // Set once killed or lost: no further crash is recorded for it.
  bool lost = false;
  base::OneShotTimer load_deadline;
  base::OneShotTimer exit_timer;
  base::RepeatingTimer footprint_timer;

 private:
  raw_ptr<InferenceProcessHost> host_;
  const std::string model_id_;
  const std::string crash_key_;
  const uint32_t max_context_tokens_;
  const uint64_t footprint_limit_bytes_;
  const bool vision_projector_sent_;
  const bool gpu_requested_;
  base::WeakPtrFactory<ModelProcess> weak_factory_{this};
};

// ---- Generation (the browser's GenerationStreamer) ---------------------------

class InferenceProcessHost::Generation : public mojom::GenerationStreamer {
 public:
  Generation(InferenceProcessHost* host,
             ModelProcess* process,
             uint64_t token,
             StreamLimits limits,
             scoped_refptr<InferenceMailbox> mailbox,
             int64_t now_ms)
      : host_(host),
        process_(process),
        token_(token),
        validator_(std::move(limits), now_ms),
        mailbox_(std::move(mailbox)),
        receiver_(this) {}
  ~Generation() override = default;

  mojo::PendingRemote<mojom::GenerationStreamer> Bind() {
    mojo::PendingRemote<mojom::GenerationStreamer> remote =
        receiver_.BindNewPipeAndPassRemote();
    // Closed without OnComplete: the process died or broke the protocol.
    // (A crash closes every pipe; whichever notification comes first ends
    // the process and fails its requests.)
    receiver_.set_disconnect_handler(base::BindOnce(
        &Generation::OnDisconnected, base::Unretained(this)));
    return remote;
  }

  // mojom::GenerationStreamer:
  void OnProgress(uint32_t done_tokens, uint32_t total_tokens) override {
    StreamValidator::ProgressResult r =
        validator_.OnProgress(done_tokens, total_tokens, host_->NowMs());
    if (r.violation != StreamViolation::kNone) {
      Violation(r.violation);
      return;
    }
    mailbox_->Heartbeat();
  }

  void OnChunk(const std::string& text) override {
    StreamValidator::ChunkResult r = validator_.OnChunk(text, host_->NowMs());
    if (r.violation != StreamViolation::kNone) {
      Violation(r.violation);
      return;
    }
    mailbox_->PushText(r.visible);
    mailbox_->Heartbeat();
    if (r.stopped) {
      // Our copy of the stop filter matched: the request is done for the
      // caller. Closing the streamer (destroying this) cancels it in the
      // service.
      InferenceOutcome outcome(InferenceOutcome::Kind::kOk);
      outcome.stopped_by_browser = true;
      host_->FinishGeneration(process_, token_, std::move(outcome));
    }
  }

  void OnComplete(mojom::GenerationSummaryPtr summary) override {
    if (!summary) {
      Violation(StreamViolation::kMessageAfterComplete);
      return;
    }
    StreamSummary s;
    s.status = static_cast<int32_t>(summary->status);
    s.prompt_tokens = summary->prompt_tokens;
    s.reused_tokens = summary->reused_tokens;
    s.generated_tokens = summary->generated_tokens;
    s.history_dropped = summary->history_dropped;
    s.context_trimmed = summary->context_trimmed;
    s.user_message_trimmed = summary->user_message_trimmed;
    s.backend_failed =
        summary->status == mojom::GenerationStatus::kBackendFailed;
    StreamValidator::CompleteResult r =
        validator_.OnComplete(s, host_->NowMs());
    if (r.violation != StreamViolation::kNone) {
      Violation(r.violation);
      return;
    }
    mailbox_->PushText(r.visible_tail);
    InferenceOutcome outcome(KindForSummary(r.summary.status));
    outcome.has_summary = true;
    outcome.summary = r.summary;
    host_->FinishGeneration(process_, token_, std::move(outcome));
  }

  StreamViolation CheckTime(int64_t now_ms) const {
    return validator_.CheckTime(now_ms);
  }
  void SetQueued() { validator_.SetQueued(); }
  void Activate(int64_t now_ms) { validator_.Activate(now_ms); }

  // Ends the request for the caller (the streamer closes when this is
  // destroyed). Text already delivered stays delivered.
  void Finish(InferenceOutcome outcome) {
    mailbox_->Finish(std::move(outcome));
  }

 private:
  void Violation(StreamViolation violation) {
    // Only during a message dispatch: kills the sender (the utility process)
    // through content's bad-message handling. KillProcess() below also ends
    // it and fails this request.
    LOG(ERROR) << "[MoltAI] inference process: " << StreamViolationName(violation);
    receiver_.ReportBadMessage(StreamViolationName(violation));
    host_->KillProcess(process_, InferenceOutcome::Kind::kViolation,
                       CrashLoopGuard::Phase::kGenerating);
  }

  void OnDisconnected() {
    host_->KillProcess(process_, InferenceOutcome::Kind::kProcessGone,
                       CrashLoopGuard::Phase::kGenerating);
  }

  raw_ptr<InferenceProcessHost> host_;
  raw_ptr<ModelProcess> process_;
  const uint64_t token_;
  StreamValidator validator_;
  scoped_refptr<InferenceMailbox> mailbox_;
  mojo::Receiver<mojom::GenerationStreamer> receiver_;
};

// ---- InferenceProcessHost ----------------------------------------------------

InferenceProcessHost::InferenceProcessHost(Delegate* delegate)
    : delegate_(delegate),
      launcher_(base::BindRepeating(&LaunchUtilityProcess)) {}

InferenceProcessHost::~InferenceProcessHost() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Shutdown();
}

int64_t InferenceProcessHost::NowMs() const {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
}

void InferenceProcessHost::SetLauncherForTesting(Launcher launcher) {
  launcher_ = std::move(launcher);
}

void InferenceProcessHost::SetTimeoutsForTesting(const Timeouts& timeouts) {
  timeouts_ = timeouts;
}

bool InferenceProcessHost::HasCurrentModelForTesting() const {
  return current_ && current_->state == ModelProcess::State::kReady;
}

size_t InferenceProcessHost::ProcessCountForTesting() const {
  return (current_ ? 1 : 0) + (loading_ ? 1 : 0) + retired_.size();
}

void InferenceProcessHost::Load(LoadRequest request,
                                scoped_refptr<InferenceMailbox> mailbox) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    mailbox->Finish(InferenceOutcome(InferenceOutcome::Kind::kShuttingDown));
    return;
  }
  if (loading_) {
    // BrowserAIRuntime serializes loads; a leftover is abandoned.
    if (loading_->load_mailbox) {
      loading_->load_mailbox->Finish(
          InferenceOutcome(InferenceOutcome::Kind::kNoModel));
      loading_->load_mailbox = nullptr;
    }
    ModelProcess* stale = loading_.get();
    retired_.push_back(std::move(loading_));
    EndProcess(stale, /*terminate_now=*/true);
  }

  auto process = std::make_unique<ModelProcess>(
      this, request.model_id, request.crash_key, request.max_context_tokens,
      request.footprint_limit_bytes, request.vision_projector.IsValid(),
      request.use_gpu);
  ModelProcess* p = process.get();
  p->load_mailbox = std::move(mailbox);
  launcher_.Run(p->service.BindNewPipeAndPassReceiver(), p->GetWeakPtr(),
                base::BindOnce(&InferenceProcessHost::OnProcessLaunched,
                               weak_factory_.GetWeakPtr(), p->GetWeakPtr()),
                request.use_gpu);
  p->service.set_disconnect_handler(
      base::BindOnce(&InferenceProcessHost::OnProcessDisconnected,
                     weak_factory_.GetWeakPtr(), p->GetWeakPtr()));

  auto params = mojom::ModelLoadParams::New();
  params->weights = std::move(request.file);
  params->file_size = request.file_size;
  params->total_ram_bytes = request.total_ram_bytes;
  params->cpu_threads = request.cpu_threads;
  params->max_context_tokens = request.max_context_tokens;
  params->allow_cpu_repack = request.allow_cpu_repack;
  params->use_gpu = request.use_gpu;
  if (request.use_gpu) {
    // Asked about the device it chooses before it loads anything on it
    // (design section 11.10).
    params->gpu_client = p->gpu_client.BindNewPipeAndPassRemote();
    for (const GpuDriverKey& key : request.skip_gpu_drivers) {
      if (params->skip_gpu_drivers.size() >= mojom::kMaxSkippedGpuDrivers) {
        break;
      }
      if (key.identified) params->skip_gpu_drivers.push_back(ToMojo(key));
    }
  }
  if (request.vision_projector.IsValid()) {
    params->vision_projector = std::move(request.vision_projector);
    params->vision_projector_size = request.vision_projector_size;
  }
  p->service->LoadModel(
      std::move(params), p->model.BindNewPipeAndPassReceiver(),
      base::BindOnce(&InferenceProcessHost::OnLoadReply,
                     weak_factory_.GetWeakPtr(), p->GetWeakPtr()));
  p->model.set_disconnect_handler(
      base::BindOnce(&InferenceProcessHost::OnProcessDisconnected,
                     weak_factory_.GetWeakPtr(), p->GetWeakPtr()));
  p->load_deadline.Start(
      FROM_HERE, timeouts_.load,
      base::BindOnce(&InferenceProcessHost::OnLoadTimeout,
                     weak_factory_.GetWeakPtr(), p->GetWeakPtr()));
  loading_ = std::move(process);
}

void InferenceProcessHost::OnProcessLaunched(
    base::WeakPtr<ModelProcess> process,
    const base::Process& handle) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process) return;
  process->process = handle.Duplicate();
  if (process->footprint_limit_bytes() > 0) {
    process->footprint_timer.Start(
        FROM_HERE, timeouts_.footprint_period,
        base::BindRepeating(&InferenceProcessHost::OnFootprintTimer,
                            weak_factory_.GetWeakPtr(), process));
  }
}

void InferenceProcessHost::OnLoadReply(base::WeakPtr<ModelProcess> process,
                                       mojom::LoadModelStatus status,
                                       mojom::ModelDescriptionPtr description) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || process.get() != loading_.get() ||
      process->state != ModelProcess::State::kLoading) {
    return;
  }
  ModelProcess* p = process.get();
  p->load_deadline.Stop();
  scoped_refptr<InferenceMailbox> mailbox = std::move(p->load_mailbox);

  if (status != mojom::LoadModelStatus::kOk) {
    InferenceOutcome outcome(InferenceOutcome::Kind::kLoadFailed);
    outcome.load_status = status;
    mailbox->Finish(std::move(outcome));
    retired_.push_back(std::move(loading_));
    EndProcess(p, /*terminate_now=*/false);
    return;
  }
  // A description is required with kOk and must be sane (it decides limits
  // and is shown in logs).
  if (!description ||
      !IsValidModelDescription(description->architecture,
                               description->backend,
                               description->context_tokens,
                               description->vocab_size,
                               mojom::kMinDescribedContextTokens,
                               p->max_context_tokens()) ||
      (description->vision && !p->vision_projector_sent()) ||
      !IsValidGpuDescription(
          p->gpu_requested(), description->backend,
          description->gpu_status == mojom::GpuStatus::kOn,
          description->gpu_layers, mojom::kMaxGpuLayers) ||
      !IsConsistentGpuStatus(
          {p->gpu_requested(), p->gpu_device.has_value(),
           p->gpu_answered && p->gpu_allowed},
          ToReported(description->gpu_status))) {
    mojo::ReportBadMessage("invalid ModelDescription");
    p->load_mailbox = std::move(mailbox);  // failed by KillProcess
    KillProcess(p, InferenceOutcome::Kind::kViolation,
                CrashLoopGuard::Phase::kLoading);
    return;
  }

  p->state = ModelProcess::State::kReady;
  p->on_gpu = description->gpu_status == mojom::GpuStatus::kOn;
  p->context_tokens = description->context_tokens;
  p->vocab_size = description->vocab_size;
  InferenceOutcome outcome(InferenceOutcome::Kind::kOk);
  outcome.architecture = description->architecture;
  outcome.backend = description->backend;
  outcome.context_tokens = description->context_tokens;
  outcome.vocab_size = description->vocab_size;
  outcome.cpu_repack_used = description->cpu_repack_used;
  outcome.vision = description->vision;
  outcome.gpu_status = description->gpu_status;
  outcome.gpu_layers = description->gpu_layers;
  outcome.gpu_device = p->gpu_device;
  // The question is answered; the pipe isn't used again.
  p->gpu_client.reset();

  // Swap: the previous model finishes what it is running, then ends.
  if (current_) {
    ModelProcess* old = current_.get();
    retired_.push_back(std::move(current_));
    Retire(old);
  }
  current_ = std::move(loading_);
  mailbox->Finish(std::move(outcome));
}

void InferenceProcessHost::OnGpuChosen(
    ModelProcess* process,
    mojom::GpuDeviceIdentityPtr device,
    mojom::GpuLoadClient::OnGpuChosenCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (process->state != ModelProcess::State::kLoading || process->lost) {
    return;  // the pipe closes with the process
  }
  // Once per load, only when the GPU was asked for, within the caps.
  std::optional<GpuDeviceReport> report =
      device && process->gpu_requested() && !process->gpu_device
          ? ToReport(*device)
          : std::nullopt;
  if (!report) {
    process->gpu_client.ReportBadMessage("invalid OnGpuChosen");
    KillProcess(process, InferenceOutcome::Kind::kViolation,
                CrashLoopGuard::Phase::kLoading);
    return;
  }
  process->gpu_device = *report;
  if (!delegate_) {
    std::move(callback).Run(false, base::File());
    process->gpu_answered = true;
    return;
  }
  delegate_->OnGpuDeviceChosen(
      *report,
      base::BindOnce(&InferenceProcessHost::OnGpuAnswer,
                     weak_factory_.GetWeakPtr(), process->GetWeakPtr(),
                     std::move(callback)));
}

void InferenceProcessHost::OnGpuAnswer(
    base::WeakPtr<ModelProcess> process,
    mojom::GpuLoadClient::OnGpuChosenCallback callback,
    bool use_gpu,
    base::File pipeline_cache) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Gone, or no longer loading: the callback's pipe closes with it.
  if (!process || process->state != ModelProcess::State::kLoading ||
      process->lost || !process->gpu_client.is_bound()) {
    return;
  }
  process->gpu_answered = true;
  process->gpu_allowed = use_gpu;
  std::move(callback).Run(use_gpu,
                          use_gpu ? std::move(pipeline_cache) : base::File());
}

void InferenceProcessHost::MaybeRequestPipelineCache(ModelProcess* process) {
  if (!process->on_gpu || !process->gpu_device ||
      process->pipeline_cache_requests_left <= 0 ||
      process->state != ModelProcess::State::kReady || !process->model) {
    return;
  }
  --process->pipeline_cache_requests_left;
  process->model->GetPipelineCache(
      base::BindOnce(&InferenceProcessHost::OnPipelineCache,
                     weak_factory_.GetWeakPtr(), process->GetWeakPtr()));
}

void InferenceProcessHost::OnPipelineCache(
    base::WeakPtr<ModelProcess> process,
    std::optional<mojo_base::BigBuffer> cache) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || process->lost || !process->gpu_device || !cache) {
    return;  // nothing new
  }
  const GpuDeviceReport& device = *process->gpu_device;
  if (cache->size() == 0) {
    // The service's cache grew over the cap: start a new one next time.
    if (delegate_) delegate_->OnPipelineCacheDropped(device);
    return;
  }
  // Checked here, before anything is stored: the size, then the header
  // against the device this process reported. Nothing past the header is
  // read. A failure isn't a protocol violation (a driver may write an odd
  // cache), but nothing more is asked of this process.
  const PipelineCacheCheck check = CheckPipelineCacheHeader(
      base::as_string_view(base::span(*cache)), device.cache_device);
  if (check != PipelineCacheCheck::kOk) {
    LOG(WARNING) << "[MoltAI] inference process: pipeline cache refused ("
                 << PipelineCacheCheckName(check) << ", " << cache->size()
                 << " bytes)";
    process->pipeline_cache_requests_left = 0;
    if (delegate_) delegate_->OnPipelineCacheDropped(device);
    return;
  }
  if (delegate_) delegate_->OnPipelineCacheData(device, std::move(*cache));
}

void InferenceProcessHost::OnLoadTimeout(base::WeakPtr<ModelProcess> process) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || process->state != ModelProcess::State::kLoading) return;
  LOG(ERROR) << "[MoltAI] inference process: load deadline exceeded";
  KillProcess(process.get(), InferenceOutcome::Kind::kTimedOut,
              CrashLoopGuard::Phase::kLoading);
}

void InferenceProcessHost::StartChat(uint64_t token,
                                     mojom::ChatRequestPtr request,
                                     mojom::GenerationParamsPtr params,
                                     StreamLimits limits,
                                     scoped_refptr<InferenceMailbox> mailbox) {
  StartGeneration(token, /*is_chat=*/true, std::move(request), nullptr,
                  std::move(params), std::move(limits), std::move(mailbox));
}

void InferenceProcessHost::StartPrompt(
    uint64_t token,
    mojom::RawPromptRequestPtr request,
    mojom::GenerationParamsPtr params,
    StreamLimits limits,
    scoped_refptr<InferenceMailbox> mailbox) {
  StartGeneration(token, /*is_chat=*/false, nullptr, std::move(request),
                  std::move(params), std::move(limits), std::move(mailbox));
}

void InferenceProcessHost::StartGeneration(
    uint64_t token,
    bool is_chat,
    mojom::ChatRequestPtr chat,
    mojom::RawPromptRequestPtr raw,
    mojom::GenerationParamsPtr params,
    StreamLimits limits,
    scoped_refptr<InferenceMailbox> mailbox) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    mailbox->Finish(InferenceOutcome(InferenceOutcome::Kind::kShuttingDown));
    return;
  }
  if (!current_ || current_->state != ModelProcess::State::kReady ||
      FindGenerationOwner(token)) {
    mailbox->Finish(InferenceOutcome(InferenceOutcome::Kind::kNoModel));
    return;
  }
  ModelProcess* p = current_.get();
  limits.context_tokens = p->context_tokens;
  // A GPU failure is believed only from a model on the GPU.
  limits.model_on_gpu = p->on_gpu;
  limits.stall_timeout_ms = timeouts_.stall.InMilliseconds();
  limits.deadline_ms = timeouts_.generation.InMilliseconds();
  auto generation = std::make_unique<Generation>(
      this, p, token, std::move(limits), std::move(mailbox), NowMs());
  mojo::PendingRemote<mojom::GenerationStreamer> streamer = generation->Bind();
  if (!p->queue_order.empty()) generation->SetQueued();
  p->queue_order.push_back(token);
  p->generations[token] = std::move(generation);
  if (is_chat) {
    p->model->StreamChat(std::move(chat), std::move(params),
                         std::move(streamer));
  } else {
    p->model->StreamPrompt(std::move(raw), std::move(params),
                           std::move(streamer));
  }
  UpdateTicking();
}

void InferenceProcessHost::Cancel(uint64_t token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ModelProcess* p = FindGenerationOwner(token);
  if (!p) return;
  FinishGeneration(p, token,
                   InferenceOutcome(InferenceOutcome::Kind::kCancelled));
}

void InferenceProcessHost::FinishGeneration(ModelProcess* process,
                                            uint64_t token,
                                            InferenceOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = process->generations.find(token);
  if (it == process->generations.end()) return;
  // Destroying the Generation closes its streamer pipe: if the service is
  // still working on it, that cancels it.
  std::unique_ptr<Generation> generation = std::move(it->second);
  process->generations.erase(it);
  RemoveFromQueue(process, token);
  // A generation that ran on the GPU compiled pipelines (section 11.12).
  const bool completed_on_gpu = outcome.kind == InferenceOutcome::Kind::kOk &&
                                outcome.has_summary &&
                                !outcome.summary.backend_failed;
  generation->Finish(std::move(outcome));
  generation.reset();
  if (completed_on_gpu) MaybeRequestPipelineCache(process);
  if (process->state == ModelProcess::State::kRetired &&
      process->generations.empty()) {
    EndProcess(process, /*terminate_now=*/false);
  }
  UpdateTicking();
}

void InferenceProcessHost::Unload(UnloadMode mode,
                                  scoped_refptr<InferenceMailbox> exited) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!current_) {
    if (exited) exited->Finish(InferenceOutcome(InferenceOutcome::Kind::kOk));
    return;
  }
  ModelProcess* p = current_.get();
  if (exited) p->exit_waiters.push_back(std::move(exited));
  retired_.push_back(std::move(current_));
  if (mode == UnloadMode::kNow) {
    for (auto& [token, generation] : p->generations) {
      generation->Finish(InferenceOutcome(InferenceOutcome::Kind::kNoModel));
    }
    p->generations.clear();
    EndProcess(p, /*terminate_now=*/false);
  } else {
    Retire(p);
  }
  UpdateTicking();
}

void InferenceProcessHost::Ping(scoped_refptr<InferenceMailbox> mailbox) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  mailbox->Heartbeat();
}

void InferenceProcessHost::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  shutting_down_ = true;
  tick_timer_.Stop();
  std::vector<std::unique_ptr<ModelProcess>> all = std::move(retired_);
  retired_.clear();
  if (current_) all.push_back(std::move(current_));
  if (loading_) all.push_back(std::move(loading_));
  const InferenceOutcome closing(InferenceOutcome::Kind::kShuttingDown);
  for (auto& p : all) {
    if (p->load_mailbox) p->load_mailbox->Finish(closing);
    for (auto& [token, generation] : p->generations) {
      generation->Finish(closing);
    }
    p->generations.clear();
    for (auto& waiter : p->exit_waiters) waiter->Finish(closing);
    p->exit_waiters.clear();
    // Dropping the remotes ends the processes; the browser is going away.
  }
}

void InferenceProcessHost::Retire(ModelProcess* process) {
  process->state = ModelProcess::State::kRetired;
  if (process->generations.empty()) {
    EndProcess(process, /*terminate_now=*/false);
  }
}

void InferenceProcessHost::EndProcess(ModelProcess* process,
                                      bool terminate_now) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  process->state = ModelProcess::State::kEnding;
  process->load_deadline.Stop();
  process->footprint_timer.Stop();
  // Closing both pipes ends the utility process (the service factory drops
  // the instance, the process exits).
  process->model.reset();
  process->service.reset();
  if (terminate_now && process->process.IsValid()) {
    process->process.Terminate(kKilledExitCode, /*wait=*/false);
  }
  // A process that doesn't exit within the grace period is killed; one that
  // still isn't reported gone after another is given up on.
  process->exit_timer.Start(
      FROM_HERE, timeouts_.exit_grace,
      base::BindOnce(&InferenceProcessHost::OnExitTimeout,
                     weak_factory_.GetWeakPtr(), process->GetWeakPtr(),
                     /*terminated=*/terminate_now));
}

void InferenceProcessHost::OnExitTimeout(base::WeakPtr<ModelProcess> process,
                                         bool terminated) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process) return;
  if (!terminated && process->process.IsValid()) {
    process->process.Terminate(kKilledExitCode, /*wait=*/false);
    process->exit_timer.Start(
        FROM_HERE, timeouts_.exit_grace,
        base::BindOnce(&InferenceProcessHost::OnExitTimeout,
                       weak_factory_.GetWeakPtr(), process,
                       /*terminated=*/true));
    return;
  }
  // No exit notification (e.g. it never launched): stop waiting.
  DestroyNow(process.get());
}

CrashLoopGuard::Phase InferenceProcessHost::PhaseOf(
    const ModelProcess* process) const {
  if (process->state == ModelProcess::State::kLoading) {
    return CrashLoopGuard::Phase::kLoading;
  }
  return process->generations.empty() ? CrashLoopGuard::Phase::kIdle
                                       : CrashLoopGuard::Phase::kGenerating;
}

void InferenceProcessHost::KillProcess(ModelProcess* process,
                                       InferenceOutcome::Kind kind,
                                       CrashLoopGuard::Phase phase) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (process->lost || process->state == ModelProcess::State::kEnding) {
    return;
  }
  process->lost = true;
  // Before it changes state below. A death while idle (the OS reclaiming
  // memory, say) counts for neither guard.
  const bool gpu_failure = process->gpu_in_use() && IsGpuFailureKind(kind) &&
                           phase != CrashLoopGuard::Phase::kIdle;
  const InferenceOutcome outcome(kind);
  if (process->load_mailbox) {
    process->load_mailbox->Finish(outcome);
    process->load_mailbox = nullptr;
  }
  // Fail every request on it; destroying the Generations closes the pipes.
  std::map<uint64_t, std::unique_ptr<Generation>> generations =
      std::move(process->generations);
  process->generations.clear();
  process->queue_order.clear();
  for (auto& [token, generation] : generations) generation->Finish(outcome);
  generations.clear();

  // Move it out of the current/loading slots.
  if (current_.get() == process) {
    retired_.push_back(std::move(current_));
  } else if (loading_.get() == process) {
    retired_.push_back(std::move(loading_));
  }
  const bool misbehaved = kind != InferenceOutcome::Kind::kProcessGone &&
                          kind != InferenceOutcome::Kind::kLaunchFailed;
  EndProcess(process, /*terminate_now=*/misbehaved);
  // Always reported (the runtime forgets the model); the guard only counts
  // kLoading / kGenerating, and a GPU failure counts for the GPU instead.
  if (delegate_) {
    delegate_->OnModelProcessLost(process->model_id(), process->crash_key(),
                                  phase, gpu_failure, process->gpu_device);
  }
  UpdateTicking();
}

void InferenceProcessHost::OnProcessDisconnected(
    base::WeakPtr<ModelProcess> process) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || process->state == ModelProcess::State::kEnding) return;
  // The service closed a pipe we didn't: it died (or misbehaved).
  OnProcessLost(process.get());
}

void InferenceProcessHost::OnProcessLost(ModelProcess* process) {
  if (process->state == ModelProcess::State::kLoading &&
      !process->process.IsValid()) {
    // It never started: not the model's fault, so not a crash for the
    // crash-loop guard.
    KillProcess(process, InferenceOutcome::Kind::kLaunchFailed,
                CrashLoopGuard::Phase::kIdle);
    return;
  }
  KillProcess(process, InferenceOutcome::Kind::kProcessGone,
              PhaseOf(process));
}

void InferenceProcessHost::OnProcessExited(ModelProcess* process,
                                           bool crashed_before_ipc) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (process->state != ModelProcess::State::kEnding) {
    // Exited without being asked to (a crash, the OOM killer, a failed
    // start).
    if (crashed_before_ipc) {
      KillProcess(process, InferenceOutcome::Kind::kLaunchFailed,
                  CrashLoopGuard::Phase::kIdle);
    } else {
      OnProcessLost(process);
    }
  }
  // The observer is notified from content code: destroy it afterwards.
  DestroySoon(process);
}

void InferenceProcessHost::DestroySoon(ModelProcess* process) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<InferenceProcessHost> host,
             base::WeakPtr<ModelProcess> p) {
            if (host && p) host->DestroyNow(p.get());
          },
          weak_factory_.GetWeakPtr(), process->GetWeakPtr()));
}

void InferenceProcessHost::DestroyNow(ModelProcess* process) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (auto& waiter : process->exit_waiters) {
    waiter->Finish(InferenceOutcome(InferenceOutcome::Kind::kOk));
  }
  process->exit_waiters.clear();
  if (current_.get() == process || loading_.get() == process) {
    return;  // never: only ending processes are destroyed
  }
  auto it = std::find_if(
      retired_.begin(), retired_.end(),
      [&](const std::unique_ptr<ModelProcess>& p) { return p.get() == process; });
  if (it != retired_.end()) retired_.erase(it);
}

void InferenceProcessHost::OnFootprintTimer(
    base::WeakPtr<ModelProcess> process) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || !process->process.IsValid() ||
      process->state == ModelProcess::State::kEnding) {
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&SamplePrivateFootprint, process->process.Duplicate()),
      base::BindOnce(&InferenceProcessHost::OnFootprintSample,
                     weak_factory_.GetWeakPtr(), process));
}

void InferenceProcessHost::OnFootprintSample(
    base::WeakPtr<ModelProcess> process,
    std::optional<uint64_t> footprint) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!process || !footprint || process->footprint_limit_bytes() == 0 ||
      *footprint <= process->footprint_limit_bytes()) {
    return;
  }
  LOG(ERROR) << "[MoltAI] inference process: private footprint "
             << (*footprint >> 20) << " MiB over its limit of "
             << (process->footprint_limit_bytes() >> 20) << " MiB";
  KillProcess(process.get(), InferenceOutcome::Kind::kMemoryExceeded,
              CrashLoopGuard::Phase::kGenerating);
}

void InferenceProcessHost::RemoveFromQueue(ModelProcess* process,
                                           uint64_t token) {
  std::deque<uint64_t>& order = process->queue_order;
  const bool was_running = !order.empty() && order.front() == token;
  order.erase(std::remove(order.begin(), order.end(), token), order.end());
  if (!was_running || order.empty()) return;
  // The service moves on to the next request: its clocks start now.
  auto next = process->generations.find(order.front());
  if (next != process->generations.end()) next->second->Activate(NowMs());
}

InferenceProcessHost::ModelProcess* InferenceProcessHost::FindGenerationOwner(
    uint64_t token) const {
  if (current_ && current_->generations.count(token)) return current_.get();
  for (const auto& p : retired_) {
    if (p->generations.count(token)) return p.get();
  }
  return nullptr;
}

void InferenceProcessHost::UpdateTicking() {
  bool any = current_ && !current_->generations.empty();
  for (const auto& p : retired_) any = any || !p->generations.empty();
  if (any && !tick_timer_.IsRunning()) {
    tick_timer_.Start(FROM_HERE, timeouts_.tick,
                      base::BindRepeating(&InferenceProcessHost::OnTick,
                                          base::Unretained(this)));
  } else if (!any) {
    tick_timer_.Stop();
  }
}

void InferenceProcessHost::OnTick() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const int64_t now = NowMs();
  std::vector<base::WeakPtr<ModelProcess>> stuck;
  auto check = [&](ModelProcess* p) {
    for (const auto& [token, generation] : p->generations) {
      const StreamViolation v = generation->CheckTime(now);
      if (v != StreamViolation::kNone) {
        LOG(ERROR) << "[MoltAI] inference process: "
                   << StreamViolationName(v);
        stuck.push_back(p->GetWeakPtr());
        return;
      }
    }
  };
  if (current_) check(current_.get());
  for (const auto& p : retired_) check(p.get());
  for (const auto& p : stuck) {
    if (p) {
      KillProcess(p.get(), InferenceOutcome::Kind::kTimedOut,
                  CrashLoopGuard::Phase::kGenerating);
    }
  }
}

}  // namespace molt_ai
