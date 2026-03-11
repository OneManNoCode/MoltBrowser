// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "src/molt_ai/runtime/browser_ai_runtime.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

// llama.cpp headers will be included when integrated
// #include "third_party/llama_cpp/llama.h"
// #include "third_party/llama_cpp/common.h"

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

namespace molt_ai {

namespace {

// Default model storage directory
const char kDefaultModelDir[] = "~/.moltbrowser/models/";

// Resource limits (from V4 spec: ResourceGovernor)
constexpr float kMaxRamUsagePercent = 0.50f;   // 50% of system RAM
constexpr float kMaxGpuUsagePercent = 0.70f;   // 70% of GPU VRAM
constexpr int kMaxConcurrentModels = 2;

// Model registry — free GGUF models from HuggingFace
struct ModelRegistryEntry {
  const char* model_id;
  const char* display_name;
  const char* huggingface_id;
  const char* filename;
  size_t file_size_bytes;
  size_t ram_required_bytes;
  int param_billions;
  const char* quantization;
};

const ModelRegistryEntry kModelRegistry[] = {
    {"tinyllama-1.1b",
     "TinyLlama 1.1B Chat",
     "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     637534208,           // ~0.6GB
     2147483648ULL,       // 2GB RAM
     1,
     "Q4_K_M"},

    {"phi-3.5-mini",
     "Phi-3.5 Mini Instruct",
     "bartowski/Phi-3.5-mini-instruct-GGUF",
     "Phi-3.5-mini-instruct-Q4_K_M.gguf",
     2362232832ULL,       // ~2.2GB
     4294967296ULL,       // 4GB RAM
     3,
     "Q4_K_M"},

    {"mistral-7b",
     "Mistral 7B Instruct v0.3",
     "MistralAI/Mistral-7B-Instruct-v0.3-GGUF",
     "Mistral-7B-Instruct-v0.3.Q4_K_M.gguf",
     4368438272ULL,       // ~4.1GB
     6442450944ULL,       // 6GB RAM
     7,
     "Q4_K_M"},

    {"llama3.1-8b",
     "LLaMA 3.1 8B Instruct",
     "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF",
     "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf",
     4615733248ULL,       // ~4.3GB
     6442450944ULL,       // 6GB RAM
     8,
     "Q4_K_M"},

    {"qwen2.5-7b",
     "Qwen2.5 7B Instruct",
     "Qwen/Qwen2.5-7B-Instruct-GGUF",
     "qwen2.5-7b-instruct-q4_k_m.gguf",
     4718592000ULL,       // ~4.4GB
     6442450944ULL,       // 6GB RAM
     7,
     "Q4_K_M"},

    {"gemma2-9b",
     "Gemma 2 9B Instruct",
     "bartowski/gemma-2-9b-it-GGUF",
     "gemma-2-9b-it-Q4_K_M.gguf",
     5905580032ULL,       // ~5.5GB
     8589934592ULL,       // 8GB RAM
     9,
     "Q4_K_M"},
};

constexpr size_t kModelRegistrySize =
    sizeof(kModelRegistry) / sizeof(kModelRegistry[0]);

}  // namespace

// ============================================================
// Implementation (PIMPL pattern)
// ============================================================
struct BrowserAIRuntime::Impl {
  HardwareCapability hardware;
  std::unordered_map<std::string, ModelInfo> models;
  std::string active_model_id;
  std::mutex generation_mutex;
  bool is_generating = false;
  bool cancel_requested = false;
  bool initialized = false;
  std::string model_directory;

  // llama.cpp context handles (to be populated when llama.cpp is linked)
  // llama_model* llama_model_handle = nullptr;
  // llama_context* llama_ctx = nullptr;

  void DetectHardware() {
    // Detect system RAM
#ifdef __APPLE__
    // macOS: use sysctl
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0);
    hardware.total_ram_bytes = static_cast<size_t>(memsize);

    // Apple Silicon always has unified memory
    hardware.gpu_backend = "metal";
    hardware.has_gpu_acceleration = true;
    hardware.architecture = "arm64";
    // On Apple Silicon, GPU VRAM = shared with system RAM
    hardware.gpu_vram_bytes = hardware.total_ram_bytes;
#elif defined(_WIN32)
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    hardware.total_ram_bytes = memInfo.ullTotalPhys;
    hardware.gpu_backend = "directx";  // Will probe for Vulkan/CUDA
    hardware.architecture = "x86_64";
#else
    // Linux: read from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
      if (line.find("MemTotal:") == 0) {
        size_t kb = 0;
        sscanf(line.c_str(), "MemTotal: %zu kB", &kb);
        hardware.total_ram_bytes = kb * 1024;
        break;
      }
    }
    hardware.gpu_backend = "vulkan";  // Will probe for CUDA
    hardware.architecture = "x86_64";
#endif

    hardware.available_ram_bytes = hardware.total_ram_bytes;  // Will refine
    hardware.cpu_cores = std::thread::hardware_concurrency();
  }

  void InitializeModelRegistry() {
    for (size_t i = 0; i < kModelRegistrySize; ++i) {
      const auto& entry = kModelRegistry[i];
      ModelInfo info;
      info.model_id = entry.model_id;
      info.display_name = entry.display_name;
      info.huggingface_id = entry.huggingface_id;
      info.file_size_bytes = entry.file_size_bytes;
      info.ram_required_bytes = entry.ram_required_bytes;
      info.parameter_count_billions = entry.param_billions;
      info.quantization = entry.quantization;

      // Check if model file exists locally
      std::string model_path = model_directory + "/" + entry.filename;
      info.file_path = model_path;
      info.is_downloaded = std::filesystem::exists(model_path);
      info.is_loaded = false;

      models[entry.model_id] = info;
    }
  }
};

// ============================================================
// Public API Implementation
// ============================================================

BrowserAIRuntime::BrowserAIRuntime() : impl_(std::make_unique<Impl>()) {}

BrowserAIRuntime::~BrowserAIRuntime() {
  Shutdown();
}

bool BrowserAIRuntime::Initialize() {
  if (impl_->initialized) return true;

  // Detect hardware capabilities
  impl_->DetectHardware();

  // Set up model directory
  const char* home = getenv("HOME");
  if (home) {
    impl_->model_directory = std::string(home) + "/.moltbrowser/models";
  } else {
    impl_->model_directory = "/tmp/moltbrowser/models";
  }

  // Create model directory if it doesn't exist
  std::filesystem::create_directories(impl_->model_directory);

  // Initialize model registry
  impl_->InitializeModelRegistry();

  impl_->initialized = true;
  return true;
}

void BrowserAIRuntime::Shutdown() {
  if (!impl_->initialized) return;

  CancelGeneration();

  // Unload all models
  for (auto& [id, info] : impl_->models) {
    if (info.is_loaded) {
      UnloadModel(id);
    }
  }

  impl_->initialized = false;
}

bool BrowserAIRuntime::LoadModel(const std::string& model_id) {
  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end()) {
    return false;
  }

  ModelInfo& info = it->second;

  if (!info.is_downloaded) {
    return false;  // Model not downloaded yet
  }

  if (info.is_loaded) {
    return true;  // Already loaded
  }

  // Check if we have enough RAM
  if (!CanRunModel(model_id)) {
    return false;
  }

  // ResourceGovernor: Check concurrent model limit
  int loaded_count = 0;
  for (const auto& [id, m] : impl_->models) {
    if (m.is_loaded) loaded_count++;
  }
  if (loaded_count >= kMaxConcurrentModels) {
    return false;
  }

  // TODO: Actual llama.cpp model loading
  // llama_model_params model_params = llama_model_default_params();
  // model_params.n_gpu_layers = -1;  // Offload all layers to GPU
  // impl_->llama_model_handle = llama_load_model_from_file(
  //     info.file_path.c_str(), model_params);

  info.is_loaded = true;
  impl_->active_model_id = model_id;
  return true;
}

bool BrowserAIRuntime::UnloadModel(const std::string& model_id) {
  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end()) {
    return false;
  }

  ModelInfo& info = it->second;
  if (!info.is_loaded) {
    return true;  // Already unloaded
  }

  // TODO: Actual llama.cpp cleanup
  // llama_free(impl_->llama_ctx);
  // llama_free_model(impl_->llama_model_handle);

  info.is_loaded = false;
  if (impl_->active_model_id == model_id) {
    impl_->active_model_id.clear();
  }
  return true;
}

std::vector<ModelInfo> BrowserAIRuntime::GetAvailableModels() const {
  std::vector<ModelInfo> result;
  result.reserve(impl_->models.size());
  for (const auto& [id, info] : impl_->models) {
    result.push_back(info);
  }
  return result;
}

ModelInfo BrowserAIRuntime::GetModelInfo(const std::string& model_id) const {
  auto it = impl_->models.find(model_id);
  if (it != impl_->models.end()) {
    return it->second;
  }
  return {};
}

bool BrowserAIRuntime::DownloadModel(
    const std::string& model_id,
    std::function<void(size_t, size_t)> progress_callback) {
  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end()) {
    return false;
  }

  ModelInfo& info = it->second;
  if (info.is_downloaded) {
    return true;  // Already downloaded
  }

  // TODO: Implement HuggingFace Hub download
  // Uses HTTPS GET to huggingface.co/api/models/{repo}/resolve/main/{filename}
  // Downloads in chunks with progress callback
  // Verifies SHA256 hash after download

  return false;  // Not yet implemented
}

bool BrowserAIRuntime::DeleteModel(const std::string& model_id) {
  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end()) {
    return false;
  }

  ModelInfo& info = it->second;
  if (info.is_loaded) {
    UnloadModel(model_id);
  }

  if (info.is_downloaded) {
    std::filesystem::remove(info.file_path);
    info.is_downloaded = false;
  }

  return true;
}

GenerationResult BrowserAIRuntime::RunPrompt(const std::string& prompt,
                                              const PromptOptions& options) {
  GenerationResult result;
  result.success = false;

  std::lock_guard<std::mutex> lock(impl_->generation_mutex);

  // Select model
  std::string model_id = options.model_id;
  if (model_id.empty()) {
    model_id = impl_->active_model_id;
  }
  if (model_id.empty()) {
    result.error_message = "No model loaded";
    return result;
  }

  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end() || !it->second.is_loaded) {
    result.error_message = "Model not loaded: " + model_id;
    return result;
  }

  impl_->is_generating = true;
  impl_->cancel_requested = false;

  auto start = std::chrono::steady_clock::now();

  // TODO: Actual llama.cpp inference
  // 1. Tokenize prompt
  // 2. Create context with parameters
  // 3. Run inference loop
  // 4. Detokenize output
  // 5. Return result

  // Placeholder for now
  result.text = "[MoltBrowser AI: Model " + model_id + " inference pending llama.cpp integration]";
  result.model_used = model_id;
  result.tokens_generated = 0;
  result.tokens_prompt = 0;
  result.success = true;

  auto end = std::chrono::steady_clock::now();
  result.generation_time_ms = std::chrono::duration<float, std::milli>(end - start).count();

  impl_->is_generating = false;
  return result;
}

void BrowserAIRuntime::StreamPrompt(const std::string& prompt,
                                     TokenCallback callback,
                                     const PromptOptions& options) {
  // TODO: Streaming inference with llama.cpp
  // Runs in a separate thread, calls callback for each token
  // Checks cancel_requested between tokens

  auto result = RunPrompt(prompt, options);
  if (callback) {
    callback(result.text, true);
  }
}

void BrowserAIRuntime::CancelGeneration() {
  impl_->cancel_requested = true;
}

RouteTarget BrowserAIRuntime::RoutePrompt(const std::string& prompt,
                                           const PageContext& context) const {
  // Routing decision matrix from V4 spec:
  // - Page summarization → LOCAL
  // - Q&A about page → LOCAL
  // - Agent tasks → LOCAL
  // - Form fill → LOCAL
  // - Complex reasoning → CLOUD
  // - Code generation → CLOUD
  // - AI Council debate → HYBRID
  // - Offline → LOCAL
  //
  // HARD RULE: If PII detected in prompt → force LOCAL

  // Check for PII indicators (force local routing)
  const std::vector<std::string> pii_indicators = {
      "password", "ssn", "social security", "credit card",
      "bank account", "passport", "driver's license"
  };
  for (const auto& indicator : pii_indicators) {
    if (prompt.find(indicator) != std::string::npos) {
      return RouteTarget::LOCAL;
    }
  }

  // Check prompt length/complexity for routing
  if (prompt.length() > 2000) {
    return RouteTarget::CLOUD;  // Complex prompts may need larger models
  }

  // Default: prefer local inference
  return RouteTarget::LOCAL;
}

std::string BrowserAIRuntime::SelectModelForTask(
    const std::string& task_type) const {
  // Task-to-model routing from V4 spec:
  // Research → Qwen
  // Reasoning → LLaMA
  // Summarization → small fast model (TinyLlama/Phi)
  // Code → LLaMA or Qwen

  if (task_type == "summarization" || task_type == "quick") {
    // Prefer smallest loaded model
    if (impl_->models.count("tinyllama-1.1b") &&
        impl_->models.at("tinyllama-1.1b").is_loaded) {
      return "tinyllama-1.1b";
    }
    if (impl_->models.count("phi-3.5-mini") &&
        impl_->models.at("phi-3.5-mini").is_loaded) {
      return "phi-3.5-mini";
    }
  }

  if (task_type == "research") {
    if (impl_->models.count("qwen2.5-7b") &&
        impl_->models.at("qwen2.5-7b").is_loaded) {
      return "qwen2.5-7b";
    }
  }

  if (task_type == "reasoning" || task_type == "code") {
    if (impl_->models.count("llama3.1-8b") &&
        impl_->models.at("llama3.1-8b").is_loaded) {
      return "llama3.1-8b";
    }
  }

  // Fallback: return whatever is loaded
  return impl_->active_model_id;
}

PageContext BrowserAIRuntime::GetPageContext(int tab_id) const {
  // TODO: Integration with Chromium's content layer
  // Will use RenderFrameHost to extract DOM content
  PageContext ctx;
  ctx.url = "";
  ctx.title = "";
  return ctx;
}

GenerationResult BrowserAIRuntime::SummarizePage(int tab_id,
                                                   const PromptOptions& options) {
  PageContext context = GetPageContext(tab_id);

  std::string prompt = "Summarize the following web page:\n\n"
                       "Title: " + context.title + "\n"
                       "URL: " + context.url + "\n\n"
                       "Content:\n" + context.main_content;

  PromptOptions opts = options;
  if (opts.model_id.empty()) {
    opts.model_id = SelectModelForTask("summarization");
  }

  return RunPrompt(prompt, opts);
}

std::string BrowserAIRuntime::ExtractData(int tab_id,
                                           const std::string& selector) {
  // TODO: Use DOMInterpreter to extract structured data
  return "{}";
}

HardwareCapability BrowserAIRuntime::GetHardwareCapability() const {
  return impl_->hardware;
}

bool BrowserAIRuntime::CanRunModel(const std::string& model_id) const {
  auto it = impl_->models.find(model_id);
  if (it == impl_->models.end()) {
    return false;
  }

  const ModelInfo& info = it->second;

  // ResourceGovernor: model cannot exceed 50% of system RAM
  size_t max_allowed = static_cast<size_t>(
      impl_->hardware.total_ram_bytes * kMaxRamUsagePercent);

  return info.ram_required_bytes <= max_allowed;
}

size_t BrowserAIRuntime::GetModelMemoryUsage() const {
  size_t total = 0;
  for (const auto& [id, info] : impl_->models) {
    if (info.is_loaded) {
      total += info.ram_required_bytes;
    }
  }
  return total;
}

}  // namespace molt_ai
