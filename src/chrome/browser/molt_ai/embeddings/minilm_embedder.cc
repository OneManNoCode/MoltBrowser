// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/embeddings/minilm_embedder.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>

#include "base/logging.h"

namespace molt_ai {

MiniLMEmbedder::MiniLMEmbedder() = default;
MiniLMEmbedder::~MiniLMEmbedder() = default;

bool MiniLMEmbedder::Initialize(const std::string& model_path) {
  // ONNX Runtime integration placeholder.
  // When integrated: load all-MiniLM-L6-v2 ONNX model (~23MB),
  // create session with CPU execution provider.
  onnx_available_ = false;
  LOG(INFO) << "[MoltAI] MiniLM embedder initialized (hash-based mode)";
  return true;
}

std::vector<float> MiniLMEmbedder::Embed(const std::string& text) const {
  return HashEmbed(text);
}

std::vector<float> MiniLMEmbedder::HashEmbed(const std::string& text) const {
  std::vector<float> embedding(kMiniLMDimensions, 0.0f);
  if (text.empty()) return embedding;

  std::string word;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ' ' || text[i] == '\n' ||
        text[i] == '\t' || text[i] == ',' || text[i] == '.') {
      if (!word.empty()) {
        size_t h1 = std::hash<std::string>{}(word);
        size_t h2 = std::hash<std::string>{}(word + "_2");
        embedding[h1 % kMiniLMDimensions] += 1.0f;
        embedding[h2 % kMiniLMDimensions] += 0.5f;
        word.clear();
      }
    } else {
      word += static_cast<char>(std::tolower(
          static_cast<unsigned char>(text[i])));
    }
  }

  // L2 normalize
  float norm = 0.0f;
  for (float v : embedding) norm += v * v;
  if (norm > 0.0f) {
    norm = std::sqrt(norm);
    for (float& v : embedding) v /= norm;
  }
  return embedding;
}

float MiniLMEmbedder::CosineSimilarity(const std::vector<float>& a,
                                        const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) return 0.0f;
  float dot = 0.0f, na = 0.0f, nb = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na == 0.0f || nb == 0.0f) return 0.0f;
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

bool MiniLMEmbedder::IsONNXAvailable() const {
  return onnx_available_;
}

}  // namespace molt_ai
