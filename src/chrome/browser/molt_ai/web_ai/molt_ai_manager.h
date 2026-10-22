// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser's implementation of blink::mojom::AIManager and
// blink::mojom::TranslationManager: the browser side of the web's built-in
// AI APIs (LanguageModel, Summarizer, Writer, Rewriter, Translator), served
// by the user's own on-device model through BrowserAIRuntime instead of
// upstream's AIManager (Gemini Nano via the optimization guide) and
// TranslationManagerImpl (TranslateKit via the component updater). Bound by
// ChromeContentBrowserClient::BindAIManager and BindTranslationManager (see
// molt_ai_manager_binder.h) for every document and worker; one instance per
// document or worker, owned by its user data and shared by both
// interfaces, so navigating away or closing the frame destroys it and every
// session, cancelling their generations. Design: docs/design/WEB_AI_APIS.md.
//
// Everything a page sends is untrusted: sizes are checked before anything
// is stored, text goes to the model only in the fields the inference service
// neutralizes, and the renderer is never CHECKed.
//
// LanguageDetector, Proofreader and the semantic embedder are not served:
// they report "unavailable" and create() fails.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "services/on_device_model/public/mojom/download_observer.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_common.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_manager.mojom.h"
#include "third_party/blink/public/mojom/on_device_translation/translation_manager.mojom.h"

namespace molt_ai::web_ai {

class WebAISession;
class LanguageModelSession;

enum class WebAIApi {
  kLanguageModel,
  kSummarizer,
  kWriter,
  kRewriter,
  kTranslator,
};

// What MoltAIManager needs to know about the document or worker it serves
// (molt_ai_manager_binder.cc implements it over RenderFrameHost and
// Profile; tests use a fake).
class WebAIEnvironment {
 public:
  virtual ~WebAIEnvironment() = default;

  // For kTranslator, |enterprise_allowed| also covers the
  // TranslatorAPIAllowed policy.
  virtual ContextFacts GetContextFacts(WebAIApi api) = 0;
  // The "language-model", "summarizer", "writer", "rewriter" and
  // "translator" permissions policy features. Blink checks them first, so
  // false here means a renderer that skipped the check.
  virtual bool IsAllowedByPermissionsPolicy(WebAIApi api) = 0;
  // The user's answer for the top-level origin.
  virtual SiteConsent GetConsent() = 0;
  // Asks the user (an infobar on the tab). |needs_model|: no model is
  // downloaded yet, so the prompt offers the model picker. Answers kAsk if
  // the prompt was closed without an answer or couldn't be shown.
  virtual void RequestConsent(
      bool needs_model,
      base::OnceCallback<void(SiteConsent)> callback) = 0;
  // The top-level site within its profile: the key of the per-site limits.
  // Never shown or sent anywhere.
  virtual std::string GetSiteKey() = 0;
  // The page is visible (its requests go first).
  virtual bool IsForeground() = 0;
  // Calls |on_change| after the user's answers may have changed, for any
  // site (the settings page, Clear browsing data, a prompt in another tab).
  // The default never calls it.
  virtual void ObserveConsent(base::RepeatingClosure on_change) {}
};

class MoltAIManager : public base::SupportsUserData::Data,
                      public blink::mojom::AIManager,
                      public blink::mojom::TranslationManager {
 public:
  // |backend| outlives this (the real one is a process-wide singleton).
  MoltAIManager(std::unique_ptr<WebAIEnvironment> environment,
                WebAIBackend* backend);
  MoltAIManager(const MoltAIManager&) = delete;
  MoltAIManager& operator=(const MoltAIManager&) = delete;
  ~MoltAIManager() override;

  void AddReceiver(mojo::PendingReceiver<blink::mojom::AIManager> receiver);
  void AddTranslationReceiver(
      mojo::PendingReceiver<blink::mojom::TranslationManager> receiver);

  // blink::mojom::AIManager:
  void CanCreateLanguageModel(
      blink::mojom::AILanguageModelCreateOptionsPtr options,
      CanCreateLanguageModelCallback callback) override;
  void CreateLanguageModel(
      mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
          client,
      blink::mojom::AILanguageModelCreateOptionsPtr options,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;
  void CanCreateSummarizer(blink::mojom::AISummarizerCreateOptionsPtr options,
                           CanCreateSummarizerCallback callback) override;
  void CreateSummarizer(
      mojo::PendingRemote<blink::mojom::AIManagerCreateSummarizerClient> client,
      blink::mojom::AISummarizerCreateOptionsPtr options,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;
  void GetLanguageModelParams(GetLanguageModelParamsCallback callback) override;
  void CanCreateWriter(blink::mojom::AIWriterCreateOptionsPtr options,
                       CanCreateWriterCallback callback) override;
  void CreateWriter(
      mojo::PendingRemote<blink::mojom::AIManagerCreateWriterClient> client,
      blink::mojom::AIWriterCreateOptionsPtr options,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;
  void CanCreateRewriter(blink::mojom::AIRewriterCreateOptionsPtr options,
                         CanCreateRewriterCallback callback) override;
  void CreateRewriter(
      mojo::PendingRemote<blink::mojom::AIManagerCreateRewriterClient> client,
      blink::mojom::AIRewriterCreateOptionsPtr options,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;
  void CanCreateProofreader(blink::mojom::AIProofreaderCreateOptionsPtr options,
                            CanCreateProofreaderCallback callback) override;
  void CreateProofreader(
      mojo::PendingRemote<blink::mojom::AIManagerCreateProofreaderClient>
          client,
      blink::mojom::AIProofreaderCreateOptionsPtr options,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;
  void CanCreateSemanticEmbedder(
      CanCreateSemanticEmbedderCallback callback) override;
  void CreateSemanticEmbedder(
      mojo::PendingRemote<blink::mojom::AIManagerCreateSemanticEmbedderClient>
          client,
      mojo::PendingRemote<on_device_model::mojom::DownloadObserver> monitor)
      override;

  // blink::mojom::TranslationManager (the Translator API; the language
  // table and the pieces are in web_ai_translation.h):
  void CreateTranslator(
      mojo::PendingRemote<blink::mojom::TranslationManagerCreateTranslatorClient>
          client,
      blink::mojom::TranslatorCreateOptionsPtr options) override;
  void TranslationAvailable(blink::mojom::TranslatorLanguageCodePtr source_lang,
                            blink::mojom::TranslatorLanguageCodePtr target_lang,
                            TranslationAvailableCallback callback) override;

  // For the sessions.
  WebAIBackend* backend() { return backend_; }
  const std::string& site_key();
  bool IsForeground();
  // The context may still use |api| (IsContextEligible(): the feature,
  // policies, the On-device AI switch), read now.
  bool IsContextStillEligible(WebAIApi api);
  // Ends session |session_id| (destroy() or its pipe closed). It is
  // deleted asynchronously, never while it is still on the stack.
  void CloseSession(uint64_t session_id);
  // LanguageModel.clone().
  void ForkLanguageModel(
      const LanguageModelSession& source,
      mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
          client);

  // How often streamed text reaches the page (kStreamIntervalMs).
  base::TimeDelta stream_interval() const { return stream_interval_; }
  void set_stream_interval_for_testing(base::TimeDelta interval) {
    stream_interval_ = interval;
  }

  size_t session_count_for_testing() const { return sessions_.size(); }
  size_t pending_create_count_for_testing() const { return creates_.size(); }

 private:
  class CreateRequest;
  class LanguageModelCreate;
  // Summarizer, Writer and Rewriter.
  template <typename Client,
            typename SessionInterface,
            typename Session,
            typename Options>
  class WritingCreate;
  class TranslatorCreate;

  // Each interface turns the decision into its own mojo answer.
  using AvailabilityCallback = base::OnceCallback<void(AvailabilityDecision)>;

  // availability() for |api|.
  void CheckAvailability(WebAIApi api,
                         bool options_supported,
                         bool languages_supported,
                         AvailabilityCallback callback);
  void OnAvailabilityDeviceFacts(WebAIApi api,
                                 bool options_supported,
                                 bool languages_supported,
                                 AvailabilityCallback callback,
                                 DeviceFacts device);

  // The user's answers changed: if this site's is no longer Allow (removed
  // or blocked in molt://ai-settings, cleared), its sessions end.
  void OnConsentChanged();
  void CloseSessionsIfNotAllowed();

  // create(): availability, the user's consent, then the model.
  void StartCreate(std::unique_ptr<CreateRequest> request);
  void OnCreateDeviceFacts(uint64_t id, DeviceFacts device);
  void OnCreateConsent(uint64_t id, SiteConsent consent);
  void OnCreateModelReady(uint64_t id, bool ok, uint32_t context_tokens);
  // Removes and returns request |id| if it is still wanted (its client is
  // connected); fails and drops it otherwise.
  std::unique_ptr<CreateRequest> TakeCreate(uint64_t id);
  CreateRequest* FindCreate(uint64_t id);

  // A new session's id, and a slot for it under the per-site limit (0 if
  // the site has too many sessions).
  uint64_t ReserveSession();
  // Takes ownership of a session made with an id from ReserveSession().
  void AdoptSession(std::unique_ptr<WebAISession> session);
  void DeleteSession(uint64_t session_id);

  std::unique_ptr<WebAIEnvironment> environment_;
  raw_ptr<WebAIBackend> backend_;
  std::optional<std::string> site_key_;
  base::TimeDelta stream_interval_ = base::Milliseconds(kStreamIntervalMs);

  mojo::ReceiverSet<blink::mojom::AIManager> receivers_;
  mojo::ReceiverSet<blink::mojom::TranslationManager> translation_receivers_;
  std::map<uint64_t, std::unique_ptr<CreateRequest>> creates_;
  uint64_t next_create_id_ = 1;
  std::map<uint64_t, std::unique_ptr<WebAISession>> sessions_;
  uint64_t next_session_id_ = 1;

  base::WeakPtrFactory<MoltAIManager> weak_factory_{this};
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_MOLT_AI_MANAGER_H_
