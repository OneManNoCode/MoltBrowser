// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// WebAIBackend over BrowserAIRuntime: pages' generations run on the same
// sandboxed on-device model as MoltBrowser's own chat.
//
// Sharing the one model with the chat:
//  * Pages use whatever model is loaded, so they never swap out the chat's
//    model; with none loaded they load PickWebModel()'s.
//  * One page generation at a time (GenerationScheduler), so the user's
//    chat, which goes to the runtime directly, waits behind at most one
//    page request; visible pages first; per-site and global caps.
//  * Each page generation has a deadline (kGenerationDeadline, model load
//    included), a reply budget and a small hidden-reasoning budget.
//
// UI thread only; the runtime's blocking calls run on ThreadPool workers.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_RUNTIME_WEB_AI_BACKEND_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_RUNTIME_WEB_AI_BACKEND_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai {
class BrowserAIRuntime;
}  // namespace molt_ai

namespace molt_ai::web_ai {

class RuntimeWebAIBackend : public WebAIBackend {
 public:
  // A page generation (and the model load before it, if any) is cancelled
  // after this long.
  static constexpr base::TimeDelta kGenerationDeadline = base::Minutes(5);
  // Device facts are re-read (a disk check of the model files and
  // settings.json) at most this often.
  static constexpr base::TimeDelta kDeviceFactsLifetime = base::Seconds(30);

  // Process-wide, like BrowserAIRuntime.
  static RuntimeWebAIBackend* GetInstance();

  RuntimeWebAIBackend(const RuntimeWebAIBackend&) = delete;
  RuntimeWebAIBackend& operator=(const RuntimeWebAIBackend&) = delete;

  // WebAIBackend:
  void GetDeviceFacts(DeviceCallback callback) override;
  void PrepareModel(PrepareCallback callback) override;
  uint64_t Generate(GenerationJob job,
                    ChunkCallback on_chunk,
                    DoneCallback on_done) override;
  void Cancel(uint64_t id) override;
  bool TryAddSession(const std::string& site_key) override;
  void RemoveSession(const std::string& site_key) override;

  // settings.json changed (molt://ai-settings turned the APIs on or off):
  // the next request reads the device facts again instead of using the
  // cached ones.
  void InvalidateDeviceFacts();

 private:
  friend class base::NoDestructor<RuntimeWebAIBackend>;
  struct Job;
  struct PrepareResult {
    bool ok = false;
    uint32_t context_tokens = 0;
  };

  RuntimeWebAIBackend();
  ~RuntimeWebAIBackend() override;

  // The runtime, initialized on first use (as the chat does).
  BrowserAIRuntime& runtime();

  void OnDeviceFacts(DeviceFacts facts);
  void OnModelPrepared(PrepareResult result);
  void MaybeStartNext();
  void OnJobChunk(uint64_t id, std::string text);
  void OnJobDone(uint64_t id, GenerationStatus status);
  void OnJobDeadline(uint64_t id);

  // Runs on a worker.
  static DeviceFacts ReadDeviceFacts(BrowserAIRuntime* runtime);
  static PrepareResult PrepareBlocking(BrowserAIRuntime* runtime);
  static GenerationStatus RunJob(
      BrowserAIRuntime* runtime,
      GenerationJob job,
      uint64_t request_id,
      scoped_refptr<base::SequencedTaskRunner> ui_task_runner,
      base::WeakPtr<RuntimeWebAIBackend> backend,
      uint64_t id);

  bool runtime_initialized_ = false;

  GenerationScheduler scheduler_;
  std::map<uint64_t, std::unique_ptr<Job>> jobs_;
  uint64_t next_id_ = 1;

  std::optional<DeviceFacts> device_facts_;
  base::TimeTicks device_facts_time_;
  bool device_facts_pending_ = false;
  // The read in flight started before InvalidateDeviceFacts(): answer its
  // waiters but don't cache it.
  bool discard_pending_facts_ = false;
  std::vector<DeviceCallback> device_waiters_;

  bool prepare_pending_ = false;
  std::vector<PrepareCallback> prepare_waiters_;

  base::WeakPtrFactory<RuntimeWebAIBackend> weak_factory_{this};
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_RUNTIME_WEB_AI_BACKEND_H_
