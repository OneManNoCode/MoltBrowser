// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser side of on-device inference (S5 phase 2): launches the
// sandboxed molt_ai.mojom.MoltInferenceService utility process, loads a model
// into it and streams generations back, treating the process as hostile
// (StreamValidator, deadlines, liveness, a footprint watchdog). One process
// per model; dropping the remotes ends the process. Design:
// docs/design/SANDBOXED_INFERENCE.md.

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_INFERENCE_PROCESS_HOST_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_INFERENCE_PROCESS_HOST_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/runtime/stream_validator.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "content/public/browser/service_process_host.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace molt_ai {

// Added to the command line of an inference process launched for the GPU
// (design section 11.4). On Linux the process's pre-sandbox hook then also
// lets it reach the DRM render nodes and what Mesa's Vulkan drivers read;
// without it the sandbox is exactly upstream's. The same string is in
// content/utility/on_device_model/molt_gpu_inference_sandbox_linux.h
// (content can't include chrome/).
inline constexpr char kMoltInferenceGpuSwitch[] = "molt-inference-gpu";

// How a load or a generation ended, for the caller blocked on it. Never
// carries prompt or response text.
struct InferenceOutcome {
  enum class Kind {
    kOk,
    // Generation: the caller cancelled it.
    kCancelled,
    // The service reported a load failure (see load_status).
    kLoadFailed,
    // The process crashed or exited.
    kProcessGone,
    // The sandboxed process couldn't be started (on Windows, e.g., the
    // portable ZIP lacks the install-directory ACLs LPAC needs).
    kLaunchFailed,
    // It broke the protocol (bad message); it was killed.
    kViolation,
    // Load deadline, generation deadline or no sign of life; it was killed.
    kTimedOut,
    // It used far more memory than the model needs; it was killed.
    kMemoryExceeded,
    // No model is loaded (or it was unloaded meanwhile).
    kNoModel,
    // MoltBrowser is closing.
    kShuttingDown,
  };

  InferenceOutcome();
  explicit InferenceOutcome(Kind kind);
  ~InferenceOutcome();
  InferenceOutcome(const InferenceOutcome&);
  InferenceOutcome& operator=(const InferenceOutcome&);

  Kind kind = Kind::kProcessGone;
  // Load.
  mojom::LoadModelStatus load_status = mojom::LoadModelStatus::kOk;
  std::string architecture;
  std::string backend;
  uint32_t context_tokens = 0;
  uint32_t vocab_size = 0;
  bool cpu_repack_used = false;
  // A vision projector is loaded (image input). Only ever true when one was
  // sent.
  bool vision = false;
  // The GPU (Vulkan, design section 11): what became of it, and the layers
  // on it (backend "vulkan" only).
  mojom::GpuStatus gpu_status = mojom::GpuStatus::kNotRequested;
  uint32_t gpu_layers = 0;
  // Generation: set when OnComplete arrived (counts clamped).
  bool has_summary = false;
  StreamSummary summary;
  // A stop sequence matched in the browser's copy of the filter.
  bool stopped_by_browser = false;
};

// Hands text and the outcome from the UI-thread host to a worker blocked in
// BrowserAIRuntime. Thread-safe. Text is validated before it gets here and is
// bounded by StreamValidator's total cap.
class InferenceMailbox : public base::RefCountedThreadSafe<InferenceMailbox> {
 public:
  using Clock = std::chrono::steady_clock;

  InferenceMailbox();
  InferenceMailbox(const InferenceMailbox&) = delete;
  InferenceMailbox& operator=(const InferenceMailbox&) = delete;

  // Producer side (UI thread).
  void PushText(std::string_view text);
  // The UI thread handled something for this request (liveness of the UI
  // thread itself, see BrowserAIRuntime).
  void Heartbeat();
  // The first outcome wins; later ones are ignored.
  void Finish(InferenceOutcome outcome);

  // Consumer side (the blocked worker). Waits up to |timeout| for text, the
  // outcome or Wake(); moves pending text into |*text|. Returns true once
  // the outcome is set and all text has been taken.
  bool Wait(std::chrono::milliseconds timeout, std::string* text);
  // Wakes Wait() early (cancellation, shutdown).
  void Wake();
  InferenceOutcome outcome() const;
  Clock::time_point last_heartbeat() const;

 private:
  friend class base::RefCountedThreadSafe<InferenceMailbox>;
  ~InferenceMailbox();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string text_;
  bool finished_ = false;
  bool woken_ = false;
  InferenceOutcome outcome_;
  Clock::time_point heartbeat_;
};

// Lives on the UI thread (ServiceProcessHost::Launch and the remotes).
// BrowserAIRuntime posts to it and waits on mailboxes.
class InferenceProcessHost {
 public:
  struct LoadRequest {
    LoadRequest();
    ~LoadRequest();
    LoadRequest(LoadRequest&&);
    LoadRequest& operator=(LoadRequest&&);

    std::string model_id;
    // Model id + file size + mtime, for the crash-loop guard.
    std::string crash_key;
    // Opened read-only (not async) by the caller.
    base::File file;
    uint64_t file_size = 0;
    // The model's vision projector (image input), opened like |file|;
    // invalid for a text-only model.
    base::File vision_projector;
    uint64_t vision_projector_size = 0;
    uint64_t total_ram_bytes = 0;
    uint32_t cpu_threads = 1;
    uint32_t max_context_tokens = 0;
    bool allow_cpu_repack = true;
    // Private footprint above which the process is killed; 0 = no check.
    uint64_t footprint_limit_bytes = 0;
    // Ask for the GPU (llama.cpp's Vulkan backend; design section 11). The
    // process is then launched with kMoltInferenceGpuSwitch, which on Linux
    // lets its sandbox reach the DRM render nodes Mesa's drivers use.
    bool use_gpu = false;
  };

  struct Timeouts {
    base::TimeDelta load = base::Minutes(10);
    base::TimeDelta generation = base::Minutes(15);
    base::TimeDelta stall = base::Seconds(120);
    base::TimeDelta exit_grace = base::Seconds(5);
    base::TimeDelta footprint_period = base::Seconds(30);
    base::TimeDelta tick = base::Seconds(1);
  };

  enum class UnloadMode {
    // Stop taking requests; let running ones finish, then end the process.
    kGraceful,
    // End it now; running requests fail.
    kNow,
  };

  // Process events for BrowserAIRuntime, on the UI thread.
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // The model process ended unexpectedly or was killed for misbehaving
    // (not an unload the runtime asked for). |gpu_failure|: it was loading
    // with the GPU requested, or running on the GPU, and crashed, hung or
    // ran over its memory (GpuFallbackGuard counts it, not CrashLoopGuard).
    virtual void OnModelProcessLost(const std::string& model_id,
                                    const std::string& crash_key,
                                    CrashLoopGuard::Phase phase,
                                    bool gpu_failure) = 0;
  };

  // Tests replace the launch of a real utility process.
  using Launcher = base::RepeatingCallback<void(
      mojo::PendingReceiver<mojom::MoltInferenceService> receiver,
      base::WeakPtr<content::ServiceProcessHost::Observer> observer,
      base::OnceCallback<void(const base::Process&)> on_process,
      bool use_gpu)>;

  explicit InferenceProcessHost(Delegate* delegate);
  ~InferenceProcessHost();
  InferenceProcessHost(const InferenceProcessHost&) = delete;
  InferenceProcessHost& operator=(const InferenceProcessHost&) = delete;

  // Launches a process and loads |request| into it. On success it replaces
  // the current model (which finishes its running requests first).
  void Load(LoadRequest request, scoped_refptr<InferenceMailbox> mailbox);

  // Generations on the current model. |token| identifies the request for
  // Cancel(). |limits.context_tokens| is filled in here.
  void StartChat(uint64_t token,
                 mojom::ChatRequestPtr request,
                 mojom::GenerationParamsPtr params,
                 StreamLimits limits,
                 scoped_refptr<InferenceMailbox> mailbox);
  void StartPrompt(uint64_t token,
                   mojom::RawPromptRequestPtr request,
                   mojom::GenerationParamsPtr params,
                   StreamLimits limits,
                   scoped_refptr<InferenceMailbox> mailbox);
  // Closes the request's streamer (the service cancels it) and finishes its
  // mailbox as cancelled.
  void Cancel(uint64_t token);

  // Unloads the current model. |exited| (may be null) is finished once the
  // process has ended.
  void Unload(UnloadMode mode, scoped_refptr<InferenceMailbox> exited);

  // Answers a blocked worker's heartbeat.
  void Ping(scoped_refptr<InferenceMailbox> mailbox);

  // The app is terminating: every request fails and every process ends.
  void Shutdown();

  void SetLauncherForTesting(Launcher launcher);
  void SetTimeoutsForTesting(const Timeouts& timeouts);
  bool HasCurrentModelForTesting() const;
  size_t ProcessCountForTesting() const;

 private:
  class ModelProcess;
  class Generation;

  void StartGeneration(uint64_t token,
                       bool is_chat,
                       mojom::ChatRequestPtr chat,
                       mojom::RawPromptRequestPtr raw,
                       mojom::GenerationParamsPtr params,
                       StreamLimits limits,
                       scoped_refptr<InferenceMailbox> mailbox);
  void OnLoadReply(base::WeakPtr<ModelProcess> process,
                   mojom::LoadModelStatus status,
                   mojom::ModelDescriptionPtr description);
  void OnLoadTimeout(base::WeakPtr<ModelProcess> process);
  void OnProcessLaunched(base::WeakPtr<ModelProcess> process,
                         const base::Process& handle);
  void OnProcessDisconnected(base::WeakPtr<ModelProcess> process);
  void OnProcessExited(ModelProcess* process, bool crashed_before_ipc);
  // A process that ended or closed its pipes on its own.
  void OnProcessLost(ModelProcess* process);
  void OnFootprintTimer(base::WeakPtr<ModelProcess> process);
  void OnFootprintSample(base::WeakPtr<ModelProcess> process,
                         std::optional<uint64_t> footprint);
  void OnExitTimeout(base::WeakPtr<ModelProcess> process, bool terminated);
  void OnTick();

  // A generation ended (the Generation is destroyed).
  void FinishGeneration(ModelProcess* process,
                        uint64_t token,
                        InferenceOutcome outcome);
  // Stop sending requests to |process|; it ends once they are done.
  void Retire(ModelProcess* process);
  // Drops its remotes so it exits, then waits for that (a grace period, then
  // a kill).
  void EndProcess(ModelProcess* process, bool terminate_now);
  // Fails everything on |process|, reports it to the delegate and ends it.
  void KillProcess(ModelProcess* process,
                   InferenceOutcome::Kind kind,
                   CrashLoopGuard::Phase phase);
  CrashLoopGuard::Phase PhaseOf(const ModelProcess* process) const;
  void DestroySoon(ModelProcess* process);
  void DestroyNow(ModelProcess* process);
  ModelProcess* FindGenerationOwner(uint64_t token) const;
  // Drops |token| from |process|'s queue; starts the next one's clocks if
  // |token| was running.
  void RemoveFromQueue(ModelProcess* process, uint64_t token);
  void UpdateTicking();
  int64_t NowMs() const;

  SEQUENCE_CHECKER(sequence_checker_);

  raw_ptr<Delegate> delegate_;
  Launcher launcher_;
  Timeouts timeouts_;
  bool shutting_down_ = false;

  // Loaded and taking requests.
  std::unique_ptr<ModelProcess> current_;
  // Being launched/loaded (the current one keeps serving meanwhile).
  std::unique_ptr<ModelProcess> loading_;
  // Finishing their requests, or ending.
  std::vector<std::unique_ptr<ModelProcess>> retired_;

  base::RepeatingTimer tick_timer_;

  base::WeakPtrFactory<InferenceProcessHost> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_INFERENCE_PROCESS_HOST_H_
