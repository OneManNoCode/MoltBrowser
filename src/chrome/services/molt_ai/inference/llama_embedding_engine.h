// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// LlamaEmbeddingEngine: a text-embedding model (llama.cpp with
// llama_context_params.embeddings) in the inference service process, for
// semantic search over AI memory (docs/design/SEMANTIC_SEARCH.md). One text
// at a time: tokenize as plain text, fit the model's window, decode, take
// the pooled vector of sequence 0, keep its leading `dimensions`
// components and L2-normalize them.
//
// Like LlamaEngine: only std and llama.h (no //base, no mojo), so it is
// built and run standalone against the vendored llama.cpp; not thread-safe
// (the service owns it on one sequence through base::SequenceBound, and
// only the cancel flag is read across threads); never logs text. The
// llama.cpp calls it makes are checked for exceptions at the top of the .cc.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_EMBEDDING_ENGINE_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_EMBEDDING_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chrome/services/molt_ai/inference/llama_engine.h"

namespace molt_ai {

// Mirrors of the molt_inference.mojom embedding types.

enum class EmbeddingPoolingType { kMean, kCls, kLast };

struct EmbeddingLoadParams {
  // Size the browser saw; a file of another size is refused.
  uint64_t file_size = 0;
  uint32_t cpu_threads = 1;
  EmbeddingPoolingType pooling = EmbeddingPoolingType::kMean;
  uint32_t max_tokens_per_text = 512;
  // A special token's text that ends every text; "" = none.
  std::string terminator;
  // Vector length returned by Embed(); at most the model's width.
  uint32_t dimensions = 0;
};

struct EmbeddingModelInfo {
  std::string architecture;
  std::string backend;
  uint32_t native_dimensions = 0;
  uint32_t dimensions = 0;
  uint32_t max_tokens_per_text = 0;
  uint32_t vocab_size = 0;
};

struct EmbeddingLoadResult {
  EngineLoadStatus status = EngineLoadStatus::kInvalidModelFile;
  EmbeddingModelInfo info;
};

class LlamaEmbeddingEngine {
 public:
  // Matches molt_ai.mojom.kMinEmbedTokensPerText / kMaxEmbedTokensPerText /
  // kMaxEmbeddingDimensions / kMaxEmbedTerminatorBytes.
  static constexpr uint32_t kMinTokensPerText = 8;
  static constexpr uint32_t kMaxTokensPerText = 2048;
  static constexpr uint32_t kMaxDimensions = 4096;
  static constexpr size_t kMaxTerminatorBytes = 64;

  LlamaEmbeddingEngine();
  ~LlamaEmbeddingEngine();
  LlamaEmbeddingEngine(const LlamaEmbeddingEngine&) = delete;
  LlamaEmbeddingEngine& operator=(const LlamaEmbeddingEngine&) = delete;

  // Loads the GGUF read through |file| (read-only, synchronous) and creates
  // an embeddings context of max_tokens_per_text tokens. |file| stays open
  // until the model is freed; it is closed by the destructor, or here on
  // failure. Call at most once.
  EmbeddingLoadResult Load(ScopedStdioFile file,
                           const EmbeddingLoadParams& params);
  bool loaded() const;

  // texts.size() * dimensions floats, one vector per text in order, each
  // L2-normalized (all zeros for a text with no tokens). nullopt if not
  // loaded, if a decode fails or yields no or non-finite values, or if
  // |is_cancelled| (polled before each text; may be empty) returns true.
  std::optional<std::vector<float>> Embed(
      const std::vector<std::string>& texts,
      const std::function<bool()>& is_cancelled);

 private:
  struct Model;
  std::unique_ptr<Model> model_;
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_EMBEDDING_ENGINE_H_
