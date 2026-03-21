// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MiniLM Embedding Engine for MoltBrowser.
// Computes 384-dimensional sentence embeddings for semantic search
// in the memory engine. Uses hash-based approach with ONNX upgrade path.

#ifndef CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_
#define CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_

#include <string>
#include <vector>

namespace molt_ai {

constexpr int kMiniLMDimensions = 384;

class MiniLMEmbedder {
 public:
  MiniLMEmbedder();
  ~MiniLMEmbedder();

  bool Initialize(const std::string& model_path = "");
  std::vector<float> Embed(const std::string& text) const;
  static float CosineSimilarity(const std::vector<float>& a,
                                 const std::vector<float>& b);
  bool IsONNXAvailable() const;

 private:
  std::vector<float> HashEmbed(const std::string& text) const;
  bool onnx_available_ = false;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_EMBEDDINGS_MINILM_EMBEDDER_H_
