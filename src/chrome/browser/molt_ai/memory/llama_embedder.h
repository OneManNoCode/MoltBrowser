// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// LlamaEmbedder — the MemoryEmbedder for the on-device embedding model
// (docs/design/SEMANTIC_SEARCH.md). Asynchronous: each EmbedBatch() goes
// to the UI thread, to EmbeddingProcessHost and the sandboxed inference
// service, and its validated reply comes back on the calling sequence.
// Queries get the model's instruction prefix and go ahead of background
// work; documents are sent as they are.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_LLAMA_EMBEDDER_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_LLAMA_EMBEDDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "chrome/browser/molt_ai/memory/embedding_model_registry.h"
#include "chrome/browser/molt_ai/memory/memory_embedder.h"
#include "chrome/browser/molt_ai/runtime/embedding_process_host.h"

namespace molt_ai {
namespace memory {

class LlamaEmbedder : public MemoryEmbedder {
 public:
  // |spec| must be pinned; |model_path| is where the GGUF was downloaded.
  LlamaEmbedder(const EmbeddingModelSpec& spec,
                const base::FilePath& model_path);
  ~LlamaEmbedder() override;

  // MemoryEmbedder:
  EmbedderSpace space() const override;
  bool is_synchronous() const override;
  std::vector<float> Embed(const std::string& text) const override;
  void EmbedBatch(std::vector<std::string> texts,
                  EmbedPurpose purpose,
                  BatchCallback done) const override;
  // Only through a model process that is already loaded
  // (EmbeddingProcessHost::EmbedIfLoaded()); kRetryLater otherwise.
  void EmbedQueryIfReady(std::string text, BatchCallback done) const override;
  size_t max_batch_size() const override;

  const EmbeddingModelConfig& config_for_testing() const { return config_; }

 private:
  EmbeddingModelConfig config_;
  EmbedderSpace space_;
  std::string query_prefix_;
  std::string document_prefix_;
};

// What the host's status means for memory.
EmbedOutcome OutcomeForStatus(EmbeddingStatus status);

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_LLAMA_EMBEDDER_H_
