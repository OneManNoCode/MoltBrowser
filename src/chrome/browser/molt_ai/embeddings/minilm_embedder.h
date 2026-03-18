// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MiniLM Embedding Engine for MoltBrowser.
// Provides semantic text embeddings using the all-MiniLM-L6-v2 model
// via ONNX Runtime for fast, on-device embedding computation.
//
// Model: sentence-transformers/all-MiniLM-L6-v2
// Dimensions: 384
// Speed: ~5ms per sentence on Apple Silicon
//
// Used by:
//   - MemoryEngine for semantic search
//   - DOMInterpreter for content similarity
//   - PromptRouter for intent classification

#ifndef CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_
#define CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_

#include <memory>
#include <string>
#include <vector>

namespace molt_ai {

// Embedding dimensions for all-MiniLM-L6-v2
constexpr int kMiniLMDimensions = 384;
constexpr int kMiniLMMaxTokens = 256;

// Embedding result
struct EmbeddingResult {
  std::vector<float> embedding;
  float compute_time_ms;
  bool success;
  std::string error;
};

// Batch embedding result
struct BatchEmbeddingResult {
  std::vector<std::vector<float>> embeddings;
  float compute_time_ms;
  bool success;
  std::string error;
};

class MiniLMEmbedder {
 public:
  MiniLMEmbedder();
  ~MiniLMEmbedder();

  MiniLMEmbedder(const MiniLMEmbedder&) = delete;
  MiniLMEmbedder& operator=(const MiniLMEmbedder&) = delete;

  // Initialize with path to ONNX model file.
  // Expected: ~/.moltbrowser/models/all-MiniLM-L6-v2.onnx
  bool Initialize(const std::string& model_path);

  // Check if the model is loaded and ready.
  bool IsAvailable() const;

  // Compute embedding for a single text.
  EmbeddingResult Embed(const std::string& text) const;

  // Compute embeddings for multiple texts (batched for efficiency).
  BatchEmbeddingResult EmbedBatch(
      const std::vector<std::string>& texts) const;

  // Compute cosine similarity between two embeddings.
  static float CosineSimilarity(const std::vector<float>& a,
                                 const std::vector<float>& b);

  // Get the embedding dimension (384 for MiniLM).
  int GetDimensions() const { return kMiniLMDimensions; }

  // Get the model file path.
  std::string GetModelPath() const;

  // Download the model from HuggingFace if not present.
  // Returns true if model is available (already existed or downloaded).
  bool EnsureModelAvailable(const std::string& model_dir);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_
