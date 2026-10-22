// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The browser side of on-device text embeddings (semantic memory search,
// docs/design/SEMANTIC_SEARCH.md): launches a sandboxed
// molt_ai.mojom.MoltInferenceService process, loads the embedding model
// into it (MoltInferenceService.LoadEmbeddingModel) and runs Embed() calls,
// one at a time. Like InferenceProcessHost for chat models, it treats the
// process as hostile: every reply is validated (embedding_validator.h), each
// load and call has a deadline, a footprint watchdog bounds its memory, and
// a crash-loop guard (CrashLoopGuard) backs off a model that keeps dying.
// The process ends after a few idle minutes, on critical memory pressure,
// and when the app terminates.
//
// One process for the whole browser (the model is a global resource, like
// BrowserAIRuntime's); every profile's memory uses it. Texts never leave the
// process's memory (it has no disk or network access) and it keeps nothing
// between texts (its cache is cleared before each one).

#ifndef CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_PROCESS_HOST_H_
#define CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_PROCESS_HOST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/runtime/stream_validator.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "content/public/browser/service_process_host.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace molt_ai {

// The embedding model to run, from the caller's model registry.
struct EmbeddingModelConfig {
  EmbeddingModelConfig();
  ~EmbeddingModelConfig();
  EmbeddingModelConfig(const EmbeddingModelConfig&);
  EmbeddingModelConfig& operator=(const EmbeddingModelConfig&);

  // Same file, same settings: a process loaded for |other| can serve this.
  bool SameModelAs(const EmbeddingModelConfig& other) const;

  std::string model_id;
  base::FilePath path;
  // Exact size of the pinned file; any other size is refused.
  uint64_t file_size = 0;
  // Free physical memory needed to start it.
  uint64_t ram_required_bytes = 0;
  mojom::EmbeddingPooling pooling = mojom::EmbeddingPooling::kMean;
  uint32_t max_tokens_per_text = 512;
  std::string terminator;
  uint32_t dimensions = 0;
  // What the loaded model must describe itself as; 0 / "" = not checked.
  uint32_t native_dimensions = 0;
  std::string architecture;
};

enum class EmbeddingStatus {
  kOk,
  // The model can't run here: the file is missing or not the pinned one,
  // on-device AI can't run on this machine or platform, or the model
  // doesn't load. Retrying won't help until something changes.
  kUnavailable,
  // Not enough free memory to start the model right now.
  kNotEnoughMemory,
  // The model crashed or misbehaved recently; relaunching is backed off.
  kBackingOff,
  // This call failed (the process crashed, misbehaved or timed out, or the
  // model reported an error). Worth retrying later.
  kFailed,
  // MoltBrowser is closing.
  kShuttingDown,
};

const char* EmbeddingStatusName(EmbeddingStatus status);

// Lives on the UI thread.
class EmbeddingProcessHost {
 public:
  // On success, one vector of config.dimensions floats per text, in order:
  // L2-normalized, or all zeros for an empty text. Empty on failure.
  using EmbedCallback = base::OnceCallback<
      void(EmbeddingStatus, std::vector<std::vector<float>>)>;

  struct Timeouts {
    base::TimeDelta load = base::Minutes(3);
    base::TimeDelta call = base::Minutes(3);
    base::TimeDelta idle = base::Minutes(5);
    base::TimeDelta exit_grace = base::Seconds(5);
    base::TimeDelta footprint_period = base::Seconds(30);
  };

  // The model file opened read-only for the process, and the RAM check.
  // Made on a MayBlock worker.
  struct OpenedModel {
    OpenedModel();
    ~OpenedModel();
    OpenedModel(OpenedModel&&);
    OpenedModel& operator=(OpenedModel&&);

    // Invalid if missing, unreadable or of another size.
    base::File file;
    // Model id + size + mtime, for the crash-loop guard.
    std::string crash_key;
    bool enough_memory = false;
  };
  using Opener =
      base::RepeatingCallback<OpenedModel(const EmbeddingModelConfig&)>;
  using Launcher = base::RepeatingCallback<void(
      mojo::PendingReceiver<mojom::MoltInferenceService> receiver,
      base::WeakPtr<content::ServiceProcessHost::Observer> observer,
      base::OnceCallback<void(const base::Process&)> on_process)>;

  // The browser's instance (created on first use; never destroyed).
  static EmbeddingProcessHost* GetInstance();

  EmbeddingProcessHost();
  ~EmbeddingProcessHost();
  EmbeddingProcessHost(const EmbeddingProcessHost&) = delete;
  EmbeddingProcessHost& operator=(const EmbeddingProcessHost&) = delete;

  // Embeds |texts| (at most mojom::kMaxEmbedTexts; each is made valid UTF-8
  // and cut to mojom::kMaxEmbedTextBytes) with |config|'s model, starting
  // its process if needed. A request for another model ends the current
  // process first. An |urgent| request (a search) goes ahead of queued
  // background work. |done| always runs, on this thread, never
  // synchronously.
  void Embed(const EmbeddingModelConfig& config,
             std::vector<std::string> texts,
             EmbedCallback done,
             bool urgent = false);

  // Ends the process (queued and running calls fail with kUnavailable) and
  // runs |done| once it has exited, e.g. before the model file is deleted.
  void Unload(base::OnceClosure done);

  // The app is terminating: everything fails, the process ends.
  void Shutdown();

  void SetOpenerForTesting(Opener opener);
  void SetLauncherForTesting(Launcher launcher);
  void SetTimeoutsForTesting(const Timeouts& timeouts);
  bool HasProcessForTesting() const;

 private:
  enum class State { kNone, kOpening, kLoading, kReady };
  struct Request;
  class ProcessObserver;
  class PressureListener;

  void StartLoad();
  void OnOpened(uint64_t attempt, OpenedModel opened);
  void OnProcessLaunched(uint64_t attempt, const base::Process& process);
  void OnLoadReply(uint64_t attempt,
                   mojom::LoadModelStatus status,
                   mojom::EmbeddingModelDescriptionPtr description);
  void OnLoadTimeout(uint64_t attempt);
  void SendNext();
  void OnEmbedReply(uint64_t attempt,
                    const std::optional<std::vector<float>>& vectors);
  void OnCallTimeout(uint64_t attempt);
  void OnDisconnected(uint64_t attempt);
  void OnProcessExited(uint64_t attempt);
  void OnExitTimeout(uint64_t attempt);
  void OnFootprintTimer();
  void OnFootprintSample(uint64_t attempt, std::optional<uint64_t> footprint);
  void OnIdle();
  void OnCriticalMemoryPressure();

  // The current process crashed or broke the protocol: everything queued
  // fails with kFailed, it is counted against the model (unless it was
  // idle) and it ends.
  void Lost(CrashLoopGuard::Phase phase, bool kill);
  // Ends the current process (killing it now when |kill|).
  void DropProcess(bool kill);
  void FailAll(EmbeddingStatus status);
  // Forgets an exited process; runs the Unload() waiters once none is left.
  void RemoveProcess(uint64_t attempt);
  ProcessObserver* FindProcess(uint64_t attempt);
  void ArmIdle();
  CrashLoopGuard::Phase CurrentPhase() const;
  int64_t NowMs() const;

  SEQUENCE_CHECKER(sequence_checker_);

  Opener opener_;
  Launcher launcher_;
  Timeouts timeouts_;
  bool shutting_down_ = false;

  std::optional<EmbeddingModelConfig> config_;
  State state_ = State::kNone;
  // Bumped whenever a process is dropped: replies and timers of an older
  // attempt are ignored.
  uint64_t attempt_ = 1;
  mojo::Remote<mojom::MoltInferenceService> service_;
  mojo::Remote<mojom::EmbeddingModel> model_;
  std::string crash_key_;
  CrashLoopGuard crash_guard_;

  base::circular_deque<std::unique_ptr<Request>> queue_;
  std::unique_ptr<Request> in_flight_;

  base::OneShotTimer load_deadline_;
  base::OneShotTimer call_deadline_;
  base::OneShotTimer idle_timer_;
  base::RepeatingTimer footprint_timer_;

  // The current process (last) and those still exiting.
  std::vector<std::unique_ptr<ProcessObserver>> processes_;
  // Unload() callers waiting for the processes to exit.
  std::vector<base::OnceClosure> exit_waiters_;

  std::unique_ptr<PressureListener> pressure_listener_;
  std::unique_ptr<base::MemoryPressureListenerRegistration>
      pressure_registration_;

  base::WeakPtrFactory<EmbeddingProcessHost> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_RUNTIME_EMBEDDING_PROCESS_HOST_H_
