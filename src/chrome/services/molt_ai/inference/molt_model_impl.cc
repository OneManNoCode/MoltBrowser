// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/molt_model_impl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai {

namespace {

// A runaway browser could otherwise queue without bound; it never has more
// than a handful of requests in flight (one per blocked caller).
constexpr size_t kMaxQueuedRequests = 64;

bool TextFits(const std::string& text) {
  return text.size() <= mojom::kMaxRequestTextBytes;
}

// Caps from molt_inference.mojom. Returns the reason for a bad message, or
// nullptr.
const char* CheckParams(const mojom::GenerationParams& p) {
  if (p.conversation_key.size() > mojom::kMaxConversationKeyBytes) {
    return "conversation_key too long";
  }
  if (p.stop_sequences.size() > mojom::kMaxStopSequences) {
    return "too many stop sequences";
  }
  for (const std::string& stop : p.stop_sequences) {
    if (stop.empty() || stop.size() > mojom::kMaxStopSequenceBytes ||
        !IsValidUtf8(stop)) {
      return "bad stop sequence";
    }
  }
  return nullptr;
}

EngineGenerationParams ToEngine(const mojom::GenerationParams& p) {
  EngineGenerationParams out;
  out.max_visible_tokens = p.max_visible_tokens;
  out.max_reasoning_tokens = p.max_reasoning_tokens;
  out.reasoning_on = p.reasoning == mojom::ReasoningMode::kOn;
  // NaN and out-of-range values would only make sampling misbehave; keep
  // them in the ranges the samplers expect.
  out.temperature = std::isfinite(p.temperature)
                        ? std::clamp(p.temperature, 0.0f, 5.0f)
                        : 0.7f;
  out.top_p = std::isfinite(p.top_p) ? std::clamp(p.top_p, 0.0f, 1.0f) : 0.9f;
  out.top_k = p.top_k;
  out.conversation_key = p.conversation_key;
  out.stop_sequences = p.stop_sequences;
  return out;
}

mojom::GenerationStatus ToMojo(EngineStatus status) {
  switch (status) {
    case EngineStatus::kEndOfGeneration:
      return mojom::GenerationStatus::kEndOfGeneration;
    case EngineStatus::kMaxTokens:
      return mojom::GenerationStatus::kMaxTokens;
    case EngineStatus::kStopSequence:
      return mojom::GenerationStatus::kStopSequence;
    case EngineStatus::kContextFull:
      return mojom::GenerationStatus::kContextFull;
    case EngineStatus::kCancelled:
      return mojom::GenerationStatus::kCancelled;
    case EngineStatus::kPromptTooLong:
      return mojom::GenerationStatus::kPromptTooLong;
    case EngineStatus::kSystemPromptTooLong:
      return mojom::GenerationStatus::kSystemPromptTooLong;
    case EngineStatus::kTokenizeFailed:
      return mojom::GenerationStatus::kTokenizeFailed;
    case EngineStatus::kDecodeFailed:
      return mojom::GenerationStatus::kDecodeFailed;
  }
  return mojom::GenerationStatus::kDecodeFailed;
}

}  // namespace

// Set on the main sequence when a request's streamer goes away, read on the
// engine sequence before every prompt chunk and token.
class MoltModelImpl::CancelFlag
    : public base::RefCountedThreadSafe<CancelFlag> {
 public:
  CancelFlag() = default;
  CancelFlag(const CancelFlag&) = delete;
  CancelFlag& operator=(const CancelFlag&) = delete;

  void Set() { cancelled_.store(true, std::memory_order_relaxed); }
  bool IsSet() const { return cancelled_.load(std::memory_order_relaxed); }

 private:
  friend class base::RefCountedThreadSafe<CancelFlag>;
  ~CancelFlag() = default;

  std::atomic<bool> cancelled_{false};
};

struct MoltModelImpl::Request {
  uint64_t id = 0;
  bool is_chat = true;
  EngineChatRequest chat;
  EngineRawRequest raw;
  EngineGenerationParams params;
  mojo::Remote<mojom::GenerationStreamer> streamer;
  scoped_refptr<CancelFlag> cancelled = base::MakeRefCounted<CancelFlag>();
};

MoltModelImpl::MoltModelImpl(base::SequenceBound<LlamaEngine> engine,
                             mojo::PendingReceiver<mojom::Model> receiver,
                             base::OnceClosure on_disconnect)
    : engine_(std::move(engine)), receiver_(this, std::move(receiver)) {
  receiver_.set_disconnect_handler(std::move(on_disconnect));
}

MoltModelImpl::~MoltModelImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Stop a running decode at the next token; |engine_| is destroyed on its
  // sequence after that task returns.
  if (running_) running_->cancelled->Set();
  for (const auto& request : queue_) request->cancelled->Set();
}

void MoltModelImpl::StreamChat(
    mojom::ChatRequestPtr request,
    mojom::GenerationParamsPtr params,
    mojo::PendingRemote<mojom::GenerationStreamer> streamer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const char* error = CheckParams(*params)) {
    receiver_.ReportBadMessage(error);
    return;
  }
  if (request->history.size() > mojom::kMaxHistoryTurns ||
      !TextFits(request->system_prompt) ||
      !TextFits(request->context_preamble) || !TextFits(request->context) ||
      !TextFits(request->user_message)) {
    receiver_.ReportBadMessage("chat request over the caps");
    return;
  }
  auto r = std::make_unique<Request>();
  r->is_chat = true;
  r->chat.system_prompt = std::move(request->system_prompt);
  r->chat.context_preamble = std::move(request->context_preamble);
  r->chat.context = std::move(request->context);
  r->chat.user_message = std::move(request->user_message);
  r->chat.history.reserve(request->history.size());
  for (mojom::ChatTurnPtr& turn : request->history) {
    if (!TextFits(turn->content)) {
      receiver_.ReportBadMessage("chat turn over the caps");
      return;
    }
    r->chat.history.push_back(
        {turn->role == mojom::ChatRole::kUser, std::move(turn->content)});
  }
  r->params = ToEngine(*params);
  r->streamer.Bind(std::move(streamer));
  Enqueue(std::move(r));
}

void MoltModelImpl::StreamPrompt(
    mojom::RawPromptRequestPtr request,
    mojom::GenerationParamsPtr params,
    mojo::PendingRemote<mojom::GenerationStreamer> streamer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (const char* error = CheckParams(*params)) {
    receiver_.ReportBadMessage(error);
    return;
  }
  if (!TextFits(request->system_prompt) || !TextFits(request->prompt)) {
    receiver_.ReportBadMessage("prompt over the caps");
    return;
  }
  auto r = std::make_unique<Request>();
  r->is_chat = false;
  r->raw.system_prompt = std::move(request->system_prompt);
  r->raw.prompt = std::move(request->prompt);
  r->params = ToEngine(*params);
  r->streamer.Bind(std::move(streamer));
  Enqueue(std::move(r));
}

void MoltModelImpl::Enqueue(std::unique_ptr<Request> request) {
  if (queue_.size() >= kMaxQueuedRequests) {
    receiver_.ReportBadMessage("too many queued requests");
    return;
  }
  request->id = next_id_++;
  request->streamer.set_disconnect_handler(
      base::BindOnce(&MoltModelImpl::OnStreamerDisconnected,
                     weak_factory_.GetWeakPtr(), request->id));
  queue_.push_back(std::move(request));
  MaybeStartNext();
}

void MoltModelImpl::MaybeStartNext() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  while (!running_ && !queue_.empty()) {
    std::unique_ptr<Request> next = std::move(queue_.front());
    queue_.pop_front();
    // The browser gave up on it while it waited: skip it.
    if (next->cancelled->IsSet() || !next->streamer.is_connected()) continue;
    running_ = std::move(next);
  }
  if (!running_) return;

  // Engine callbacks run on the engine sequence; chunks and progress hop
  // back here, in order, ahead of the Then() reply.
  scoped_refptr<base::SequencedTaskRunner> main =
      base::SequencedTaskRunner::GetCurrentDefault();
  base::WeakPtr<MoltModelImpl> weak = weak_factory_.GetWeakPtr();
  const uint64_t id = running_->id;
  EngineCallbacks callbacks;
  callbacks.is_cancelled = [flag = running_->cancelled]() {
    return flag->IsSet();
  };
  callbacks.on_progress = [main, weak, id](uint32_t done, uint32_t total) {
    main->PostTask(FROM_HERE, base::BindOnce(&MoltModelImpl::OnProgress, weak,
                                             id, done, total));
  };
  callbacks.on_chunk = [main, weak, id](std::string text) {
    main->PostTask(FROM_HERE, base::BindOnce(&MoltModelImpl::OnChunk, weak,
                                             id, std::move(text)));
  };

  if (running_->is_chat) {
    engine_.AsyncCall(&LlamaEngine::Chat)
        .WithArgs(std::move(running_->chat), running_->params,
                  std::move(callbacks))
        .Then(base::BindOnce(&MoltModelImpl::OnGenerationDone, weak, id));
  } else {
    engine_.AsyncCall(&LlamaEngine::Raw)
        .WithArgs(std::move(running_->raw), running_->params,
                  std::move(callbacks))
        .Then(base::BindOnce(&MoltModelImpl::OnGenerationDone, weak, id));
  }
}

void MoltModelImpl::OnChunk(uint64_t id, std::string text) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_ || running_->id != id || !running_->streamer.is_connected()) {
    return;
  }
  running_->streamer->OnChunk(text);
}

void MoltModelImpl::OnProgress(uint64_t id,
                               uint32_t done_tokens,
                               uint32_t total_tokens) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_ || running_->id != id || !running_->streamer.is_connected()) {
    return;
  }
  running_->streamer->OnProgress(done_tokens, total_tokens);
}

void MoltModelImpl::OnGenerationDone(uint64_t id, EngineSummary summary) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_ || running_->id != id) return;
  if (running_->streamer.is_connected()) {
    auto out = mojom::GenerationSummary::New();
    out->status = ToMojo(summary.status);
    out->prompt_tokens = summary.prompt_tokens;
    out->reused_tokens = summary.reused_tokens;
    out->generated_tokens = summary.generated_tokens;
    out->history_dropped = summary.history_dropped;
    out->context_trimmed = summary.context_trimmed;
    out->user_message_trimmed = summary.user_message_trimmed;
    running_->streamer->OnComplete(std::move(out));
  }
  running_.reset();  // closes the streamer pipe
  MaybeStartNext();
}

void MoltModelImpl::OnStreamerDisconnected(uint64_t id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (running_ && running_->id == id) {
    // OnGenerationDone() follows once the engine notices.
    running_->cancelled->Set();
    return;
  }
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if ((*it)->id == id) {
      queue_.erase(it);
      return;
    }
  }
}

void MoltModelImpl::Tokenize(const std::string& text,
                             TokenizeCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (text.size() > mojom::kMaxTokenizeTextBytes) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  engine_.AsyncCall(&LlamaEngine::Tokenize)
      .WithArgs(text, static_cast<size_t>(mojom::kMaxTokenizeTextBytes))
      .Then(base::BindOnce(&MoltModelImpl::OnTokenized,
                           weak_factory_.GetWeakPtr(), std::move(callback)));
}

void MoltModelImpl::OnTokenized(TokenizeCallback callback,
                                std::optional<std::vector<int32_t>> tokens) {
  std::move(callback).Run(tokens);
}

void MoltModelImpl::CountTokens(const std::string& text,
                                CountTokensCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Tokenize(text, base::BindOnce(
                     [](CountTokensCallback callback,
                        const std::optional<std::vector<int32_t>>& tokens) {
                       std::move(callback).Run(
                           tokens ? std::optional<uint32_t>(
                                        static_cast<uint32_t>(tokens->size()))
                                  : std::nullopt);
                     },
                     std::move(callback)));
}

void MoltModelImpl::ForgetConversation(const std::string& conversation_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (conversation_key.size() > mojom::kMaxConversationKeyBytes) {
    receiver_.ReportBadMessage("conversation_key too long");
    return;
  }
  engine_.AsyncCall(&LlamaEngine::ForgetConversation)
      .WithArgs(conversation_key);
}

}  // namespace molt_ai
