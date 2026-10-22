// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_sessions.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::web_ai {

namespace {

using blink::mojom::ModelStreamingResponseStatus;

blink::mojom::ModelExecutionContextInfoPtr ContextInfo(uint32_t usage) {
  return blink::mojom::ModelExecutionContextInfo::New(usage);
}

// The status a page gets for a generation that didn't complete. Never
// kErrorInvalidRequest / kErrorRequestThrottled here: Blink files a crash
// report for those.
ModelStreamingResponseStatus StatusFor(GenerationStatus status) {
  switch (status) {
    case GenerationStatus::kCancelled:
      return ModelStreamingResponseStatus::kErrorCancelled;
    case GenerationStatus::kOk:
    case GenerationStatus::kThrottled:
    case GenerationStatus::kFailed:
      break;
  }
  return ModelStreamingResponseStatus::kErrorGenericFailure;
}

// The text of one prompt, or nullopt if it holds something a text model
// can't take (an image, audio, a tool call or result) or is an assistant
// prefix. Not sanitized yet.
std::optional<std::string> PromptText(
    const blink::mojom::AILanguageModelPromptPtr& prompt) {
  if (!prompt || prompt->is_prefix) {
    return std::nullopt;
  }
  std::string text;
  for (const auto& content : prompt->content) {
    if (!content || !content->is_text()) {
      return std::nullopt;
    }
    if (!text.empty()) {
      text += "\n";
    }
    text += content->get_text();
  }
  return text;
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

void WebAISession::Enqueue(Request request) {
  if (closed_ || !request.responder) {
    return;
  }
  if (queue_.size() >= kMaxQueuedPerSession) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorGenericFailure, nullptr);
    return;
  }
  request.id = next_request_id_++;
  // The page aborting the request (AbortSignal) closes this pipe.
  request.responder.set_disconnect_handler(
      base::BindOnce(&WebAISession::OnResponderDisconnected,
                     weak_factory_.GetWeakPtr(), request.id));
  queue_.push_back(std::move(request));
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
  while (!closed_ && !running_ && !queue_.empty()) {
    running_.emplace(std::move(queue_.front()));
    queue_.pop_front();
    std::optional<GenerationJob> job = Begin(*running_);
    if (!job) {
      // Answered without the model.
      running_.reset();
      continue;
    }
    reply_.clear();
    unsent_.clear();
    last_flush_ = base::TimeTicks::Now();
    const uint64_t request_id = running_->id;
    job_id_ = manager_->backend()->Generate(
        std::move(*job),
        base::BindRepeating(&WebAISession::OnChunk, weak_factory_.GetWeakPtr(),
                            request_id),
        base::BindOnce(&WebAISession::OnDone, weak_factory_.GetWeakPtr(),
                       request_id));
  }
}

void WebAISession::OnChunk(uint64_t request_id, const std::string& text) {
  if (!running_ || running_->id != request_id) {
    return;
  }
  reply_ += text;
  unsent_ += text;
  if (base::TimeTicks::Now() - last_flush_ >= stream_interval_) {
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
  info->sampling_mode = config_.sampling_mode;
  return info;
}

void LanguageModelSession::Prompt(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    on_device_model::mojom::ResponseConstraintPtr constraint,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  // A JSON schema is already spelled out in the prompt by Blink; the model
  // isn't forced to follow it (no constrained decoding yet). A regex can't
  // be expressed at all.
  if (constraint && !constraint->is_json_schema()) {
    mojo::Remote<blink::mojom::ModelStreamingResponder> responder(
        std::move(pending_responder));
    responder->OnError(ModelStreamingResponseStatus::kErrorInvalidRequest,
                       nullptr);
    return;
  }
  Queue(std::move(prompts), /*append_only=*/false,
        std::move(pending_responder));
}

void LanguageModelSession::Append(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Queue(std::move(prompts), /*append_only=*/true,
        std::move(pending_responder));
}

void LanguageModelSession::Queue(
    std::vector<blink::mojom::AILanguageModelPromptPtr> prompts,
    bool append_only,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Request request;
  request.responder.Bind(std::move(pending_responder));
  request.append_only = append_only;

  const uint32_t quota = conversation_.input_quota();
  const size_t bytes = PromptBytes(prompts);
  if (ExceedsQuotaBySize(bytes, quota)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(TokensForSize(bytes), quota));
    return;
  }
  for (const auto& prompt : prompts) {
    std::optional<std::string> text = PromptText(prompt);
    // Only the first message of a session may be a system message, and only
    // when the session is created (Blink enforces both).
    if (!text ||
        prompt->role == blink::mojom::AILanguageModelPromptRole::kSystem) {
      request.responder->OnError(
          ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
      return;
    }
    request.turns.push_back(MakeTurn(
        prompt->role == blink::mojom::AILanguageModelPromptRole::kAssistant
            ? Role::kAssistant
            : Role::kUser,
        molt_ai::ToDisplayText(*text)));
  }
  // A prompt must end with the user's turn for the model to answer.
  if (!append_only &&
      (request.turns.empty() || request.turns.back().role != Role::kUser)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInvalidRequest, nullptr);
    return;
  }
  Enqueue(std::move(request));
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
  job.user_message = std::move(shape.user_message);
  job.max_output_tokens = config_.max_reply_tokens;
  job.sampling = config_.sampling;
  job.conversation_key = conversation_key_;
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
    std::optional<std::string> text = PromptText(prompt);
    if (!text) {
      std::move(callback).Run(std::nullopt);
      return;
    }
    tokens = AddTokens(tokens, EstimateMessageTokens(*text));
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
