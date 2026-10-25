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
//  * A page's images (Prompt API image input) are scaled to
//    kWebImageMaxSide on a worker; the images pages hold are capped per
//    site and in total (GenerationScheduler). A page's audio clips (audio
//    input; decoded and resampled to 16 kHz mono by Blink in the renderer)
//    are checked and turned into 16-bit samples (runtime/model_audio.h),
//    and count as images against the same caps.
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

#include "base/callback_list.h"
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
  // settings.json) at most this often for each site.
  static constexpr base::TimeDelta kDeviceFactsLifetime = base::Seconds(30);

  // Process-wide, like BrowserAIRuntime.
  static RuntimeWebAIBackend* GetInstance();

  RuntimeWebAIBackend(const RuntimeWebAIBackend&) = delete;
  RuntimeWebAIBackend& operator=(const RuntimeWebAIBackend&) = delete;

  // WebAIBackend:
  // The browser's own callers (page translation) have an entry of their
  // own, apart from every site's.
  void GetDeviceFacts(DeviceCallback callback) override;
  void GetDeviceFactsForSite(const std::string& site_key,
                             DeviceCallback callback) override;
  bool WebApisSwitchOn() override;
  void PrepareModel(bool wants_images,
                    bool wants_audio,
                    PrepareCallback callback) override;
  uint64_t Generate(GenerationJob job,
                    ChunkCallback on_chunk,
                    DoneCallback on_done) override;
  void Cancel(uint64_t id) override;
  bool TryAddSession(const std::string& site_key) override;
  void RemoveSession(const std::string& site_key) override;
  void PrepareImages(std::vector<SkBitmap> bitmaps,
                     ImagesCallback callback) override;
  bool TryHoldImages(const std::string& site_key, size_t count) override;
  void ReleaseImages(const std::string& site_key, size_t count) override;

  // molt://ai-settings wrote |enabled| to settings.json's "web_ai_apis":
  // sessions are checked against it at once (WebApisSwitchOn()), the
  // device facts are read again, and the observers hear of it.
  void SetWebApisSwitch(bool enabled);

  // settings.json may have changed (molt://ai-settings couldn't write it):
  // every site's next request reads the device facts again instead of
  // using its cached ones.
  void InvalidateDeviceFacts();

  // Runs |on_change| when the "web_ai_apis" switch changes (set from
  // molt://ai-settings, or read changed from a hand edit).
  base::CallbackListSubscription ObserveWebApisSwitch(
      base::RepeatingClosure on_change);

 private:
  friend class base::NoDestructor<RuntimeWebAIBackend>;
  struct Job;
  struct PrepareResult {
    bool ok = false;
    uint32_t context_tokens = 0;
    bool sees_images = false;
    bool hears_audio = false;
  };

  RuntimeWebAIBackend();
  ~RuntimeWebAIBackend() override;

  // The runtime, initialized on first use (as the chat does).
  BrowserAIRuntime& runtime();

  void OnDeviceFacts(const std::string& site_key,
                     uint64_t ticket,
                     DeviceFacts facts);
  // Sets the switch's value; tells the observers if it changed.
  void UpdateWebApisSwitch(bool enabled);
  void OnModelPrepared(PrepareResult result);
  void MaybeStartNext();
  void OnJobChunk(uint64_t id, std::string text);
  void OnJobDone(uint64_t id, GenerationStatus status);
  void OnJobDeadline(uint64_t id);

  // Runs on a worker.
  static DeviceFacts ReadDeviceFacts(BrowserAIRuntime* runtime);
  static PrepareResult PrepareBlocking(BrowserAIRuntime* runtime,
                                       bool wants_images,
                                       bool wants_audio);
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

  // Each site's device facts (and the browser's own, under ""), so that
  // one site's calls never make another's faster or slower.
  SiteDeviceFacts site_facts_{kDeviceFactsLifetime.InMicroseconds()};
  // The calls waiting for each read in flight, by ticket.
  std::map<uint64_t, std::vector<DeviceCallback>> device_waiters_;

  // The "web_ai_apis" switch, as last read or set (on until read), and the
  // read it was last taken from: an older read finishing later doesn't
  // overwrite it.
  bool web_apis_switch_ = true;
  uint64_t web_apis_switch_ticket_ = 0;
  base::RepeatingClosureList web_apis_switch_observers_;

  bool prepare_pending_ = false;
  std::vector<PrepareCallback> prepare_waiters_;

  base::WeakPtrFactory<RuntimeWebAIBackend> weak_factory_{this};
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_RUNTIME_WEB_AI_BACKEND_H_
