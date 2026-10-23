// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/embedding_process_host.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/process/process_metrics.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/browser/molt_ai/runtime/embedding_validator.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "content/public/browser/service_process_info.h"

#if BUILDFLAG(IS_MAC)
#include "content/public/browser/browser_child_process_host.h"
#endif

namespace molt_ai {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

// Kills right away: the process misbehaved or ignored the request to exit.
constexpr int kKilledExitCode = base::Process::kResultCodeKilledBadMessage;

// Private memory of |process| (as InferenceProcessHost samples it). Blocks.
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
    base::OnceCallback<void(const base::Process&)> on_process) {
  // The sandbox (kOnDeviceModelExecution) comes from the interface's
  // [ServiceSandbox] attribute.
  content::ServiceProcessHost::Launch(
      std::move(receiver),
      content::ServiceProcessHost::Options()
          .WithDisplayName(std::string("On-Device AI Search Model"))
          .WithObserver(std::move(observer))
          .WithProcessCallback(std::move(on_process))
          .Pass());
}

// The default Opener (MayBlock worker): the browser opens the file (the
// sandbox can't) and checks it is the pinned one before any process starts.
// Not async: llama.cpp reads it synchronously. Share delete, so removing
// the model isn't blocked by a process still exiting.
EmbeddingProcessHost::OpenedModel OpenModelFile(
    const EmbeddingModelConfig& config) {
  EmbeddingProcessHost::OpenedModel out;
  if (config.path.value().empty() || config.file_size == 0) return out;
  base::File file(config.path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                                   base::File::FLAG_WIN_SHARE_DELETE);
  base::File::Info info;
  if (!file.IsValid() || !file.GetInfo(&info) || info.is_directory ||
      info.size < 0 || static_cast<uint64_t>(info.size) != config.file_size) {
    return out;
  }
  out.crash_key =
      config.model_id + ":" + base::NumberToString(config.file_size) + ":" +
      base::NumberToString(static_cast<int64_t>(
          info.last_modified.ToDeltaSinceWindowsEpoch().InMicroseconds()));
  out.enough_memory =
      static_cast<uint64_t>(std::max<int64_t>(
          0, base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes())) >=
      config.ram_required_bytes;
  out.file = std::move(file);
  return out;
}

// Every caller gets its answer from a fresh task: never re-entrantly, so a
// callback that queues more work always finds the host in a settled state.
void ReplySoon(EmbeddingProcessHost::EmbedCallback done,
               EmbeddingStatus status,
               std::vector<std::vector<float>> vectors = {}) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), status, std::move(vectors)));
}

// Closing a file blocks, which the UI thread must not do.
void CloseFileSoon(base::File file) {
  if (!file.IsValid()) return;
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce([](base::File) {}, std::move(file)));
}

EmbeddingStatus StatusForLoadFailure(mojom::LoadModelStatus status) {
  switch (status) {
    case mojom::LoadModelStatus::kOutOfMemory:
      return EmbeddingStatus::kNotEnoughMemory;
    case mojom::LoadModelStatus::kOk:
    case mojom::LoadModelStatus::kUnsupportedCpu:
    case mojom::LoadModelStatus::kInvalidModelFile:
    case mojom::LoadModelStatus::kInvalidVisionProjector:
    case mojom::LoadModelStatus::kBackendUnavailable:
      return EmbeddingStatus::kUnavailable;
  }
  return EmbeddingStatus::kUnavailable;
}

}  // namespace

const char* EmbeddingStatusName(EmbeddingStatus status) {
  switch (status) {
    case EmbeddingStatus::kOk:
      return "ok";
    case EmbeddingStatus::kUnavailable:
      return "unavailable";
    case EmbeddingStatus::kNotEnoughMemory:
      return "not enough memory";
    case EmbeddingStatus::kBackingOff:
      return "backing off after crashes";
    case EmbeddingStatus::kFailed:
      return "failed";
    case EmbeddingStatus::kShuttingDown:
      return "shutting down";
    case EmbeddingStatus::kNotLoaded:
      return "not loaded";
  }
  return "unknown";
}

// ---- EmbeddingModelConfig ----------------------------------------------------

EmbeddingModelConfig::EmbeddingModelConfig() = default;
EmbeddingModelConfig::~EmbeddingModelConfig() = default;
EmbeddingModelConfig::EmbeddingModelConfig(const EmbeddingModelConfig&) =
    default;
EmbeddingModelConfig& EmbeddingModelConfig::operator=(
    const EmbeddingModelConfig&) = default;

bool EmbeddingModelConfig::SameModelAs(
    const EmbeddingModelConfig& other) const {
  return model_id == other.model_id && path == other.path &&
         file_size == other.file_size && pooling == other.pooling &&
         max_tokens_per_text == other.max_tokens_per_text &&
         terminator == other.terminator && dimensions == other.dimensions &&
         native_dimensions == other.native_dimensions &&
         architecture == other.architecture;
}

EmbeddingProcessHost::OpenedModel::OpenedModel() = default;
EmbeddingProcessHost::OpenedModel::~OpenedModel() = default;
EmbeddingProcessHost::OpenedModel::OpenedModel(OpenedModel&&) = default;
EmbeddingProcessHost::OpenedModel&
EmbeddingProcessHost::OpenedModel::operator=(OpenedModel&&) = default;

// ---- Internals -----------------------------------------------------------------

struct EmbeddingProcessHost::Request {
  // The non-empty texts, as sent.
  std::vector<std::string> texts;
  // One per text of the caller: its index in |texts|, or -1 for an empty
  // text (an all-zero vector, never sent).
  std::vector<int> slots;
  size_t dimensions = 0;
  EmbedCallback done;
};

// Lifecycle notifications of one process (per-instance observer).
class EmbeddingProcessHost::ProcessObserver
    : public content::ServiceProcessHost::Observer {
 public:
  ProcessObserver(EmbeddingProcessHost* host, uint64_t attempt)
      : host_(host), attempt_(attempt) {}
  ~ProcessObserver() override = default;

  // content::ServiceProcessHost::Observer (UI thread):
  void OnServiceProcessTerminatedNormally(
      const content::ServiceProcessInfo& info) override {
    host_->OnProcessExited(attempt_);
  }
  void OnServiceProcessCrashed(
      const content::ServiceProcessInfo& info) override {
    host_->OnProcessExited(attempt_);
  }

  base::WeakPtr<ProcessObserver> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  uint64_t attempt() const { return attempt_; }

  base::Process process;
  base::OneShotTimer exit_timer;
  bool terminated = false;

 private:
  raw_ptr<EmbeddingProcessHost> host_;
  const uint64_t attempt_;
  base::WeakPtrFactory<ProcessObserver> weak_factory_{this};
};

class EmbeddingProcessHost::PressureListener
    : public base::MemoryPressureListener {
 public:
  explicit PressureListener(EmbeddingProcessHost* host) : host_(host) {}

 private:
  void OnMemoryPressure(base::MemoryPressureLevel level) override {
    if (level == base::MEMORY_PRESSURE_LEVEL_CRITICAL) {
      host_->OnCriticalMemoryPressure();
    }
  }

  raw_ptr<EmbeddingProcessHost> host_;
};

// ---- EmbeddingProcessHost ------------------------------------------------------

// static
EmbeddingProcessHost* EmbeddingProcessHost::GetInstance() {
  static base::NoDestructor<EmbeddingProcessHost> instance;
  static base::NoDestructor<base::CallbackListSubscription> subscription(
      browser_shutdown::AddAppTerminatingCallback(base::BindOnce(
          [](EmbeddingProcessHost* host) { host->Shutdown(); },
          base::Unretained(instance.get()))));
  return instance.get();
}

EmbeddingProcessHost::EmbeddingProcessHost()
    : opener_(base::BindRepeating(&OpenModelFile)),
      launcher_(base::BindRepeating(&LaunchUtilityProcess)) {}

EmbeddingProcessHost::~EmbeddingProcessHost() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Shutdown();
}

int64_t EmbeddingProcessHost::NowMs() const {
  return (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds();
}

void EmbeddingProcessHost::SetOpenerForTesting(Opener opener) {
  opener_ = std::move(opener);
}

void EmbeddingProcessHost::SetLauncherForTesting(Launcher launcher) {
  launcher_ = std::move(launcher);
}

void EmbeddingProcessHost::SetTimeoutsForTesting(const Timeouts& timeouts) {
  timeouts_ = timeouts;
}

bool EmbeddingProcessHost::HasProcessForTesting() const {
  return state_ != State::kNone;
}

void EmbeddingProcessHost::Embed(const EmbeddingModelConfig& config,
                                 std::vector<std::string> texts,
                                 EmbedCallback done,
                                 bool urgent) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) {
    ReplySoon(std::move(done), EmbeddingStatus::kShuttingDown);
    return;
  }
  if (texts.size() > mojom::kMaxEmbedTexts || config.dimensions == 0 ||
      config.dimensions > mojom::kMaxEmbeddingDimensions) {
    LOG(ERROR) << "[MoltAI] embedding request over the caps";
    ReplySoon(std::move(done), EmbeddingStatus::kFailed);
    return;
  }

  auto request = std::make_unique<Request>();
  request->dimensions = config.dimensions;
  for (const std::string& text : texts) {
    // The service treats invalid UTF-8 as a bad message: send valid UTF-8
    // (NUL and invalid bytes become U+FFFD), cut on a character boundary.
    std::string clean = ToDisplayText(text);
    if (clean.size() > mojom::kMaxEmbedTextBytes) {
      clean.resize(Utf8SafePrefixLength(
          std::string_view(clean).substr(0, mojom::kMaxEmbedTextBytes)));
    }
    if (clean.empty()) {
      request->slots.push_back(-1);
      continue;
    }
    request->slots.push_back(static_cast<int>(request->texts.size()));
    request->texts.push_back(std::move(clean));
  }
  if (request->texts.empty()) {
    ReplySoon(std::move(done), EmbeddingStatus::kOk,
              std::vector<std::vector<float>>(
                  texts.size(), std::vector<float>(config.dimensions, 0.0f)));
    return;
  }
  request->done = std::move(done);

  if (config_ && !config_->SameModelAs(config)) {
    // Another model: the old process can't serve it.
    FailAll(EmbeddingStatus::kFailed);
    DropProcess(/*kill=*/false);
  }
  config_ = config;
  if (urgent) {
    queue_.push_front(std::move(request));
  } else {
    queue_.push_back(std::move(request));
  }
  idle_timer_.Stop();
  if (state_ == State::kNone) {
    StartLoad();
  } else if (state_ == State::kReady && !in_flight_) {
    SendNext();
  }
}

void EmbeddingProcessHost::EmbedIfLoaded(const EmbeddingModelConfig& config,
                                         std::vector<std::string> texts,
                                         EmbedCallback done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Only a process that is ready with this very model: Embed() would
  // otherwise start, load or replace one.
  if (shutting_down_ || state_ != State::kReady || !config_ ||
      !config_->SameModelAs(config)) {
    ReplySoon(std::move(done), EmbeddingStatus::kNotLoaded);
    return;
  }
  Embed(config, std::move(texts), std::move(done), /*urgent=*/true);
}

void EmbeddingProcessHost::StartLoad() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!pressure_registration_) {
    // Same tag as BrowserAIRuntime's listener (see the note there): the
    // enum is closed and critical-level notifications are never suppressed.
    pressure_listener_ = std::make_unique<PressureListener>(this);
    pressure_registration_ =
        std::make_unique<base::MemoryPressureListenerRegistration>(
            base::MemoryPressureListenerTag::kGlicKeyedService,
            pressure_listener_.get());
  }
  state_ = State::kOpening;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(opener_, *config_),
      base::BindOnce(&EmbeddingProcessHost::OnOpened,
                     weak_factory_.GetWeakPtr(), attempt_));
}

void EmbeddingProcessHost::OnOpened(uint64_t attempt, OpenedModel opened) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || state_ != State::kOpening || !config_) {
    CloseFileSoon(std::move(opened.file));
    return;
  }
  if (!opened.file.IsValid()) {
    LOG(WARNING) << "[MoltAI] embedding model file missing or not the "
                    "pinned one: "
                 << config_->model_id;
    state_ = State::kNone;
    FailAll(EmbeddingStatus::kUnavailable);
    return;
  }
  const CrashLoopGuard::Verdict verdict =
      crash_guard_.Check(opened.crash_key, NowMs());
  if (verdict != CrashLoopGuard::Verdict::kAllowed ||
      !opened.enough_memory) {
    state_ = State::kNone;
    CloseFileSoon(std::move(opened.file));
    FailAll(verdict != CrashLoopGuard::Verdict::kAllowed
                ? EmbeddingStatus::kBackingOff
                : EmbeddingStatus::kNotEnoughMemory);
    return;
  }
  crash_key_ = opened.crash_key;
  state_ = State::kLoading;

  auto observer = std::make_unique<ProcessObserver>(this, attempt_);
  launcher_.Run(service_.BindNewPipeAndPassReceiver(), observer->GetWeakPtr(),
                base::BindOnce(&EmbeddingProcessHost::OnProcessLaunched,
                               weak_factory_.GetWeakPtr(), attempt_));
  processes_.push_back(std::move(observer));
  service_.set_disconnect_handler(
      base::BindOnce(&EmbeddingProcessHost::OnDisconnected,
                     weak_factory_.GetWeakPtr(), attempt_));

  auto params = mojom::EmbeddingModelLoadParams::New();
  params->weights = std::move(opened.file);
  params->file_size = config_->file_size;
  params->cpu_threads = static_cast<uint32_t>(
      std::clamp(base::SysInfo::NumberOfProcessors() / 4, 1, 4));
  params->pooling = config_->pooling;
  params->max_tokens_per_text = config_->max_tokens_per_text;
  params->terminator = config_->terminator;
  params->dimensions = config_->dimensions;
  service_->LoadEmbeddingModel(
      std::move(params), model_.BindNewPipeAndPassReceiver(),
      base::BindOnce(&EmbeddingProcessHost::OnLoadReply,
                     weak_factory_.GetWeakPtr(), attempt_));
  model_.set_disconnect_handler(
      base::BindOnce(&EmbeddingProcessHost::OnDisconnected,
                     weak_factory_.GetWeakPtr(), attempt_));
  load_deadline_.Start(FROM_HERE, timeouts_.load,
                       base::BindOnce(&EmbeddingProcessHost::OnLoadTimeout,
                                      weak_factory_.GetWeakPtr(), attempt_));
}

void EmbeddingProcessHost::OnProcessLaunched(uint64_t attempt,
                                             const base::Process& process) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ProcessObserver* p = FindProcess(attempt);
  if (!p) return;
  p->process = process.Duplicate();
  if (attempt == attempt_ && config_) {
    footprint_timer_.Start(
        FROM_HERE, timeouts_.footprint_period,
        base::BindRepeating(&EmbeddingProcessHost::OnFootprintTimer,
                            base::Unretained(this)));
  }
}

void EmbeddingProcessHost::OnLoadReply(
    uint64_t attempt,
    mojom::LoadModelStatus status,
    mojom::EmbeddingModelDescriptionPtr description) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || state_ != State::kLoading || !config_) return;
  load_deadline_.Stop();
  if (status != mojom::LoadModelStatus::kOk) {
    LOG(WARNING) << "[MoltAI] embedding model didn't load (status "
                 << static_cast<int>(status) << ")";
    FailAll(StatusForLoadFailure(status));
    DropProcess(/*kill=*/false);
    return;
  }
  EmbeddingExpectations expected;
  expected.dimensions = config_->dimensions;
  expected.max_tokens_per_text = config_->max_tokens_per_text;
  expected.native_dimensions = config_->native_dimensions;
  expected.architecture = config_->architecture;
  if (!description ||
      !IsValidEmbeddingDescription(
          description->architecture, description->backend,
          description->native_dimensions, description->dimensions,
          description->max_tokens_per_text, description->vocab_size,
          expected)) {
    // Not the model the registry pins (or a lying process).
    mojo::ReportBadMessage("invalid EmbeddingModelDescription");
    Lost(CrashLoopGuard::Phase::kLoading, /*kill=*/true);
    return;
  }
  state_ = State::kReady;
  VLOG(1) << "[MoltAI] embedding model loaded (" << description->architecture
          << ", " << description->backend << ")";
  SendNext();
}

void EmbeddingProcessHost::OnLoadTimeout(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || state_ != State::kLoading) return;
  LOG(ERROR) << "[MoltAI] embedding model: load deadline exceeded";
  Lost(CrashLoopGuard::Phase::kLoading, /*kill=*/true);
}

void EmbeddingProcessHost::SendNext() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || in_flight_) return;
  if (queue_.empty()) {
    ArmIdle();
    return;
  }
  in_flight_ = std::move(queue_.front());
  queue_.pop_front();
  model_->Embed(in_flight_->texts,
                base::BindOnce(&EmbeddingProcessHost::OnEmbedReply,
                               weak_factory_.GetWeakPtr(), attempt_));
  call_deadline_.Start(FROM_HERE, timeouts_.call,
                       base::BindOnce(&EmbeddingProcessHost::OnCallTimeout,
                                      weak_factory_.GetWeakPtr(), attempt_));
}

void EmbeddingProcessHost::OnEmbedReply(
    uint64_t attempt,
    const std::optional<std::vector<float>>& vectors) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || !in_flight_) return;
  call_deadline_.Stop();
  std::unique_ptr<Request> request = std::move(in_flight_);
  if (!vectors) {
    // The model reported a failure (not a protocol violation).
    ReplySoon(std::move(request->done), EmbeddingStatus::kFailed);
    SendNext();
    return;
  }
  const EmbeddingReplyError error = CheckEmbeddingReply(
      *vectors, request->texts.size(), request->dimensions);
  if (error != EmbeddingReplyError::kNone) {
    LOG(ERROR) << "[MoltAI] embedding model: "
               << EmbeddingReplyErrorName(error);
    // Kills the sender through content's bad-message handling; Lost() also
    // ends it and fails everything queued.
    mojo::ReportBadMessage(EmbeddingReplyErrorName(error));
    ReplySoon(std::move(request->done), EmbeddingStatus::kFailed);
    Lost(CrashLoopGuard::Phase::kGenerating, /*kill=*/true);
    return;
  }
  std::vector<std::vector<float>> rows =
      SplitEmbeddingReply(*vectors, request->dimensions);
  std::vector<std::vector<float>> out;
  out.reserve(request->slots.size());
  for (int slot : request->slots) {
    out.push_back(slot < 0 ? std::vector<float>(request->dimensions, 0.0f)
                           : std::move(rows[static_cast<size_t>(slot)]));
  }
  ReplySoon(std::move(request->done), EmbeddingStatus::kOk, std::move(out));
  SendNext();
}

void EmbeddingProcessHost::OnCallTimeout(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || !in_flight_) return;
  LOG(ERROR) << "[MoltAI] embedding model: call deadline exceeded";
  Lost(CrashLoopGuard::Phase::kGenerating, /*kill=*/true);
}

void EmbeddingProcessHost::OnDisconnected(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || state_ == State::kNone) return;
  ProcessObserver* p = FindProcess(attempt);
  if (state_ == State::kLoading && (!p || !p->process.IsValid())) {
    // It never started: not the model's fault (on Windows, e.g., the
    // portable ZIP lacks the ACLs the sandbox needs).
    LOG(WARNING) << "[MoltAI] the embedding model's sandboxed process "
                    "didn't start";
    FailAll(EmbeddingStatus::kUnavailable);
    DropProcess(/*kill=*/false);
    return;
  }
  Lost(CurrentPhase(), /*kill=*/false);
}

void EmbeddingProcessHost::OnProcessExited(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt == attempt_ && state_ != State::kNone) {
    // Exited without being asked to (a crash, the OOM killer).
    Lost(CurrentPhase(), /*kill=*/false);
  }
  // The observer is notified from content code: forget it afterwards.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&EmbeddingProcessHost::RemoveProcess,
                                weak_factory_.GetWeakPtr(), attempt));
}

void EmbeddingProcessHost::OnExitTimeout(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ProcessObserver* p = FindProcess(attempt);
  if (!p) return;
  if (!p->terminated && p->process.IsValid()) {
    p->process.Terminate(kKilledExitCode, /*wait=*/false);
    p->terminated = true;
    p->exit_timer.Start(FROM_HERE, timeouts_.exit_grace,
                        base::BindOnce(&EmbeddingProcessHost::OnExitTimeout,
                                       weak_factory_.GetWeakPtr(), attempt));
    return;
  }
  // No exit notification (e.g. it never launched): stop waiting.
  RemoveProcess(attempt);
}

void EmbeddingProcessHost::OnFootprintTimer() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ProcessObserver* p = FindProcess(attempt_);
  if (!p || !p->process.IsValid() || state_ == State::kNone) return;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&SamplePrivateFootprint, p->process.Duplicate()),
      base::BindOnce(&EmbeddingProcessHost::OnFootprintSample,
                     weak_factory_.GetWeakPtr(), attempt_));
}

void EmbeddingProcessHost::OnFootprintSample(
    uint64_t attempt,
    std::optional<uint64_t> footprint) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt != attempt_ || state_ == State::kNone || !config_ ||
      !footprint) {
    return;
  }
  // Weights (repacked copies count on CPU), a one-text KV cache and compute
  // buffers stay far below this.
  const uint64_t limit = 2 * (config_->file_size + kGiB);
  if (*footprint <= limit) return;
  LOG(ERROR) << "[MoltAI] embedding model: private footprint "
             << (*footprint >> 20) << " MiB over its limit of "
             << (limit >> 20) << " MiB";
  Lost(CrashLoopGuard::Phase::kGenerating, /*kill=*/true);
}

void EmbeddingProcessHost::ArmIdle() {
  idle_timer_.Start(FROM_HERE, timeouts_.idle,
                    base::BindOnce(&EmbeddingProcessHost::OnIdle,
                                   weak_factory_.GetWeakPtr()));
}

void EmbeddingProcessHost::OnIdle() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (in_flight_ || !queue_.empty() || state_ != State::kReady) return;
  VLOG(1) << "[MoltAI] embedding model idle: ending its process";
  DropProcess(/*kill=*/false);
}

void EmbeddingProcessHost::OnCriticalMemoryPressure() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // As for the chat model: never interrupt a call.
  if (in_flight_ || !queue_.empty() || state_ != State::kReady) return;
  LOG(INFO) << "[MoltAI] critical memory pressure: ending the embedding "
               "model's process";
  DropProcess(/*kill=*/false);
}

CrashLoopGuard::Phase EmbeddingProcessHost::CurrentPhase() const {
  if (state_ == State::kLoading) return CrashLoopGuard::Phase::kLoading;
  return in_flight_ ? CrashLoopGuard::Phase::kGenerating
                    : CrashLoopGuard::Phase::kIdle;
}

void EmbeddingProcessHost::Lost(CrashLoopGuard::Phase phase, bool kill) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!crash_key_.empty()) crash_guard_.RecordCrash(crash_key_, phase, NowMs());
  LOG(WARNING) << "[MoltAI] the embedding model's process "
               << (kill ? "misbehaved and was stopped" : "ended unexpectedly");
  FailAll(EmbeddingStatus::kFailed);
  DropProcess(kill);
}

void EmbeddingProcessHost::FailAll(EmbeddingStatus status) {
  if (in_flight_) {
    ReplySoon(std::move(in_flight_->done), status);
    in_flight_.reset();
  }
  call_deadline_.Stop();
  while (!queue_.empty()) {
    ReplySoon(std::move(queue_.front()->done), status);
    queue_.pop_front();
  }
}

void EmbeddingProcessHost::DropProcess(bool kill) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t old = attempt_;
  ++attempt_;  // replies and timers of the old process are ignored now
  state_ = State::kNone;
  crash_key_.clear();
  load_deadline_.Stop();
  call_deadline_.Stop();
  idle_timer_.Stop();
  footprint_timer_.Stop();
  // Closing both pipes ends the utility process.
  model_.reset();
  service_.reset();
  ProcessObserver* p = FindProcess(old);
  if (!p) {
    if (processes_.empty()) {
      std::vector<base::OnceClosure> waiters = std::move(exit_waiters_);
      exit_waiters_.clear();
      for (base::OnceClosure& w : waiters) std::move(w).Run();
    }
    return;
  }
  if (kill && p->process.IsValid()) {
    p->process.Terminate(kKilledExitCode, /*wait=*/false);
    p->terminated = true;
  }
  // A process that doesn't exit within the grace period is killed; one that
  // still isn't reported gone after another is given up on.
  p->exit_timer.Start(FROM_HERE, timeouts_.exit_grace,
                      base::BindOnce(&EmbeddingProcessHost::OnExitTimeout,
                                     weak_factory_.GetWeakPtr(), old));
}

EmbeddingProcessHost::ProcessObserver* EmbeddingProcessHost::FindProcess(
    uint64_t attempt) {
  for (const auto& p : processes_) {
    if (p->attempt() == attempt) return p.get();
  }
  return nullptr;
}

void EmbeddingProcessHost::RemoveProcess(uint64_t attempt) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (attempt == attempt_ && state_ != State::kNone) return;  // still in use
  std::erase_if(processes_, [&](const std::unique_ptr<ProcessObserver>& p) {
    return p->attempt() == attempt;
  });
  if (!processes_.empty()) return;
  std::vector<base::OnceClosure> waiters = std::move(exit_waiters_);
  exit_waiters_.clear();
  for (base::OnceClosure& w : waiters) std::move(w).Run();
}

void EmbeddingProcessHost::Unload(base::OnceClosure done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  FailAll(EmbeddingStatus::kUnavailable);
  config_.reset();
  exit_waiters_.push_back(std::move(done));
  if (state_ != State::kNone) {
    DropProcess(/*kill=*/false);
  } else if (processes_.empty()) {
    std::vector<base::OnceClosure> waiters = std::move(exit_waiters_);
    exit_waiters_.clear();
    for (base::OnceClosure& w : waiters) std::move(w).Run();
  }
}

void EmbeddingProcessHost::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shutting_down_) return;
  shutting_down_ = true;
  FailAll(EmbeddingStatus::kShuttingDown);
  if (state_ != State::kNone) DropProcess(/*kill=*/false);
  // The browser is going away: don't wait for exits.
  processes_.clear();
  std::vector<base::OnceClosure> waiters = std::move(exit_waiters_);
  exit_waiters_.clear();
  for (base::OnceClosure& w : waiters) std::move(w).Run();
  pressure_registration_.reset();
}

}  // namespace molt_ai
