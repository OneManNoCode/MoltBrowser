// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/molt_model_impl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/services/molt_ai/inference/engine/audio_input.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "chrome/services/molt_ai/inference/engine/json_schema_grammar.h"
#include "chrome/services/molt_ai/inference/engine/gpu_pipeline_cache.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace molt_ai {

namespace {

// A runaway browser could otherwise queue without bound; it never has more
// than a handful of requests in flight (one per blocked caller).
constexpr size_t kMaxQueuedRequests = 64;

// The mojom caps on images are the engine's (engine/image_input.h).
static_assert(mojom::kMaxImageSide == kMaxImageSide);
static_assert(mojom::kMinImageSide == kMinImageSide);
static_assert(mojom::kMaxImageAspectRatio == kMaxImageAspectRatio);
static_assert(mojom::kMaxImagesPerRequest == kMaxImagesPerRequest);
// And on audio (engine/audio_input.h).
static_assert(mojom::kAudioSampleRate == kAudioSampleRate);
static_assert(mojom::kMinAudioSamples == kMinAudioSamples);
static_assert(mojom::kMaxAudioSamples == kMaxAudioSamples);
static_assert(mojom::kMaxAudioPerRequest == kMaxAudioPerRequest);
// And the pipeline cache's (engine/gpu_pipeline_cache.h).
static_assert(mojom::kMaxPipelineCacheBytes == kMaxPipelineCacheBytes);

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
  if (p.constraint && p.constraint->is_json_schema() &&
      p.constraint->get_json_schema().size() >
          mojom::kMaxConstraintSchemaBytes) {
    return "constraint over the cap";
  }
  return nullptr;
}

// The grammar for |constraint| ("" for none), or nullopt if it is outside
// the supported subset. The schema is untrusted (a web page's
// responseConstraint): the converter is linear in its size, which
// CheckParams() capped, and bounds the grammar it produces, so this runs
// on the main sequence when the request arrives.
std::optional<std::string> GrammarFor(
    const mojom::OutputConstraintPtr& constraint) {
  if (!constraint) return std::string();
  if (!constraint->is_json_schema()) return std::nullopt;
  SchemaGrammar g = JsonSchemaToGrammar(constraint->get_json_schema());
  if (g.status != SchemaGrammarStatus::kOk) return std::nullopt;
  return std::move(g.gbnf);
}

// Answers a request that can't run (a refused constraint, images for a
// model without vision, audio for one without an audio encoder, a buffer
// the service couldn't map) without queueing it.
void AnswerAtOnce(
    mojo::PendingRemote<mojom::GenerationStreamer> pending_streamer,
    mojom::GenerationStatus status) {
  mojo::Remote<mojom::GenerationStreamer> streamer(
      std::move(pending_streamer));
  auto summary = mojom::GenerationSummary::New();
  summary->status = status;
  streamer->OnComplete(std::move(summary));
}

// The caps on a request's images (the bounds of each, how many, which
// turns). Returns the reason for a bad message, or nullptr.
const char* CheckImages(const mojom::ChatRequest& request, size_t* count) {
  size_t n = request.user_images.size();
  for (const mojom::ChatTurnPtr& turn : request.history) {
    if (!turn->images.empty() && turn->role != mojom::ChatRole::kUser) {
      return "image on an assistant turn";
    }
    n += turn->images.size();
  }
  if (n > mojom::kMaxImagesPerRequest) return "too many images";
  auto check = [](const mojom::ImageInputPtr& image) {
    // An unmappable buffer reads as empty; StreamChat answers it apart.
    return image->rgb.storage_type() ==
               mojo_base::BigBuffer::StorageType::kInvalidBuffer ||
           CheckModelImage(image->width, image->height, image->rgb.size()) ==
               ImageCheck::kOk;
  };
  for (const mojom::ImageInputPtr& image : request.user_images) {
    if (!check(image)) return "image outside the bounds";
  }
  for (const mojom::ChatTurnPtr& turn : request.history) {
    for (const mojom::ImageInputPtr& image : turn->images) {
      if (!check(image)) return "image outside the bounds";
    }
  }
  *count = n;
  return nullptr;
}

// The same for its audio clips (the length of each, how many, which
// turns).
const char* CheckAudio(const mojom::ChatRequest& request, size_t* count) {
  size_t n = request.user_audio.size();
  for (const mojom::ChatTurnPtr& turn : request.history) {
    if (!turn->audio.empty() && turn->role != mojom::ChatRole::kUser) {
      return "audio on an assistant turn";
    }
    n += turn->audio.size();
  }
  if (n > mojom::kMaxAudioPerRequest) return "too many audio clips";
  auto check = [](const mojom::AudioInputPtr& audio) {
    // An unmappable buffer reads as empty; StreamChat answers it apart.
    return audio->pcm.storage_type() ==
               mojo_base::BigBuffer::StorageType::kInvalidBuffer ||
           CheckModelAudio(audio->pcm.size()) == AudioCheck::kOk;
  };
  for (const mojom::AudioInputPtr& audio : request.user_audio) {
    if (!check(audio)) return "audio outside the bounds";
  }
  for (const mojom::ChatTurnPtr& turn : request.history) {
    for (const mojom::AudioInputPtr& audio : turn->audio) {
      if (!check(audio)) return "audio outside the bounds";
    }
  }
  *count = n;
  return nullptr;
}

bool AnyUnmappedAudio(const mojom::ChatRequest& request) {
  auto unmapped = [](const mojom::AudioInputPtr& audio) {
    return audio->pcm.storage_type() ==
           mojo_base::BigBuffer::StorageType::kInvalidBuffer;
  };
  for (const mojom::AudioInputPtr& audio : request.user_audio) {
    if (unmapped(audio)) return true;
  }
  for (const mojom::ChatTurnPtr& turn : request.history) {
    for (const mojom::AudioInputPtr& audio : turn->audio) {
      if (unmapped(audio)) return true;
    }
  }
  return false;
}

bool AnyUnmappedImage(const mojom::ChatRequest& request) {
  auto unmapped = [](const mojom::ImageInputPtr& image) {
    return image->rgb.storage_type() ==
           mojo_base::BigBuffer::StorageType::kInvalidBuffer;
  };
  for (const mojom::ImageInputPtr& image : request.user_images) {
    if (unmapped(image)) return true;
  }
  for (const mojom::ChatTurnPtr& turn : request.history) {
    for (const mojom::ImageInputPtr& image : turn->images) {
      if (unmapped(image)) return true;
    }
  }
  return false;
}

// The pixels, copied out of the message (checked by CheckImages()).
std::vector<EngineImage> ToEngineImages(
    const std::vector<mojom::ImageInputPtr>& images) {
  std::vector<EngineImage> out;
  out.reserve(images.size());
  for (const mojom::ImageInputPtr& image : images) {
    out.emplace_back(image->width, image->height,
                     std::vector<uint8_t>(image->rgb.begin(),
                                          image->rgb.end()));
  }
  return out;
}

// The samples, copied out of the message (checked by CheckAudio(); 16-bit
// in the host's byte order).
std::vector<EngineAudio> ToEngineAudio(
    const std::vector<mojom::AudioInputPtr>& audio) {
  std::vector<EngineAudio> out;
  out.reserve(audio.size());
  for (const mojom::AudioInputPtr& clip : audio) {
    std::vector<int16_t> samples(clip->pcm.size() / kAudioBytesPerSample);
    std::copy_n(clip->pcm.data(), samples.size() * kAudioBytesPerSample,
                reinterpret_cast<uint8_t*>(samples.data()));
    out.emplace_back(std::move(samples));
  }
  return out;
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
    case EngineStatus::kConstraintFailed:
      return mojom::GenerationStatus::kConstraintFailed;
    case EngineStatus::kImagesUnsupported:
      return mojom::GenerationStatus::kImagesUnsupported;
    case EngineStatus::kBackendFailed:
      return mojom::GenerationStatus::kBackendFailed;
    case EngineStatus::kAudioUnsupported:
      return mojom::GenerationStatus::kAudioUnsupported;
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
                             bool vision,
                             bool audio,
                             mojo::PendingReceiver<mojom::Model> receiver,
                             base::OnceClosure on_disconnect)
    : engine_(std::move(engine)),
      vision_(vision),
      audio_(audio),
      receiver_(this, std::move(receiver)) {
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
  size_t n_images = 0;
  if (const char* error = CheckImages(*request, &n_images)) {
    receiver_.ReportBadMessage(error);
    return;
  }
  if (n_images > 0 && !vision_) {
    AnswerAtOnce(std::move(streamer),
                 mojom::GenerationStatus::kImagesUnsupported);
    return;
  }
  size_t n_audio = 0;
  if (const char* error = CheckAudio(*request, &n_audio)) {
    receiver_.ReportBadMessage(error);
    return;
  }
  if (n_audio > 0 && !audio_) {
    AnswerAtOnce(std::move(streamer),
                 mojom::GenerationStatus::kAudioUnsupported);
    return;
  }
  if ((n_images > 0 && AnyUnmappedImage(*request)) ||
      (n_audio > 0 && AnyUnmappedAudio(*request))) {
    // The shared memory couldn't be mapped here (out of address space).
    AnswerAtOnce(std::move(streamer), mojom::GenerationStatus::kDecodeFailed);
    return;
  }
  auto r = std::make_unique<Request>();
  r->is_chat = true;
  r->chat.system_prompt = std::move(request->system_prompt);
  r->chat.context_preamble = std::move(request->context_preamble);
  r->chat.context = std::move(request->context);
  r->chat.user_message = std::move(request->user_message);
  r->chat.user_images = ToEngineImages(request->user_images);
  r->chat.user_audio = ToEngineAudio(request->user_audio);
  r->chat.history.reserve(request->history.size());
  for (mojom::ChatTurnPtr& turn : request->history) {
    if (!TextFits(turn->content)) {
      receiver_.ReportBadMessage("chat turn over the caps");
      return;
    }
    EngineChatTurn engine_turn(turn->role == mojom::ChatRole::kUser,
                               std::move(turn->content));
    engine_turn.images = ToEngineImages(turn->images);
    engine_turn.audio = ToEngineAudio(turn->audio);
    r->chat.history.push_back(std::move(engine_turn));
  }
  std::optional<std::string> grammar = GrammarFor(params->constraint);
  if (!grammar) {
    AnswerAtOnce(std::move(streamer),
                 mojom::GenerationStatus::kConstraintUnsupported);
    return;
  }
  r->params = ToEngine(*params);
  r->params.grammar = std::move(*grammar);
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
  std::optional<std::string> grammar = GrammarFor(params->constraint);
  if (!grammar) {
    AnswerAtOnce(std::move(streamer),
                 mojom::GenerationStatus::kConstraintUnsupported);
    return;
  }
  auto r = std::make_unique<Request>();
  r->is_chat = false;
  r->raw.system_prompt = std::move(request->system_prompt);
  r->raw.prompt = std::move(request->prompt);
  r->params = ToEngine(*params);
  r->params.grammar = std::move(*grammar);
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

void MoltModelImpl::GetPipelineCache(GetPipelineCacheCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // On the engine's sequence, so between generations.
  engine_.AsyncCall(&LlamaEngine::PipelineCacheUpdate)
      .WithArgs(static_cast<size_t>(mojom::kMaxPipelineCacheBytes))
      .Then(base::BindOnce(&MoltModelImpl::OnPipelineCache,
                           weak_factory_.GetWeakPtr(), std::move(callback)));
}

void MoltModelImpl::OnPipelineCache(GetPipelineCacheCallback callback,
                                    EnginePipelineCache update) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  switch (update.state) {
    case EnginePipelineCache::State::kNone:
      std::move(callback).Run(std::nullopt);
      return;
    case EnginePipelineCache::State::kTooLarge:
      // Empty: the browser drops the copy it has, so the next load starts
      // a cache with only what is used.
      std::move(callback).Run(mojo_base::BigBuffer());
      return;
    case EnginePipelineCache::State::kData:
      std::move(callback).Run(mojo_base::BigBuffer(update.data));
      return;
  }
}

}  // namespace molt_ai
