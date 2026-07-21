// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/models/model_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#endif

namespace molt_ai {

struct ModelManager::Impl {
  std::string model_dir;
  bool initialized = false;
  std::vector<ModelInfo> registry;
  std::unordered_map<std::string, bool> download_in_progress;

  // Returns the path to a bundled model if it exists in the .app's Resources
  // directory. On macOS, this is Chromium.app/Contents/Resources/molt_models/.
  // Returns empty string if no bundled model exists.
  std::string GetBundledModelPath(const std::string& model_id) const {
#if BUILDFLAG(IS_MAC)
    // Check the main bundle's Contents/Resources/molt_models/ directory
    // (where package-dmg.sh places the bundled TinyLlama model).
    base::FilePath main_resources = base::apple::MainBundlePath().Append(
        "Contents/Resources/molt_models");
    base::FilePath bundled = main_resources.Append(model_id + ".gguf");
    if (std::filesystem::exists(bundled.value())) {
      return bundled.value();
    }
    // Also try the framework bundle's Resources/ directory
    base::FilePath framework_resources =
        base::apple::FrameworkBundlePath().Append("Resources/molt_models");
    bundled = framework_resources.Append(model_id + ".gguf");
    if (std::filesystem::exists(bundled.value())) {
      return bundled.value();
    }
#elif BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX)
    base::FilePath exe_dir;
    if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
      base::FilePath bundled =
          exe_dir.Append(FILE_PATH_LITERAL("molt_models"))
              .Append(base::FilePath::FromUTF8Unsafe(model_id + ".gguf"));
      if (std::filesystem::exists(bundled.value())) {
        return bundled.AsUTF8Unsafe();
      }
    }
#endif
    return "";
  }

  void ScanLocalModels() {
    if (!std::filesystem::exists(model_dir)) {
      std::filesystem::create_directories(model_dir);
    }

    for (auto& model : registry) {
      // 1. First check if the model is bundled with the app (free, no
      //    download needed). This lets us ship MoltBrowser with TinyLlama
      //    pre-installed so users have AI working out of the box.
      std::string bundled_path = GetBundledModelPath(model.model_id);
      if (!bundled_path.empty()) {
        model.is_downloaded = true;
        model.file_path = bundled_path;
        model.file_size_bytes = std::filesystem::file_size(bundled_path);
        LOG(INFO) << "[MoltAI] Found bundled model: " << model.model_id
                  << " at " << bundled_path;
        continue;
      }

      // 2. Otherwise check the user's local models directory (downloaded).
      std::string path = model_dir + "/" + model.model_id + ".gguf";
      if (std::filesystem::exists(path)) {
        model.is_downloaded = true;
        model.file_path = path;
        model.file_size_bytes = std::filesystem::file_size(path);
      } else {
        model.is_downloaded = false;
      }
    }
  }
};

ModelManager::ModelManager() : impl_(std::make_unique<Impl>()) {}
ModelManager::~ModelManager() = default;

bool ModelManager::Initialize(const std::string& model_dir) {
  impl_->model_dir = model_dir;

  // Create directory if needed
  std::filesystem::create_directories(model_dir);

  // Initialize model registry from known HuggingFace sources
  // Refreshed 2026-07 to the current generation of free, open-weight, on-device
  // models (all run 100% locally via llama.cpp — nothing is sent to the maker).
  // Ordered small -> large. `company` drives the brand icon in the picker.
  impl_->registry = {
      // Ships in the box — zero-download default (community project).
      {"tinyllama-1.1b", "TinyLlama 1.1B Chat", "",
       "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
       637534208ULL, 2147483648ULL, 1, "Q4_K_M", false, false, "TinyLlama"},
      // Meta — open weights (Llama Community License)
      {"llama3.2-3b", "Llama 3.2 3B Instruct", "",
       "bartowski/Llama-3.2-3B-Instruct-GGUF",
       2019377152ULL, 4294967296ULL, 3, "Q4_K_M", false, false, "Meta"},
      // Alibaba — Apache 2.0; strong tool-use/agentic. "Qwen" is the
      // recognizable HF publisher label shown in the picker.
      {"qwen3-4b", "Qwen3 4B Instruct", "",
       "Qwen/Qwen3-4B-GGUF",
       2500000000ULL, 4294967296ULL, 4, "Q4_K_M", false, false, "Qwen"},
      // Google DeepMind — open weights (Gemma Terms). Labeled "Gemma" (the
      // model brand) NOT "Google" to honor the zero-"Google"-string rule.
      {"gemma3-4b", "Gemma 3 4B Instruct", "",
       "unsloth/gemma-3-4b-it-GGUF",
       2490000000ULL, 4294967296ULL, 4, "Q4_K_M", false, false, "Gemma"},
      // Mistral AI — Apache 2.0
      {"mistral-7b", "Mistral 7B Instruct v0.3", "",
       "bartowski/Mistral-7B-Instruct-v0.3-GGUF",
       4368438272ULL, 6442450944ULL, 7, "Q4_K_M", false, false, "Mistral AI"},
      // Meta — open weights; proven all-rounder
      {"llama3.1-8b", "Llama 3.1 8B Instruct", "",
       "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF",
       4615733248ULL, 6442450944ULL, 8, "Q4_K_M", false, false, "Meta"},
      // Qwen (Alibaba) — Apache 2.0; recommended default for page-Q&A + agentic
      {"qwen3-8b", "Qwen3 8B Instruct", "",
       "Qwen/Qwen3-8B-GGUF",
       5030000000ULL, 6442450944ULL, 8, "Q4_K_M", false, false, "Qwen"},
      // Microsoft — MIT
      {"phi-4", "Phi-4 14B", "",
       "bartowski/phi-4-GGUF",
       9050000000ULL, 12884901888ULL, 14, "Q4_K_M", false, false, "Microsoft"},
      // OpenAI — Apache 2.0 open weights (native MXFP4 quant)
      {"gpt-oss-20b", "gpt-oss 20B", "",
       "ggml-org/gpt-oss-20b-GGUF",
       12110000000ULL, 17179869184ULL, 20, "MXFP4", false, false, "OpenAI"},
  };

  // Scan for locally downloaded models
  impl_->ScanLocalModels();

  impl_->initialized = true;
  return true;
}

std::vector<ModelInfo> ModelManager::GetAllModels() const {
  return impl_->registry;
}

std::vector<ModelInfo> ModelManager::GetDownloadedModels() const {
  std::vector<ModelInfo> result;
  for (const auto& model : impl_->registry) {
    if (model.is_downloaded) {
      result.push_back(model);
    }
  }
  return result;
}

std::vector<ModelInfo> ModelManager::GetCompatibleModels(
    const HardwareCapability& hardware) const {
  std::vector<ModelInfo> result;
  size_t max_ram = static_cast<size_t>(hardware.total_ram_bytes * 0.50f);

  for (const auto& model : impl_->registry) {
    if (model.ram_required_bytes <= max_ram) {
      result.push_back(model);
    }
  }

  // Sort by size (smallest first)
  std::sort(result.begin(), result.end(),
            [](const ModelInfo& a, const ModelInfo& b) {
              return a.file_size_bytes < b.file_size_bytes;
            });

  return result;
}

bool ModelManager::DownloadModel(const std::string& model_id,
                                  DownloadCallback callback) {
  // Find model in registry
  ModelInfo* model = nullptr;
  for (auto& m : impl_->registry) {
    if (m.model_id == model_id) {
      model = &m;
      break;
    }
  }

  if (!model) return false;
  if (model->is_downloaded) return true;
  if (IsDownloading(model_id)) return false;

  impl_->download_in_progress[model_id] = true;

  // TODO: Implement actual HuggingFace Hub download
  // URL pattern: https://huggingface.co/{repo}/resolve/main/{filename}
  // Download in chunks, verify SHA256, report progress via callback
  //
  // Example URL:
  // https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF/
  //   resolve/main/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf

  impl_->download_in_progress[model_id] = false;
  return false;  // Not yet implemented
}

void ModelManager::CancelDownload(const std::string& model_id) {
  impl_->download_in_progress[model_id] = false;
}

bool ModelManager::IsDownloading(const std::string& model_id) const {
  auto it = impl_->download_in_progress.find(model_id);
  return it != impl_->download_in_progress.end() && it->second;
}

std::string ModelManager::GetModelPath(const std::string& model_id) const {
  for (const auto& model : impl_->registry) {
    if (model.model_id == model_id && model.is_downloaded) {
      return model.file_path;
    }
  }
  return "";
}

bool ModelManager::DeleteModel(const std::string& model_id) {
  for (auto& model : impl_->registry) {
    if (model.model_id == model_id) {
      if (model.is_downloaded && !model.file_path.empty()) {
        std::filesystem::remove(model.file_path);
        model.is_downloaded = false;
        model.file_path.clear();
        return true;
      }
      break;
    }
  }
  return false;
}

size_t ModelManager::GetTotalDiskUsage() const {
  size_t total = 0;
  for (const auto& model : impl_->registry) {
    if (model.is_downloaded) {
      total += model.file_size_bytes;
    }
  }
  return total;
}

bool ModelManager::VerifyModel(const std::string& model_id) const {
  // TODO: SHA256 verification
  return true;
}

std::string ModelManager::RecommendModel(
    const HardwareCapability& hardware) const {
  auto compatible = GetCompatibleModels(hardware);

  if (compatible.empty()) {
    return "";  // No compatible models
  }

  // Prefer the largest model that fits in 50% RAM
  // and is already downloaded
  for (auto it = compatible.rbegin(); it != compatible.rend(); ++it) {
    if (it->is_downloaded) {
      return it->model_id;
    }
  }

  // If nothing downloaded, recommend based on hardware
  size_t available = static_cast<size_t>(hardware.total_ram_bytes * 0.50f);

  if (available >= 8589934592ULL) {  // 8GB+
    return "qwen3-8b";
  } else if (available >= 4294967296ULL) {  // 4GB+
    return "qwen3-4b";
  } else {
    return "tinyllama-1.1b";
  }
}

}  // namespace molt_ai
