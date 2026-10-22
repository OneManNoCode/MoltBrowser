// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltAIManager needs from the on-device runtime. The real one is
// RuntimeWebAIBackend (BrowserAIRuntime, i.e. the sandboxed inference
// process); molt_ai_unittests uses a fake. UI thread only.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai::web_ai {

// One generation for a page. The text fields carry molt_ai::ChatPrompt's
// trust labels: system_prompt and context_preamble are browser-authored;
// context, history and user_message are page text (neutralized by the
// inference service).
struct GenerationJob {
  GenerationJob();
  GenerationJob(GenerationJob&&);
  GenerationJob& operator=(GenerationJob&&);
  ~GenerationJob();

  // Scheduling: the top-level site (within its profile) and whether the
  // page is visible.
  std::string site_key;
  bool foreground = true;

  std::string system_prompt;
  std::string context_preamble;
  std::string context;
  std::vector<std::pair<Role, std::string>> history;
  std::string user_message;

  uint32_t max_output_tokens = 0;
  Sampling sampling = kDefaultSampling;
  // KV-cache reuse across the turns of one LanguageModel session; empty for
  // the writing-assistance APIs.
  std::string conversation_key;
};

enum class GenerationStatus {
  kOk,
  // Cancelled by MoltBrowser (deadline) or the runtime (shutdown).
  kCancelled,
  // Refused by the per-site or global limits.
  kThrottled,
  // No model could be loaded, or the model process failed.
  kFailed,
};

class WebAIBackend {
 public:
  using DeviceCallback = base::OnceCallback<void(DeviceFacts)>;
  // |context_tokens|: the loaded model's context window (0 if unknown).
  using PrepareCallback =
      base::OnceCallback<void(bool ok, uint32_t context_tokens)>;
  using ChunkCallback = base::RepeatingCallback<void(const std::string&)>;
  using DoneCallback = base::OnceCallback<void(GenerationStatus)>;

  virtual ~WebAIBackend() = default;

  // Device facts (runtime support, installed models, settings.json).
  // Answers asynchronously, never re-entrantly.
  virtual void GetDeviceFacts(DeviceCallback callback) = 0;

  // Loads the model pages use (PickWebModel()) if none is loaded. Only
  // called once the user allowed the site.
  virtual void PrepareModel(PrepareCallback callback) = 0;

  // Queues |job|. Text arrives through |on_chunk|, then |on_done| runs once.
  // Never calls back synchronously. Returns a non-zero id for Cancel().
  virtual uint64_t Generate(GenerationJob job,
                            ChunkCallback on_chunk,
                            DoneCallback on_done) = 0;

  // Cancels |id| (queued or running). Neither callback runs afterwards.
  virtual void Cancel(uint64_t id) = 0;

  // Per-site session slots (kMaxSessionsPerSite).
  virtual bool TryAddSession(const std::string& site_key) = 0;
  virtual void RemoveSession(const std::string& site_key) = 0;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_
