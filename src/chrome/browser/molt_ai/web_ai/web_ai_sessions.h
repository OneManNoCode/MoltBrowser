// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The session objects behind LanguageModel, Summarizer, Writer and Rewriter
// (the Translator's is in web_ai_translator_session.h). Owned by
// MoltAIManager. Each serves its requests one at a time, in order (a prompt
// sees the appends before it), through the backend's queue; the streamed
// text reaches the page at most every kStreamIntervalMs. A prompt with a
// JSON-schema responseConstraint is decoded under that constraint (the
// inference service's grammar sampler) and its reply reaches the page
// whole, only after it parsed as JSON here.
//
// Image input (a LanguageModel created with expectedInputs "image"): a
// request's bitmaps are held (WebAIBackend::TryHoldImages()) and turned
// into model images on a worker as soon as it arrives; it starts only once
// they are ready, so requests still run in order.
//
// Ending a request: the page aborting it (its responder pipe closes) cancels
// the generation; destroy(), the page dropping the session, a navigation or
// the frame going away (MoltAIManager destroyed) cancel everything.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SESSIONS_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SESSIONS_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_backend.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_prompts.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/ai/ai_language_model.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_rewriter.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_summarizer.mojom.h"
#include "third_party/blink/public/mojom/ai/ai_writer.mojom.h"
#include "third_party/blink/public/mojom/ai/model_streaming_responder.mojom.h"

namespace molt_ai::web_ai {

// A page's bitmap (decoded by Blink) the model may be shown, scaled down:
// it has pixels, and IsUsableImageSource() holds for its size. Checked
// before any work is done on it; mojo's bitmap traits already refuse a
// bitmap without pixels, so that half only guards against their change.
bool IsUsableBitmapForModel(const SkBitmap& bitmap);

class WebAISession {
 public:
  WebAISession(MoltAIManager& manager, uint64_t id, WebAIApi api);
  WebAISession(const WebAISession&) = delete;
  WebAISession& operator=(const WebAISession&) = delete;
  // Cancels the running generation and fails every waiting request with
  // kErrorSessionDestroyed.
  virtual ~WebAISession();

  uint64_t id() const { return id_; }
  WebAIApi api() const { return api_; }

 protected:
  // A request waiting in, or at the head of, the session's queue.
  struct Request {
    Request();
    Request(Request&&);
    Request& operator=(Request&&);
    ~Request();

    uint64_t id = 0;
    mojo::Remote<blink::mojom::ModelStreamingResponder> responder;
    // LanguageModel: the turns a prompt or append adds, and (once started)
    // the plan for adding them.
    std::vector<Turn> turns;
    bool append_only = false;
    Conversation::Plan plan;
    // LanguageModel responseConstraint (a JSON Schema; "" = none). Moved
    // into the job when it starts; |constrained| stays: a constrained reply
    // reaches the page only whole, once it parsed as JSON.
    std::string json_schema;
    bool constrained = false;
    // LanguageModel image input: the turn of |turns| each of the request's
    // images belongs to, in order. Until they are ready (|images_pending|)
    // the request can't start. |held_images| of the session's image hold
    // are this request's.
    std::vector<size_t> image_turns;
    bool images_pending = false;
    size_t held_images = 0;
    // Summarizer / Writer / Rewriter: the prompt, built when queued.
    TaskPrompt task;
    // Translator: the input in pieces, and the next one to look at.
    std::vector<TranslationPiece> pieces;
    size_t next_piece = 0;
  };

  // Queues |request| (failing it if the API was turned off since create(),
  // MoltAIManager::MayServe(), or if kMaxQueuedPerSession wait already).
  // |bitmaps|, one per entry of |request.image_turns|, are held and turned
  // into model images first (failing the request with a generic error over
  // the image caps, and as an invalid request if they can't be used).
  void Enqueue(Request request, std::vector<SkBitmap> bitmaps = {});

  // The images the session's context keeps (held with its requests').
  virtual size_t ContextImages() const;
  // Adds |count| to the session's image hold; false over the caps.
  bool HoldImages(size_t count);

  // The request at the head of the queue starts. Returns the generation to
  // run, or nullopt if the request was answered without the model (an
  // append, an error).
  virtual std::optional<GenerationJob> Begin(Request& request) = 0;
  // The generation for |request| completed with |reply|. Commits what must
  // be committed, sends what precedes the completion (contextoverflow) and
  // returns the context info for OnCompletion().
  virtual blink::mojom::ModelExecutionContextInfoPtr Commit(
      Request& request,
      std::string reply) = 0;
  // A constrained generation of |request| completed with |reply| (complete
  // JSON): the text the page gets for it. By default the reply itself;
  // nullopt fails the request (kInvalidOutput).
  virtual std::optional<std::string> ConstrainedText(Request& request,
                                                     const std::string& reply);
  // A request answered in several generations (Translator: one per piece
  // of a long input). Called after each successful generation, once its
  // text was sent: the next generation for |request|, which keeps running,
  // or nullopt when it is complete (then Commit() and OnCompletion()). By
  // default every request takes one generation.
  virtual std::optional<GenerationJob> NextJob(Request& request);

  // A job with the session's scheduling fields filled in.
  GenerationJob NewJob();
  // Ends the session (destroy() or the session's own pipe closed).
  void Close();

  MoltAIManager& manager() { return *manager_; }

 private:
  void MaybeStartNext();
  // Runs |job| for the running request.
  void StartJob(GenerationJob job);
  void OnChunk(uint64_t request_id, const std::string& text);
  void OnDone(uint64_t request_id, GenerationStatus status);
  void OnResponderDisconnected(uint64_t request_id);
  void Flush();
  void OnImagesReady(uint64_t request_id,
                     std::optional<std::vector<ModelImage>> images);
  // Lets go of the images the session holds beyond its waiting and running
  // requests' and its context's (never takes more).
  void SyncHeldImages();

  // MoltAIManager::MayServe() for the session's API, read now: checked when
  // a request arrives, when it starts and between the generations of one
  // request, so a request fails once the APIs are turned off for the page
  // (a switch, a policy, the site's answer taken back), as create() would.
  bool MayServe();

  const raw_ref<MoltAIManager> manager_;
  const uint64_t id_;
  const WebAIApi api_;
  const std::string site_key_;
  std::deque<Request> queue_;
  std::optional<Request> running_;
  uint64_t job_id_ = 0;
  uint64_t next_request_id_ = 1;
  std::string reply_;
  std::string unsent_;
  base::TimeTicks last_flush_;
  const base::TimeDelta stream_interval_;
  bool closed_ = false;
  size_t held_images_ = 0;

  base::WeakPtrFactory<WebAISession> weak_factory_{this};
};

// LanguageModel (the Prompt API).
class LanguageModelSession : public WebAISession,
                             public blink::mojom::AILanguageModel {
 public:
  struct Config {
    Config();
    Config(const Config&);
    Config& operator=(const Config&);
    ~Config();

    Sampling sampling = kDefaultSampling;
    std::optional<blink::mojom::AILanguageModelSamplingMode> sampling_mode;
    uint32_t max_reply_tokens = kSmallMaxReplyTokens;
    // Built from validated language tags only.
    std::string system_prompt;
    // Created with expectedInputs "image" on a model that sees images:
    // user turns may hold images.
    bool image_input = false;
  };

  LanguageModelSession(
      MoltAIManager& manager,
      uint64_t id,
      Config config,
      Conversation conversation,
      mojo::PendingReceiver<blink::mojom::AILanguageModel> receiver);
  ~LanguageModelSession() override;

  blink::mojom::AILanguageModelInstanceInfoPtr GetInfo() const;
  const Config& config() const { return config_; }
  const Conversation& conversation() const { return conversation_; }
  // A clone holds the images of the context it was given; false over the
  // caps (the clone can't be made).
  bool HoldContextImages();

  // blink::mojom::AILanguageModel:
  void Prompt(std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
              on_device_model::mojom::ResponseConstraintPtr constraint,
              mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                  pending_responder) override;
  void Append(std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
              mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                  pending_responder) override;
  void Fork(
      mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
          client) override;
  void Destroy() override;
  void MeasureInputUsage(
      std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
      MeasureInputUsageCallback callback) override;

 private:
  // WebAISession:
  std::optional<GenerationJob> Begin(Request& request) override;
  blink::mojom::ModelExecutionContextInfoPtr Commit(Request& request,
                                                    std::string reply) override;
  size_t ContextImages() const override;

  void Queue(std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
             bool append_only,
             std::string json_schema,
             mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                 pending_responder);

  const Config config_;
  Conversation conversation_;
  const std::string conversation_key_;
  mojo::Receiver<blink::mojom::AILanguageModel> receiver_;
};

// The shared part of Summarizer, Writer and Rewriter: stateless requests
// built from the create() options, within kWritingAssistanceInputQuota.
class WritingSession : public WebAISession {
 public:
  WritingSession(MoltAIManager& manager,
                 uint64_t id,
                 WebAIApi api,
                 std::string shared_context);
  ~WritingSession() override;

 protected:
  // The prompt for one request, from the create() options (web_ai_prompts).
  virtual TaskPrompt BuildPrompt(std::string_view shared_context,
                                 std::string_view context,
                                 std::string_view input) const = 0;

  void Execute(const std::string& input,
               const std::string& context,
               mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                   pending_responder);
  void Measure(const std::string& input,
               const std::string& context,
               base::OnceCallback<void(std::optional<uint32_t>)> callback);

 private:
  // WebAISession:
  std::optional<GenerationJob> Begin(Request& request) override;
  blink::mojom::ModelExecutionContextInfoPtr Commit(Request& request,
                                                    std::string reply) override;

  const std::string shared_context_;
};

class SummarizerSession : public WritingSession,
                          public blink::mojom::AISummarizer {
 public:
  SummarizerSession(MoltAIManager& manager,
                    uint64_t id,
                    std::string shared_context,
                    SummarizerOptions options,
                    mojo::PendingReceiver<blink::mojom::AISummarizer> receiver);
  ~SummarizerSession() override;

  // blink::mojom::AISummarizer:
  void Summarize(const std::string& input,
                 const std::string& context,
                 mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                     pending_responder) override;
  void MeasureUsage(const std::string& input,
                    const std::string& context,
                    MeasureUsageCallback callback) override;

 private:
  // WritingSession:
  TaskPrompt BuildPrompt(std::string_view shared_context,
                         std::string_view context,
                         std::string_view input) const override;

  const SummarizerOptions options_;
  mojo::Receiver<blink::mojom::AISummarizer> receiver_;
};

class WriterSession : public WritingSession, public blink::mojom::AIWriter {
 public:
  WriterSession(MoltAIManager& manager,
                uint64_t id,
                std::string shared_context,
                WriterOptions options,
                mojo::PendingReceiver<blink::mojom::AIWriter> receiver);
  ~WriterSession() override;

  // blink::mojom::AIWriter:
  void Write(const std::string& input,
             const std::optional<std::string>& context,
             mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                 pending_responder) override;
  void MeasureUsage(const std::string& input,
                    const std::string& context,
                    MeasureUsageCallback callback) override;

 private:
  // WritingSession:
  TaskPrompt BuildPrompt(std::string_view shared_context,
                         std::string_view context,
                         std::string_view input) const override;

  const WriterOptions options_;
  mojo::Receiver<blink::mojom::AIWriter> receiver_;
};

class RewriterSession : public WritingSession, public blink::mojom::AIRewriter {
 public:
  RewriterSession(MoltAIManager& manager,
                  uint64_t id,
                  std::string shared_context,
                  RewriterOptions options,
                  mojo::PendingReceiver<blink::mojom::AIRewriter> receiver);
  ~RewriterSession() override;

  // blink::mojom::AIRewriter:
  void Rewrite(const std::string& input,
               const std::optional<std::string>& context,
               mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
                   pending_responder) override;
  void MeasureUsage(const std::string& input,
                    const std::string& context,
                    MeasureUsageCallback callback) override;

 private:
  // WritingSession:
  TaskPrompt BuildPrompt(std::string_view shared_context,
                         std::string_view context,
                         std::string_view input) const override;

  const RewriterOptions options_;
  mojo::Receiver<blink::mojom::AIRewriter> receiver_;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SESSIONS_H_
