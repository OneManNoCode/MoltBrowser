// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_MODELS_MODEL_MANAGER_H_
#define MOLT_AI_MODELS_MODEL_MANAGER_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

namespace molt_ai {

// Download progress info
struct DownloadProgress {
  std::string model_id;
  size_t bytes_downloaded;
  size_t total_bytes;
  float percent_complete;
  float speed_bytes_per_sec;
  int estimated_seconds_remaining;
  bool is_complete;
  bool has_error;
  std::string error_message;
};

// Download callback
using DownloadCallback = std::function<void(const DownloadProgress& progress)>;

// ============================================================
// ModelManager — GGUF model lifecycle management
// ============================================================
// Handles downloading, storing, verifying, loading, and
// unloading of GGUF quantized models.
//
// Model sources (free, from HuggingFace Hub):
//   - LLaMA 3.1 8B: bartowski/Meta-Llama-3.1-8B-Instruct-GGUF
//   - Qwen2.5 7B: Qwen/Qwen2.5-7B-Instruct-GGUF
//   - Mistral 7B: MistralAI/Mistral-7B-Instruct-v0.3-GGUF
//   - Phi-3.5 Mini: bartowski/Phi-3.5-mini-instruct-GGUF
//   - Gemma 2 9B: bartowski/gemma-2-9b-it-GGUF
//   - TinyLlama 1.1B: TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF
//   - Embedding: sentence-transformers/all-MiniLM-L6-v2 (ONNX)
//
// Storage: ~/.moltbrowser/models/
// Format: GGUF only (via llama.cpp)
// ============================================================
class ModelManager {
 public:
  ModelManager();
  ~ModelManager();

  // Initialize with model storage directory
  bool Initialize(const std::string& model_dir);

  // ---- Discovery ----

  // Get all models in the registry (both downloaded and available)
  std::vector<ModelInfo> GetAllModels() const;

  // Get only downloaded models
  std::vector<ModelInfo> GetDownloadedModels() const;

  // Get models that can run on current hardware
  std::vector<ModelInfo> GetCompatibleModels(
      const HardwareCapability& hardware) const;

  // ---- Download ----

  // Download a model from HuggingFace Hub
  bool DownloadModel(const std::string& model_id,
                     DownloadCallback callback = nullptr);

  // Cancel an in-progress download
  void CancelDownload(const std::string& model_id);

  // Check if a model is currently downloading
  bool IsDownloading(const std::string& model_id) const;

  // ---- Storage ----

  // Get the local file path for a model
  std::string GetModelPath(const std::string& model_id) const;

  // Delete a downloaded model
  bool DeleteModel(const std::string& model_id);

  // Get total disk usage of all downloaded models
  size_t GetTotalDiskUsage() const;

  // ---- Verification ----

  // Verify model file integrity (SHA256)
  bool VerifyModel(const std::string& model_id) const;

  // Get the recommended model for given hardware
  std::string RecommendModel(const HardwareCapability& hardware) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // MOLT_AI_MODELS_MODEL_MANAGER_H_
