// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_translator_session.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/web_ai/molt_ai_manager.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::web_ai {

namespace {

using blink::mojom::ModelStreamingResponseStatus;

// The translation in a reply decoded under kTranslatorResponseSchema. The
// browser parses what the model wrote with base::JSONReader (memory-safe),
// and trusts nothing about its shape.
std::optional<std::string> TranslationIn(const std::string& reply) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(reply, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  const std::string* translation =
      parsed->GetDict().FindString(kTranslatorResponseKey);
  if (!translation) {
    return std::nullopt;
  }
  return molt_ai::ToDisplayText(*translation);
}

}  // namespace

TranslatorSession::TranslatorSession(
    MoltAIManager& manager,
    uint64_t id,
    TranslatorPair pair,
    mojo::PendingReceiver<blink::mojom::Translator> receiver)
    : WebAISession(manager, id),
      pair_(pair),
      receiver_(this, std::move(receiver)) {
  // Translator.destroy() and garbage collection drop the pipe.
  receiver_.set_disconnect_handler(
      base::BindOnce(&TranslatorSession::Close, base::Unretained(this)));
}

TranslatorSession::~TranslatorSession() = default;

void TranslatorSession::Translate(
    const std::string& input,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Execute(input, std::move(pending_responder));
}

void TranslatorSession::TranslateStreaming(
    const std::string& input,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  // The same deltas: Blink joins them for translate() and streams them for
  // translateStreaming().
  Execute(input, std::move(pending_responder));
}

void TranslatorSession::Execute(
    const std::string& input,
    mojo::PendingRemote<blink::mojom::ModelStreamingResponder>
        pending_responder) {
  Request request;
  request.responder.Bind(std::move(pending_responder));
  // As upstream's Translator checks TranslatorAPIAllowed on every call: a
  // policy (or the On-device AI switch) turned off since create() stops
  // new translations.
  if (!manager().IsContextStillEligible(WebAIApi::kTranslator)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorGenericFailure, nullptr);
    return;
  }
  // Sized before anything is copied. Blink requires the quota info with
  // kErrorInputTooLarge (QuotaExceededError's requested and quota).
  const uint32_t quota = kTranslatorInputQuota;
  if (ExceedsQuotaBySize(input.size(), quota)) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(TokensForSize(input.size()), quota));
    return;
  }
  const uint32_t requested = EstimateTokens(input);
  if (requested > quota) {
    request.responder->OnError(
        ModelStreamingResponseStatus::kErrorInputTooLarge,
        blink::mojom::QuotaErrorInfo::New(requested, quota));
    return;
  }
  request.pieces = SplitForTranslation(molt_ai::ToDisplayText(input),
                                       kTranslatorPieceTokens);
  // Every reply is held back until it parsed (ConstrainedText()).
  request.constrained = true;
  Enqueue(std::move(request));
}

GenerationJob TranslatorSession::JobForNextPiece(Request& request) {
  TaskPrompt prompt =
      BuildTranslatorPrompt(pair_, request.pieces[request.next_piece].text);
  ++request.next_piece;
  GenerationJob job = NewJob();
  job.system_prompt = std::move(prompt.system_prompt);
  job.user_message = std::move(prompt.user_message);
  job.max_output_tokens = prompt.max_output_tokens;
  job.sampling.temperature = prompt.temperature;
  job.sampling.top_k = prompt.top_k;
  job.json_schema = kTranslatorResponseSchema;
  return job;
}

std::optional<GenerationJob> TranslatorSession::Begin(Request& request) {
  // Whitespace, or text with nothing to translate, before the first piece
  // the model translates.
  const std::string lead = TakeUntranslated(request.pieces, &request.next_piece);
  if (!lead.empty()) {
    request.responder->OnStreaming(lead);
  }
  if (request.next_piece >= request.pieces.size()) {
    // Nothing for the model (an empty input, digits, punctuation).
    request.responder->OnCompletion(nullptr);
    return std::nullopt;
  }
  return JobForNextPiece(request);
}

std::optional<std::string> TranslatorSession::ConstrainedText(
    Request& request,
    const std::string& reply) {
  if (request.next_piece == 0 || request.next_piece > request.pieces.size()) {
    return std::nullopt;
  }
  // The piece this reply translates.
  const TranslationPiece& piece = request.pieces[request.next_piece - 1];
  std::optional<std::string> translation = TranslationIn(reply);
  if (!translation) {
    return std::nullopt;
  }
  std::optional<std::string> text = CleanTranslation(piece.text, *translation);
  if (!text) {
    return std::nullopt;
  }
  // The whitespace (and untranslatable text) after the piece goes with it.
  *text += TakeUntranslated(request.pieces, &request.next_piece);
  return text;
}

std::optional<GenerationJob> TranslatorSession::NextJob(Request& request) {
  if (request.next_piece >= request.pieces.size()) {
    return std::nullopt;
  }
  return JobForNextPiece(request);
}

blink::mojom::ModelExecutionContextInfoPtr TranslatorSession::Commit(
    Request& request,
    std::string reply) {
  // Translations are stateless; there is no context to report.
  return nullptr;
}

}  // namespace molt_ai::web_ai
