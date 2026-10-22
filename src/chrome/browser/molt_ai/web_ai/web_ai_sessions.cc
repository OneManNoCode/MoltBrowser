// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_sessions.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::web_ai {

namespace {

using blink::mojom::ModelStreamingResponseStatus;

// The largest responseConstraint schema the inference service takes
// (molt_ai.mojom.kMaxConstraintSchemaBytes).
constexpr size_t kMaxResponseConstraintBytes = 32 * 1024;

blink::mojom::ModelExecutionContextInfoPtr ContextInfo(uint32_t usage) {
  return blink::mojom::ModelExecutionContextInfo::New(usage);
}

// The status a page gets for a generation that didn't complete. Never
// kErrorRequestThrottled here, and kErrorInvalidRequest only where upstream
// answers it (a response constraint the model can't apply): Blink files a
// crash report for those.
ModelStreamingResponseStatus StatusFor(GenerationStatus status) {
  switch (status) {
    case GenerationStatus::kCancelled:
      return ModelStreamingResponseStatus::kErrorCancelled;
    case GenerationStatus::kConstraintUnsupported:
      // NotSupportedError, "The request is invalid - the input or options
      // could not be processed." (upstream's answer to kInvalidConstraint).
      return ModelStreamingResponseStatus::kErrorInvalidRequest;
    case GenerationStatus::kInvalidOutput:
      // UnknownError, "Failed to parse the response."
      return ModelStreamingResponseStatus::kErrorResponseParsingFailed;
    case GenerationStatus::kOutputTruncated:
      // QuotaExceededError, "The response exceeded output limits and was
      // truncated."
      return ModelStreamingResponseStatus::kErrorResponseExceedsMaxTokens;
    case GenerationStatus::kOk:
    case GenerationStatus::kThrottled:
    case GenerationStatus::kFailed:
      break;
  }
  return ModelStreamingResponseStatus::kErrorGenericFailure;
}

// A constrained reply goes to the page only if it is complete JSON. The
// runtime checked it too; this is the page's boundary. base::JSONReader is
// memory-safe (Rust), so parsing what the model produced here is fine.
bool IsCompleteJson(const std::string& reply) {
  return base::JSONReader::Read(reply, base::JSON_PARSE_RFC).has_value();
}

// One prompt's text (not sanitized yet) and images.
struct PromptParts {
  std::string text;
  std::vector<const SkBitmap*> bitmaps;
};

// nullopt if |prompt| holds something the session can't take: audio, a
// tool call or result, an assistant prefix, or an image unless
// |images_allowed| and the turn is the user's (images reach the model with
// user turns only).
std::optional<PromptParts> SplitPrompt(
    const blink::mojom::AILanguageModelPromptPtr& prompt,
    bool images_allowed) {
  if (!prompt || prompt->is_prefix) {
    return std::nullopt;
  }
  PromptParts parts;
  for (const auto& content : prompt->content) {
    if (!content) {
      return std::nullopt;
    }
    if (content->is_bitmap()) {
      if (!images_allowed ||
          prompt->role != blink::mojom::AILanguageModelPromptRole::kUser) {
        return std::nullopt;
      }
      parts.bitmaps.push_back(&content->get_bitmap());
      continue;
    }
    if (!content->is_text()) {
      return std::nullopt;
    }
    if (!parts.text.empty()) {
      parts.text += "\n";
    }
    parts.text += content->get_text();
  }
  return parts;
}

size_t PromptBytes(
    const std::vector<blink::mojom::AILanguageModelPromptPtr>& prompts) {
  size_t bytes = 0;
  for (const auto& prompt : prompts) {
    if (!prompt) {
      continue;
    }
    for (const auto& content : prompt->content) {
      if (content && content->is_text()) {
        bytes += content->get_text().size();
      }
    }
  }
  return bytes;
}

std::string NewConversationKey() {
  // Process-unique, never sent to the page; only for KV-cache reuse in the
  // inference process.
  static uint64_t next = 1;
  return "web-ai-" + base::NumberToString(next++);
}

}  // namespace

bool IsUsableBitmapForModel(const SkBitmap& bitmap) {
  return IsUsableImageSource(bitmap.width(), bitmap.height()) &&
         !bitmap.drawsNothing();
}

// ---------------------------------------------------------------------------
// WebAISession
// ---------------------------------------------------------------------------

WebAISession::Request::Request() = default;
WebAISession::Request::Request(Request&&) = default;
WebAISession::Request& WebAISession::Request::operator=(Request&&) = default;
WebAISession::Request::~Request() = default;

WebAISession::WebAISession(MoltAIManager& manager, uint64_t id)
    : manager_(manager),
      id_(id),
      site_key_(manager.site_key()),
      stream_interval_(manager.stream_interval()) {}

WebAISession::~WebAISession() {
  closed_ = true;
  if (held_images_ > 0) {
    manager_->backend()->ReleaseImages(site_key_, held_images_);
    held_images_ = 0;
  }
  if (job_id_ != 0) {
    manager_->backend()->Cancel(job_id_);
    job_id_ = 0;
  }
  if (running_ && running_->responder) {
    running_->responder->OnError(
        ModelStreamingResponseStatus::kErrorSessionDestroyed, nullptr);
  }
  for (Request& request : queue_) {
    if (request.responder) {
      request.responder->OnError(
          ModelStreamingResponseStatus::kErrorSessionDestroyed, nullptr);
    }
  }
  manager_->backend()->RemoveSession(site_key_);
}

void WebAISession::Enqueue(Request request, std::vector<SkBitmap> bitmaps) {
  if (closed_ || !request.responder) {
    return;
  }
  if (queue_.size() >= kMaxQueuedPerSession) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorGenericFailure, nullptr);
    return;
  }
  if (bitmaps.size() != request.image_turns.size()) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
    return;
  }
  if (!bitmaps.empty()) {
    // Over the per-site or total image caps: a generic failure (never
    // kErrorRequestThrottled, which crash-reports in the renderer).
    if (!HoldImages(bitmaps.size())) {
      request.responder->OnError(
          ModelStreamingResponseStatus::kErrorGenericFailure, nullptr);
      return;
    }
    request.held_images = bitmaps.size();
    request.images_pending = true;
  }
  request.id = next_request_id_++;
  const uint64_t request_id = request.id;
  // The page aborting the request (AbortSignal) closes this pipe.
  request.responder.set_disconnect_handler(
      base::BindOnce(&WebAISession::OnResponderDisconnected,
                     weak_factory_.GetWeakPtr(), request_id));
  queue_.push_back(std::move(request));
  if (!bitmaps.empty()) {
    // Scaled on a worker while earlier requests run; the page's bitmaps
    // aren't kept once that is done.
    manager_->backend()->PrepareImages(
        std::move(bitmaps),
        base::BindOnce(&WebAISession::OnImagesReady,
                       weak_factory_.GetWeakPtr(), request_id));
  }
  MaybeStartNext();
}

size_t WebAISession::ContextImages() const {
  return 0;
}

bool WebAISession::HoldImages(size_t count) {
  if (count == 0) {
    return true;
  }
  if (!manager_->backend()->TryHoldImages(site_key_, count)) {
    return false;
  }
  held_images_ += count;
  return true;
}

void WebAISession::SyncHeldImages() {
  size_t wanted = ContextImages();
  if (running_) {
    wanted += running_->held_images;
  }
  for (const Request& request : queue_) {
    wanted += request.held_images;
  }
  if (held_images_ > wanted) {
    manager_->backend()->ReleaseImages(site_key_, held_images_ - wanted);
    held_images_ = wanted;
  }
}

void WebAISession::OnImagesReady(
    uint64_t request_id,
    std::optional<std::vector<ModelImage>> images) {
  if (closed_) {
    return;
  }
  auto it = queue_.begin();
  while (it != queue_.end() && it->id != request_id) {
    ++it;
  }
  if (it == queue_.end() || !it->images_pending) {
    // Aborted while the images were being made.
    return;
  }
  bool ok = images.has_value() && images->size() == it->image_turns.size();
  for (size_t i = 0; ok && i < images->size(); ++i) {
    ok = it->image_turns[i] < it->turns.size() &&
         it->turns[it->image_turns[i]].role == Role::kUser &&
         (*images)[i].IsValid();
  }
  if (!ok) {
    // An image that can't be shown to the model ("the input could not be
    // processed", as for content a session can't take).
    if (it->responder) {
      it->responder->OnError(
          ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
    }
    queue_.erase(it);
    SyncHeldImages();
    MaybeStartNext();
    return;
  }
  for (size_t i = 0; i < images->size(); ++i) {
    it->turns[it->image_turns[i]].images.push_back(std::move((*images)[i]));
  }
  it->images_pending = false;
  MaybeStartNext();
}

GenerationJob WebAISession::NewJob() {
  GenerationJob job;
  job.site_key = site_key_;
  job.foreground = manager_->IsForeground();
  return job;
}

void WebAISession::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  manager_->CloseSession(id_);
}

void WebAISession::MaybeStartNext() {
  // A request whose images aren't ready yet waits at the head of the queue
  // (so the requests behind it wait too: they run in order).
  while (!closed_ && !running_ && !queue_.empty() &&
         !queue_.front().images_pending) {
    running_.emplace(std::move(queue_.front()));
    queue_.pop_front();
    std::optional<GenerationJob> job = Begin(*running_);
    if (!job) {
      // Answered without the model.
      running_.reset();
      continue;
    }
    StartJob(std::move(*job));
  }
  if (!closed_) {
    SyncHeldImages();
  }
}

void WebAISession::StartJob(GenerationJob job) {
  reply_.clear();
  unsent_.clear();
  last_flush_ = base::TimeTicks::Now();
  const uint64_t request_id = running_->id;
  job_id_ = manager_->backend()->Generate(
      std::move(job),
      base::BindRepeating(&WebAISession::OnChunk, weak_factory_.GetWeakPtr(),
                          request_id),
      base::BindOnce(&WebAISession::OnDone, weak_factory_.GetWeakPtr(),
                     request_id));
}

std::optional<std::string> WebAISession::ConstrainedText(
    Request& request,
    const std::string& reply) {
  return reply;
}

std::optional<GenerationJob> WebAISession::NextJob(Request& request) {
  return std::nullopt;
}

void WebAISession::OnChunk(uint64_t request_id, const std::string& text) {
  if (!running_ || running_->id != request_id) {
    return;
  }
  reply_ += text;
  unsent_ += text;
  // A constrained reply is held back until it is complete and checked.
  if (!running_->constrained &&
      base::TimeTicks::Now() - last_flush_ >= stream_interval_) {
    Flush();
  }
}

void WebAISession::Flush() {
  last_flush_ = base::TimeTicks::Now();
  if (unsent_.empty() || !running_ || !running_->responder) {
    return;
  }
  running_->responder->OnStreaming(unsent_);
  unsent_.clear();
}

void WebAISession::OnDone(uint64_t request_id, GenerationStatus status) {
  if (!running_ || running_->id != request_id) {
    return;
  }
  job_id_ = 0;
  if (status == GenerationStatus::kOk && running_->constrained) {
    // Held back until now: the page gets it only once it is complete JSON.
    std::optional<std::string> text;
    if (IsCompleteJson(reply_)) {
      text = ConstrainedText(*running_, reply_);
    }
    if (text) {
      unsent_ = std::move(*text);
    } else {
      status = GenerationStatus::kInvalidOutput;
    }
  }
  // A closing session (destroy(), its pipe gone; Blink has already rejected
  // the page's calls) starts no further generation.
  if (status == GenerationStatus::kOk && !closed_) {
    if (std::optional<GenerationJob> next = NextJob(*running_)) {
      // One part of the request is done: its text goes out now, and the
      // request keeps running with the next generation.
      Flush();
      StartJob(std::move(*next));
      return;
    }
  }
  Request request = std::move(*running_);
  running_.reset();
  if (status == GenerationStatus::kOk) {
    if (!unsent_.empty() && request.responder) {
      request.responder->OnStreaming(unsent_);
    }
    unsent_.clear();
    blink::mojom::ModelExecutionContextInfoPtr info =
        Commit(request, std::move(reply_));
    if (request.responder) {
      request.responder->OnCompletion(std::move(info));
    }
  } else if (request.responder) {
    request.responder->OnError(StatusFor(status), nullptr);
  }
  reply_.clear();
  unsent_.clear();
  MaybeStartNext();
}

void WebAISession::OnResponderDisconnected(uint64_t request_id) {
  if (running_ && running_->id == request_id) {
    // Aborted while running: stop the generation; the context is unchanged.
    if (job_id_ != 0) {
      manager_->backend()->Cancel(job_id_);
      job_id_ = 0;
    }
    running_.reset();
    reply_.clear();
    unsent_.clear();
    MaybeStartNext();
    return;
  }
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->id == request_id) {
      queue_.erase(it);
      // It may have been the one holding up the queue (its images not
      // ready yet).
      MaybeStartNext();
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// LanguageModelSession
// ---------------------------------------------------------------------------

LanguageModelSession::Config::Config() = default;
LanguageModelSession::Config::Config(const Config&) = default;
LanguageModelSession::Config& LanguageModelSession::Config::operator=(
    const Config&) = default;
LanguageModelSession::Config::~Config() = default;

LanguageModelSession::LanguageModelSession(
    MoltAIManager& manager,
    uint64_t id,
    Config config,
    Conversation conversation,
    mojo::PendingReceiver<blink::mojom::AILanguageModel> receiver)
    : WebAISession(manager, id),
      config_(std::move(config)),
      conversation_(std::move(conversation)),
      conversation_key_(NewConversationKey()),
      receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&LanguageModelSession::Close, base::Unretained(this)));
}

LanguageModelSession::~LanguageModelSession() = default;

blink::mojom::AILanguageModelInstanceInfoPtr LanguageModelSession::GetInfo()
    const {
  auto info = blink::mojom::AILanguageModelInstanceInfo::New();
  info->input_quota = conversation_.input_quota();
  info->input_usage = conversation_.input_usage();
  info->sampling_params = blink::mojom::AILanguageModelSamplingParams::New(
      config_.sampling.top_k, config_.sampling.temperature);
  info->input_types = std::vector<blink::mojom::AILanguageModelPromptType>{
      blink::mojom::AILanguageModelPromptType::kText};
  if (config_.image_input) {
    info->input_types->push_back(
        blink::mojom::AILanguageModelPromptType::kImage);
  }
  info->sampling_mode = config_.sampling_mode;
  return info;
}

void LanguageModelSession::Prompt(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    on_device_model::mojom::ResponseConstraintPtr constraint,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  // A JSON schema is enforced: the inference service turns it into a
  // grammar for its sampler (Blink has also put it in the prompt, unless
  // the page set omitResponseConstraintInput). A schema outside the
  // supported subset fails the request (kErrorInvalidRequest, as upstream
  // answers a constraint its model can't apply). A regular expression isn't
  // supported: Blink sends only its source, without the flags, so it can't
  // be applied faithfully.
  std::string json_schema;
  if (constraint) {
    if (!constraint->is_json_schema() ||
        constraint->get_json_schema().empty() ||
        constraint->get_json_schema().size() > kMaxResponseConstraintBytes) {
      mojo::Remote<blink::mojom::ModelStreamingResponder> responder(
          std::move(pending_responder));
      responder->OnError(ModelStreamingResponseStatus::kErrorInvalidRequest,
                         nullptr);
      return;
    }
    json_schema = constraint->get_json_schema();
  }
  Queue(std::move(prompts), /*append_only=*/false, std::move(json_schema),
        std::move(pending_responder));
}

void LanguageModelSession::Append(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Queue(std::move(prompts), /*append_only=*/true, std::string(),
        std::move(pending_responder));
}

void LanguageModelSession::Queue(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    bool append_only,
    std::string json_schema,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Request request;
  request.responder.Bind(std::move(pending_responder));
  request.append_only = append_only;
  request.constrained = !json_schema.empty();
  request.json_schema = std::move(json_schema);

  const uint32_t quota = conversation_.input_quota();
  const size_t bytes = PromptBytes(prompts);
  if (ExceedsQuotaBySize(bytes, quota)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(TokensForSize(bytes), quota));
    return;
  }
  std::vector<SkBitmap> bitmaps;
  for (const auto& prompt : prompts) {
    std::optional<PromptParts> parts =
        SplitPrompt(prompt, config_.image_input);
    // Only the first message of a session may be a system message, and only
    // when the session is created (Blink enforces both).
    if (!parts ||
        prompt->role == blink::mojom::AILanguageModelPromptRole::kSystem) {
      request.responder->OnError(
          ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
      return;
    }
    // At most what one request shows the model, each one a size that can
    // be scaled; checked before any work is done on them.
    for (const SkBitmap* bitmap : parts->bitmaps) {
      if (bitmaps.size() >= kMaxContextImages ||
          !IsUsableBitmapForModel(*bitmap)) {
        request.responder->OnError(
            ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
        return;
      }
      request.image_turns.push_back(request.turns.size());
      bitmaps.push_back(*bitmap);
    }
    request.turns.push_back(MakeTurn(
        prompt->role == blink::mojom::AILanguageModelPromptRole::kAssistant
            ? Role::kAssistant
            : Role::kUser,
        molt_ai::ToDisplayText(parts->text), parts->bitmaps.size()));
  }
  // A prompt must end with the user's turn for the model to answer.
  if (!append_only &&
      (request.turns.empty() || request.turns.back().role != Role::kUser)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
    return;
  }
  Enqueue(std::move(request), std::move(bitmaps));
}

std::optional<GenerationJob> LanguageModelSession::Begin(Request& request) {
  request.plan = conversation_.PlanAdd(request.turns);
  if (!request.plan.fits) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(request.plan.requested,
                                          conversation_.input_quota()));
    return std::nullopt;
  }
  if (request.append_only) {
    const bool overflow = request.plan.evict > 0;
    conversation_.Add(request.plan, std::move(request.turns));
    if (overflow) {
      request.responder->OnContextOverflow();
    }
    request.responder->OnCompletion(
        ContextInfo(conversation_.input_usage()));
    return std::nullopt;
  }
  Conversation::Shape shape =
      conversation_.ShapeAfter(request.plan, request.turns);
  GenerationJob job = NewJob();
  job.system_prompt = config_.system_prompt;
  if (!conversation_.system_prompt().empty()) {
    job.context_preamble = kPageInstructionsPreamble;
    job.context = conversation_.system_prompt();
  }
  job.history = std::move(shape.history);
  job.history_images = std::move(shape.history_images);
  job.user_message = std::move(shape.user_message);
  job.user_images = std::move(shape.user_images);
  job.max_output_tokens = config_.max_reply_tokens;
  job.sampling = config_.sampling;
  job.conversation_key = conversation_key_;
  job.json_schema = std::move(request.json_schema);
  return job;
}

blink::mojom::ModelExecutionContextInfoPtr LanguageModelSession::Commit(
    Request& request,
    std::string reply) {
  bool overflow = request.plan.evict > 0;
  conversation_.Add(request.plan, std::move(request.turns));
  overflow |= conversation_.AddReply(std::move(reply));
  if (overflow && request.responder) {
    request.responder->OnContextOverflow();
  }
  return ContextInfo(conversation_.input_usage());
}

size_t LanguageModelSession::ContextImages() const {
  return conversation_.image_count();
}

bool LanguageModelSession::HoldContextImages() {
  return HoldImages(conversation_.image_count());
}

void LanguageModelSession::Fork(
    mojo::PendingRemote<blink::mojom::AIManagerCreateLanguageModelClient>
        client) {
  manager().ForkLanguageModel(*this, std::move(client));
}

void LanguageModelSession::Destroy() {
  receiver_.reset();
  Close();
}

void LanguageModelSession::MeasureInputUsage(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    MeasureInputUsageCallback callback) {
  // Huge input: a lower bound without reading it all.
  constexpr uint32_t kReadLimitTokens = 1 << 20;
  const size_t bytes = PromptBytes(prompts);
  if (ExceedsQuotaBySize(bytes, kReadLimitTokens)) {
    std::move(callback).Run(TokensForSize(bytes));
    return;
  }
  uint32_t tokens = 0;
  for (const auto& prompt : prompts) {
    std::optional<PromptParts> parts =
        SplitPrompt(prompt, config_.image_input);
    if (!parts) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    tokens = AddTokens(tokens, EstimateMessageTokens(parts->text));
    for (size_t i = 0; i < parts->bitmaps.size(); ++i) {
      tokens = AddTokens(tokens, kImageInputTokens);
    }
  }
  std::move(callback).Run(tokens);
}

// ---------------------------------------------------------------------------
// WritingSession
// ---------------------------------------------------------------------------

WritingSession::WritingSession(MoltAIManager& manager,
                               uint64_t id,
                               std::string shared_context)
    : WebAISession(manager, id), shared_context_(std::move(shared_context)) {}

WritingSession::~WritingSession() = default;

void WritingSession::Execute(
    const std::string& input,
    const std::string& context,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Request request;
  request.responder.Bind(std::move(pending_responder));
  const uint32_t quota = kWritingAssistanceInputQuota;
  const size_t bytes = shared_context_.size() + context.size() + input.size();
  if (ExceedsQuotaBySize(bytes, quota)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(TokensForSize(bytes), quota));
    return;
  }
  const uint32_t requested =
      AddTokens(AddTokens(EstimateTokens(shared_context_),
                          EstimateTokens(context)),
                EstimateTokens(input));
  if (requested > quota) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(requested, quota));
    return;
  }
  request.task = BuildPrompt(shared_context_, molt_ai::ToDisplayText(context),
                             molt_ai::ToDisplayText(input));
  Enqueue(std::move(request));
}

void WritingSession::Measure(
    const std::string& input,
    const std::string& context,
    base::OnceCallback<void(std::optional<uint32_t>)> callback) {
  std::move(callback).Run(
      AddTokens(EstimateTokens(input), EstimateTokens(context)));
}

std::optional<GenerationJob> WritingSession::Begin(Request& request) {
  GenerationJob job = NewJob();
  job.system_prompt = std::move(request.task.system_prompt);
  job.context_preamble = std::move(request.task.context_preamble);
  job.context = std::move(request.task.context);
  job.user_message = std::move(request.task.user_message);
  job.max_output_tokens = request.task.max_output_tokens;
  job.sampling.temperature = request.task.temperature;
  job.sampling.top_k = request.task.top_k;
  return job;
}

blink::mojom::ModelExecutionContextInfoPtr WritingSession::Commit(
    Request& request,
    std::string reply) {
  return nullptr;
}

// ---------------------------------------------------------------------------
// Summarizer, Writer, Rewriter
// ---------------------------------------------------------------------------

SummarizerSession::SummarizerSession(
    MoltAIManager& manager,
    uint64_t id,
    std::string shared_context,
    SummarizerOptions options,
    mojo::PendingReceiver<blink::mojom::AISummarizer> receiver)
    : WritingSession(manager, id, std::move(shared_context)),
      options_(std::move(options)),
      receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&SummarizerSession::Close, base::Unretained(this)));
}

SummarizerSession::~SummarizerSession() = default;

void SummarizerSession::Summarize(
    const std::string& input,
    const std::string& context,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Execute(input, context, std::move(pending_responder));
}

void SummarizerSession::MeasureUsage(const std::string& input,
                                     const std::string& context,
                                     MeasureUsageCallback callback) {
  Measure(input, context, std::move(callback));
}

TaskPrompt SummarizerSession::BuildPrompt(std::string_view shared_context,
                                          std::string_view context,
                                          std::string_view input) const {
  return BuildSummarizerPrompt(options_, shared_context, context, input);
}

WriterSession::WriterSession(
    MoltAIManager& manager,
    uint64_t id,
    std::string shared_context,
    WriterOptions options,
    mojo::PendingReceiver<blink::mojom::AIWriter> receiver)
    : WritingSession(manager, id, std::move(shared_context)),
      options_(std::move(options)),
      receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&WriterSession::Close, base::Unretained(this)));
}

WriterSession::~WriterSession() = default;

void WriterSession::Write(
    const std::string& input,
    const std::optional<std::string>& context,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Execute(input, context.value_or(std::string()),
          std::move(pending_responder));
}

void WriterSession::MeasureUsage(const std::string& input,
                                 const std::string& context,
                                 MeasureUsageCallback callback) {
  Measure(input, context, std::move(callback));
}

TaskPrompt WriterSession::BuildPrompt(std::string_view shared_context,
                                      std::string_view context,
                                      std::string_view input) const {
  return BuildWriterPrompt(options_, shared_context, context, input);
}

RewriterSession::RewriterSession(
    MoltAIManager& manager,
    uint64_t id,
    std::string shared_context,
    RewriterOptions options,
    mojo::PendingReceiver<blink::mojom::AIRewriter> receiver)
    : WritingSession(manager, id, std::move(shared_context)),
      options_(std::move(options)),
      receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&RewriterSession::Close, base::Unretained(this)));
}

RewriterSession::~RewriterSession() = default;

void RewriterSession::Rewrite(
    const std::string& input,
    const std::optional<std::string>& context,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Execute(input, context.value_or(std::string()),
          std::move(pending_responder));
}

void RewriterSession::MeasureUsage(const std::string& input,
                                   const std::string& context,
                                   MeasureUsageCallback callback) {
  Measure(input, context, std::move(callback));
}

TaskPrompt RewriterSession::BuildPrompt(std::string_view shared_context,
                                        std::string_view context,
                                        std::string_view input) const {
  return BuildRewriterPrompt(options_, shared_context, context, input);
}

}  // namespace molt_ai::web_ai
