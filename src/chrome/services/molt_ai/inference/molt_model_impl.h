// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_MODEL_IMPL_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_MODEL_IMPL_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"
#include "chrome/services/molt_ai/inference/llama_engine.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace molt_ai {

// mojom::Model, on the service's main sequence. The LlamaEngine lives on its
// own ThreadPool sequence (base::SequenceBound), so generation never blocks
// the thread that receives mojo messages and a closed streamer is seen
// while a request runs, not only between requests.
//
// Requests are served one at a time, in arrival order (one llama_context;
// chat, agent and automation share it). A request's GenerationStreamer
// closing sets its cancel flag: a queued request is dropped, a running one
// stops at the next prompt chunk or token. Chunks and progress are posted
// back here and forwarded in order; OnComplete is sent last, then the
// streamer is closed.
//
// An output constraint (a JSON Schema) is turned into a grammar when the
// request arrives (engine/json_schema_grammar.h, bounded and linear); one
// outside the supported subset is answered at once with
// kConstraintUnsupported and never queued.
//
// Images (section 10 of the design) are checked when the request arrives:
// outside engine/image_input.h's bounds, more than kMaxImagesPerRequest, or
// on an assistant turn is a bad message; with no vision projector loaded
// the request is answered at once with kImagesUnsupported.
class MoltModelImpl : public mojom::Model {
 public:
  // |on_disconnect| runs when the browser closes the Model pipe. |vision|:
  // the engine has a vision projector.
  MoltModelImpl(base::SequenceBound<LlamaEngine> engine,
                bool vision,
                mojo::PendingReceiver<mojom::Model> receiver,
                base::OnceClosure on_disconnect);
  ~MoltModelImpl() override;

  MoltModelImpl(const MoltModelImpl&) = delete;
  MoltModelImpl& operator=(const MoltModelImpl&) = delete;

  // mojom::Model:
  void StreamChat(mojom::ChatRequestPtr request,
                  mojom::GenerationParamsPtr params,
                  mojo::PendingRemote<mojom::GenerationStreamer> streamer)
      override;
  void StreamPrompt(mojom::RawPromptRequestPtr request,
                    mojom::GenerationParamsPtr params,
                    mojo::PendingRemote<mojom::GenerationStreamer> streamer)
      override;
  void Tokenize(const std::string& text, TokenizeCallback callback) override;
  void CountTokens(const std::string& text,
                   CountTokensCallback callback) override;
  void ForgetConversation(const std::string& conversation_key) override;

 private:
  class CancelFlag;
  struct Request;

  void Enqueue(std::unique_ptr<Request> request);
  void MaybeStartNext();
  void OnChunk(uint64_t id, std::string text);
  void OnProgress(uint64_t id, uint32_t done_tokens, uint32_t total_tokens);
  void OnGenerationDone(uint64_t id, EngineSummary summary);
  void OnStreamerDisconnected(uint64_t id);
  void OnTokenized(TokenizeCallback callback,
                   std::optional<std::vector<int32_t>> tokens);

  SEQUENCE_CHECKER(sequence_checker_);

  base::SequenceBound<LlamaEngine> engine_;
  const bool vision_;
  mojo::Receiver<mojom::Model> receiver_;
  base::circular_deque<std::unique_ptr<Request>> queue_;
  std::unique_ptr<Request> running_;
  uint64_t next_id_ = 1;

  base::WeakPtrFactory<MoltModelImpl> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_MODEL_IMPL_H_
