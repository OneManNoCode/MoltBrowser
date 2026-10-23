// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltAIManager needs from the on-device runtime. The real one is
// RuntimeWebAIBackend (BrowserAIRuntime, i.e. the sandboxed inference
// process); molt_ai_unittests uses a fake. UI thread only.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "third_party/skia/include/core/SkBitmap.h"

namespace molt_ai::web_ai {

// One generation for a page. The text fields carry molt_ai::ChatPrompt's
// trust labels: system_prompt and context_preamble are browser-authored;
// context, history and user_message are page text (neutralized by the
// inference service). Images are the page's too, and go with user turns
// only, at most kMaxContextImages in all.
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
  // Empty, or one entry per |history| entry: its images.
  std::vector<std::vector<ModelImage>> history_images;
  std::string user_message;
  std::vector<ModelImage> user_images;

  uint32_t max_output_tokens = 0;
  Sampling sampling = kDefaultSampling;
  // KV-cache reuse across the turns of one LanguageModel session; empty for
  // the writing-assistance APIs.
  std::string conversation_key;
  // LanguageModel responseConstraint: a JSON Schema as JSON text (Blink's
  // JSON.stringify of the page's object), enforced by the inference
  // service's grammar sampler; "" = none. Page text: only the sandboxed
  // service parses it. On kOk the reply is complete JSON.
  std::string json_schema;
};

enum class GenerationStatus {
  kOk,
  // Cancelled by MoltBrowser (deadline) or the runtime (shutdown).
  kCancelled,
  // Refused by the per-site or global limits.
  kThrottled,
  // No model could be loaded, or the model process failed.
  kFailed,
  // json_schema is outside what the on-device model can enforce.
  kConstraintUnsupported,
  // A constrained reply that isn't complete JSON.
  kInvalidOutput,
  // A constrained reply cut off by the reply budget.
  kOutputTruncated,
};

class WebAIBackend {
 public:
  using DeviceCallback = base::OnceCallback<void(DeviceFacts)>;
  // |context_tokens|: the loaded model's context window (0 if unknown).
  // |sees_images|: the loaded model has its vision projector (image input).
  using PrepareCallback = base::OnceCallback<
      void(bool ok, uint32_t context_tokens, bool sees_images)>;
  using ImagesCallback =
      base::OnceCallback<void(std::optional<std::vector<ModelImage>>)>;
  using ChunkCallback = base::RepeatingCallback<void(const std::string&)>;
  using DoneCallback = base::OnceCallback<void(GenerationStatus)>;

  virtual ~WebAIBackend() = default;

  // Device facts (runtime support, installed models, settings.json).
  // Answers asynchronously, never re-entrantly.
  virtual void GetDeviceFacts(DeviceCallback callback) = 0;

  // The user's switch for these APIs ("web_ai_apis" in settings.json, which
  // molt://ai-settings sets), as last read or set: what every call of an
  // existing session is checked against (MaySessionServe()). Backends
  // without the switch (test fakes) report it on.
  virtual bool WebApisSwitchOn();

  // Loads the model pages use (PickWebModel(), preferring one that sees
  // images if |wants_images|) if none is loaded. Only called once the user
  // allowed the site.
  virtual void PrepareModel(bool wants_images, PrepareCallback callback) = 0;

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

  // Turns a page's bitmaps (decoded by Blink, any size up to
  // kMaxImageSourcePixels) into model images, scaled to kWebImageMaxSide
  // (runtime/model_image_prep.h), off the UI thread. nullopt if any can't
  // be used. Never calls back synchronously.
  virtual void PrepareImages(std::vector<SkBitmap> bitmaps,
                             ImagesCallback callback) = 0;

  // Model images held for pages, per site and in total
  // (kMaxHeldImagesPerSite, kMaxHeldImagesTotal): false, and nothing held,
  // over either cap.
  virtual bool TryHoldImages(const std::string& site_key, size_t count) = 0;
  virtual void ReleaseImages(const std::string& site_key, size_t count) = 0;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_BACKEND_H_
