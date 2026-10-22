// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_INFERENCE_SERVICE_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_INFERENCE_SERVICE_H_

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"
#include "chrome/services/molt_ai/inference/llama_embedding_engine.h"
#include "chrome/services/molt_ai/inference/llama_engine.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace molt_ai {

class MoltEmbeddingModelImpl;
class MoltModelImpl;

// molt_ai.mojom.MoltInferenceService: on-device llama.cpp inference in a
// utility process sandboxed with kOnDeviceModelExecution
// (docs/design/SANDBOXED_INFERENCE.md). One process per model: LoadModel()
// is accepted once, and the process ends when the browser drops the
// service pipe (and with it the Model pipe).
//
// The model file arrives as a read-only handle opened by the browser; the
// sandbox can't open it by path. Every llama.cpp call happens on the
// engine's ThreadPool sequence (LlamaEngine), never on the main thread.
class MoltInferenceService : public mojom::MoltInferenceService {
 public:
  explicit MoltInferenceService(
      mojo::PendingReceiver<mojom::MoltInferenceService> receiver);
  ~MoltInferenceService() override;

  MoltInferenceService(const MoltInferenceService&) = delete;
  MoltInferenceService& operator=(const MoltInferenceService&) = delete;

  // mojom::MoltInferenceService:
  void LoadModel(mojom::ModelLoadParamsPtr params,
                 mojo::PendingReceiver<mojom::Model> model,
                 LoadModelCallback callback) override;
  // Semantic search (docs/design/SEMANTIC_SEARCH.md); the process's one
  // load, like LoadModel.
  void LoadEmbeddingModel(mojom::EmbeddingModelLoadParamsPtr params,
                          mojo::PendingReceiver<mojom::EmbeddingModel> model,
                          LoadEmbeddingModelCallback callback) override;

 private:
  void OnLoaded(mojo::PendingReceiver<mojom::Model> model,
                LoadModelCallback callback,
                EngineLoadResult result);
  void OnModelDisconnected();
  void OnEmbeddingLoaded(mojo::PendingReceiver<mojom::EmbeddingModel> model,
                         LoadEmbeddingModelCallback callback,
                         EmbeddingLoadResult result);

  SEQUENCE_CHECKER(sequence_checker_);

  mojo::Receiver<mojom::MoltInferenceService> receiver_;
  bool load_requested_ = false;
  // The engine while it loads; moved into |model_| on success.
  base::SequenceBound<LlamaEngine> engine_;
  std::unique_ptr<MoltModelImpl> model_;
  // The embedding engine while it loads; moved into |embedding_model_|.
  base::SequenceBound<LlamaEmbeddingEngine> embedding_engine_;
  std::unique_ptr<MoltEmbeddingModelImpl> embedding_model_;

  base::WeakPtrFactory<MoltInferenceService> weak_factory_{this};
};

// Factory for the utility process's service registry
// (chrome/utility/services.cc, RegisterMainThreadServices):
//   services.Add(molt_ai::RunMoltInferenceService);
std::unique_ptr<MoltInferenceService> RunMoltInferenceService(
    mojo::PendingReceiver<mojom::MoltInferenceService> receiver);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_MOLT_INFERENCE_SERVICE_H_
