// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_EMBEDDING_MODEL_IMPL_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_EMBEDDING_MODEL_IMPL_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"
#include "chrome/services/molt_ai/inference/llama_embedding_engine.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace molt_ai {

// mojom::EmbeddingModel, on the service's main sequence. The
// LlamaEmbeddingEngine lives on its own ThreadPool sequence
// (base::SequenceBound), which runs Embed() calls one at a time in arrival
// order. The caps of molt_inference.mojom are checked again here; a
// violation is a bad message. Closing the pipe stops the call in progress
// before its next text.
class MoltEmbeddingModelImpl : public mojom::EmbeddingModel {
 public:
  // |on_disconnect| runs when the browser closes the EmbeddingModel pipe.
  MoltEmbeddingModelImpl(base::SequenceBound<LlamaEmbeddingEngine> engine,
                         mojo::PendingReceiver<mojom::EmbeddingModel> receiver,
                         base::OnceClosure on_disconnect);
  ~MoltEmbeddingModelImpl() override;

  MoltEmbeddingModelImpl(const MoltEmbeddingModelImpl&) = delete;
  MoltEmbeddingModelImpl& operator=(const MoltEmbeddingModelImpl&) = delete;

  // mojom::EmbeddingModel:
  void Embed(const std::vector<std::string>& texts,
             EmbedCallback callback) override;

 private:
  class CancelFlag;

  void OnEmbedded(EmbedCallback callback,
                  std::optional<std::vector<float>> vectors);

  SEQUENCE_CHECKER(sequence_checker_);

  base::SequenceBound<LlamaEmbeddingEngine> engine_;
  mojo::Receiver<mojom::EmbeddingModel> receiver_;
  scoped_refptr<CancelFlag> cancelled_;
  // Calls queued on the engine sequence, not yet answered.
  size_t pending_ = 0;

  base::WeakPtrFactory<MoltEmbeddingModelImpl> weak_factory_{this};
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_EMBEDDING_MODEL_IMPL_H_
