// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/embeddings/minilm_embedder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <numeric>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"

// ONNX Runtime headers — conditionally included
// Install ONNX Runtime: https://onnxruntime.ai/
// brew install onnxruntime (macOS)
// apt install libonnxruntime-dev (Linux)
#if __has_include("onnxruntime/core/session/onnxruntime_cxx_api.h")
#include "onnxruntime/core/session/onnxruntime_cxx_api.h"
#define MOLT_HAS_ONNXRUNTIME 1
#else
#define MOLT_HAS_ONNXRUNTIME 0
#endif

namespace molt_ai {

struct MiniLMEmbedder::Impl {
  std::string model_path;
  bool initialized = false;

#if MOLT_HAS_ONNXRUNTIME
  std::unique_ptr<Ort::Env> env;
  std::unique_ptr<Ort::Session> session;
  std::unique_ptr<Ort::SessionOptions> session_options;
  Ort::AllocatorWithDefaultOptions allocator;
#endif

  // Simple word-piece-like tokenizer (functional placeholder)
  // In production, use HuggingFace tokenizers library or built-in BPE
  struct SimpleTokenizer {
    // Simplified tokenization: lowercase, split on whitespace/punct
    std::vector<int64_t> Tokenize(const std::string& text,
                                   int max_length) const {
      std::vector<int64_t> tokens;
      tokens.push_back(101);  // [CLS]

      std::string word;
      for (size_t i = 0; i < text.size() && tokens.size() < (size_t)(max_length - 1); ++i) {
        char c = text[i];
        if (std::isalnum(c)) {
          word += std::tolower(c);
        } else {
          if (!word.empty()) {
            // Hash word to a vocab ID (simplified)
            size_t hash = std::hash<std::string>{}(word);
            tokens.push_back(static_cast<int64_t>(hash % 30000) + 1000);
            word.clear();
          }
        }
      }
      if (!word.empty()) {
        size_t hash = std::hash<std::string>{}(word);
        tokens.push_back(static_cast<int64_t>(hash % 30000) + 1000);
      }

      tokens.push_back(102);  // [SEP]
      return tokens;
    }
  } tokenizer;

  // Hash-based embedding fallback (when ONNX Runtime not available)
  std::vector<float> HashEmbed(const std::string& text) const {
    std::vector<float> embedding(kMiniLMDimensions, 0.0f);

    if (text.empty())
      return embedding;

    std::string word;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || !std::isalnum(text[i])) {
        if (!word.empty()) {
          // Distribute word across multiple dimensions for richer repr
          size_t h1 = std::hash<std::string>{}(word);
          size_t h2 = std::hash<std::string>{}(word + "_2");
          size_t h3 = std::hash<std::string>{}(word + "_3");

          embedding[h1 % kMiniLMDimensions] += 1.0f;
          embedding[h2 % kMiniLMDimensions] += 0.5f;
          embedding[h3 % kMiniLMDimensions] += 0.25f;

          // Bigram with previous word context
          if (i > word.size() + 1) {
            size_t h4 = std::hash<std::string>{}(
                text.substr(i - word.size() - 2, word.size() + 1));
            embedding[h4 % kMiniLMDimensions] += 0.3f;
          }

          word.clear();
        }
      } else {
        word += std::tolower(text[i]);
      }
    }

    // L2 normalize
    float norm = 0.0f;
    for (float v : embedding)
      norm += v * v;
    if (norm > 0.0f) {
      norm = std::sqrt(norm);
      for (float& v : embedding)
        v /= norm;
    }

    return embedding;
  }
};

MiniLMEmbedder::MiniLMEmbedder() : impl_(std::make_unique<Impl>()) {}
MiniLMEmbedder::~MiniLMEmbedder() = default;

bool MiniLMEmbedder::Initialize(const std::string& model_path) {
  impl_->model_path = model_path;

#if MOLT_HAS_ONNXRUNTIME
  try {
    impl_->env = std::make_unique<Ort::Env>(
        ORT_LOGGING_LEVEL_WARNING, "MoltAI_MiniLM");

    impl_->session_options = std::make_unique<Ort::SessionOptions>();
    impl_->session_options->SetIntraOpNumThreads(2);
    impl_->session_options->SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);

    // Enable CoreML on macOS / Metal on iOS for acceleration
#if defined(__APPLE__)
    // Ort::SessionOptions::AppendExecutionProvider_CoreML(0) if available
#endif

    if (base::PathExists(base::FilePath(model_path))) {
      impl_->session = std::make_unique<Ort::Session>(
          *impl_->env, model_path.c_str(), *impl_->session_options);
      impl_->initialized = true;
      LOG(INFO) << "[MoltAI] MiniLM ONNX model loaded: " << model_path;
      return true;
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "[MoltAI] ONNX Runtime error: " << e.what();
  }
#endif

  // Fallback: hash-based embeddings
  impl_->initialized = true;
  LOG(INFO) << "[MoltAI] MiniLM using hash-based embeddings "
            << "(ONNX Runtime not available or model not found)";
  return true;
}

bool MiniLMEmbedder::IsAvailable() const {
  return impl_->initialized;
}

EmbeddingResult MiniLMEmbedder::Embed(const std::string& text) const {
  EmbeddingResult result;
  auto start = std::chrono::steady_clock::now();

#if MOLT_HAS_ONNXRUNTIME
  if (impl_->session) {
    try {
      // Tokenize input
      auto token_ids = impl_->tokenizer.Tokenize(text, kMiniLMMaxTokens);
      int64_t seq_len = static_cast<int64_t>(token_ids.size());

      // Create attention mask (all 1s)
      std::vector<int64_t> attention_mask(seq_len, 1);

      // Create token type IDs (all 0s for single sentence)
      std::vector<int64_t> token_type_ids(seq_len, 0);

      // Create input tensors
      std::array<int64_t, 2> input_shape = {1, seq_len};
      auto memory_info = Ort::MemoryInfo::CreateCpu(
          OrtArenaAllocator, OrtMemTypeDefault);

      std::vector<Ort::Value> input_tensors;
      input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
          memory_info, token_ids.data(), token_ids.size(),
          input_shape.data(), input_shape.size()));
      input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
          memory_info, attention_mask.data(), attention_mask.size(),
          input_shape.data(), input_shape.size()));
      input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
          memory_info, token_type_ids.data(), token_type_ids.size(),
          input_shape.data(), input_shape.size()));

      // Input/output names
      const char* input_names[] = {
          "input_ids", "attention_mask", "token_type_ids"};
      const char* output_names[] = {"last_hidden_state"};

      // Run inference
      auto output_tensors = impl_->session->Run(
          Ort::RunOptions{nullptr},
          input_names, input_tensors.data(), 3,
          output_names, 1);

      // Mean pooling over sequence dimension
      auto& output = output_tensors[0];
      auto output_shape = output.GetTensorTypeAndShapeInfo().GetShape();
      const float* output_data = output.GetTensorData<float>();

      result.embedding.resize(kMiniLMDimensions, 0.0f);
      for (int64_t t = 0; t < seq_len; ++t) {
        for (int d = 0; d < kMiniLMDimensions; ++d) {
          result.embedding[d] += output_data[t * kMiniLMDimensions + d];
        }
      }
      // Average
      for (float& v : result.embedding) {
        v /= static_cast<float>(seq_len);
      }

      // L2 normalize
      float norm = 0.0f;
      for (float v : result.embedding)
        norm += v * v;
      if (norm > 0.0f) {
        norm = std::sqrt(norm);
        for (float& v : result.embedding)
          v /= norm;
      }

      result.success = true;
      auto end = std::chrono::steady_clock::now();
      result.compute_time_ms =
          std::chrono::duration<float, std::milli>(end - start).count();
      return result;
    } catch (const std::exception& e) {
      LOG(WARNING) << "[MoltAI] ONNX inference failed, falling back: "
                   << e.what();
    }
  }
#endif

  // Fallback: hash-based embedding
  result.embedding = impl_->HashEmbed(text);
  result.success = true;
  auto end = std::chrono::steady_clock::now();
  result.compute_time_ms =
      std::chrono::duration<float, std::milli>(end - start).count();
  return result;
}

BatchEmbeddingResult MiniLMEmbedder::EmbedBatch(
    const std::vector<std::string>& texts) const {
  BatchEmbeddingResult result;
  auto start = std::chrono::steady_clock::now();

  result.embeddings.reserve(texts.size());
  for (const auto& text : texts) {
    auto single = Embed(text);
    result.embeddings.push_back(std::move(single.embedding));
  }

  result.success = true;
  auto end = std::chrono::steady_clock::now();
  result.compute_time_ms =
      std::chrono::duration<float, std::milli>(end - start).count();
  return result;
}

float MiniLMEmbedder::CosineSimilarity(const std::vector<float>& a,
                                         const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty())
    return 0.0f;

  float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }

  if (norm_a == 0.0f || norm_b == 0.0f)
    return 0.0f;

  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

std::string MiniLMEmbedder::GetModelPath() const {
  return impl_->model_path;
}

bool MiniLMEmbedder::EnsureModelAvailable(const std::string& model_dir) {
  base::FilePath model_path =
      base::FilePath(model_dir).Append(
          FILE_PATH_LITERAL("all-MiniLM-L6-v2.onnx"));

  if (base::PathExists(model_path)) {
    return Initialize(model_path.value());
  }

  // Model not found — initialize with hash fallback
  LOG(INFO) << "[MoltAI] MiniLM ONNX model not found at "
            << model_path.value()
            << " — using hash-based embeddings. "
            << "Download from: https://huggingface.co/sentence-transformers/"
            << "all-MiniLM-L6-v2/resolve/main/onnx/model.onnx";
  return Initialize(model_path.value());
}

}  // namespace molt_ai
