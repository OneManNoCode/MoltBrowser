// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>  // for int32_t/int64_t — required on Linux under -fmodules
#include <cstdio>   // for sscanf
#include <cstdlib>  // for getenv
#include <iostream>
#include <mutex>
#include <ratio>
#include <thread>
#include <vector>

// Chromium includes
#include "base/memory/raw_ptr_exclusion.h"

// llama.cpp headers — wired in Day 4
#include "third_party/llama_cpp/include/llama.h"

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

// Inference defaults
constexpr int kDefaultContextSize = 2048;
constexpr int kMaxTokenBuffer = 8192;

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

// Helper: convert a single token to a string piece
std::string TokenToPiece(const llama_vocab* vocab, llama_token token) {
  char buf[256];
  int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
  if (n < 0) {
    // Buffer too small — allocate larger
    std::vector<char> large_buf(static_cast<size_t>(-n));
    n = llama_token_to_piece(vocab, token, large_buf.data(),
                             static_cast<int32_t>(large_buf.size()), 0, false);
    if (n > 0) {
      return std::string(large_buf.data(), static_cast<size_t>(n));
    }
    return "";
  }
  return std::string(buf, static_cast<size_t>(n));
}

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

  // llama.cpp context handles — C library types, excluded from raw_ptr check
  RAW_PTR_EXCLUSION llama_model* llama_model_handle = nullptr;
  RAW_PTR_EXCLUSION llama_context* llama_ctx = nullptr;
  std::string loaded_model_id;  // Track which model is currently loaded in llama

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

      // Set local file path. is_downloaded is left false here and updated
      // by RefreshModelStatus() on a worker thread to keep Initialize()
      // non-blocking on the UI thread.
      std::string model_path = model_directory + "/" + entry.filename;
      info.file_path = model_path;
      info.is_downloaded = false;
      info.is_loaded = false;

      models[entry.model_id] = info;
    }
  }

  // Free llama.cpp resources for the currently loaded model
  void FreeLlamaResources() {
    if (llama_ctx) {
      llama_free(llama_ctx);
      llama_ctx = nullptr;
    }
    if (llama_model_handle) {
      llama_model_free(llama_model_handle);
      llama_model_handle = nullptr;
    }
    loaded_model_id.clear();
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

  // Initialize() is invoked from the UI thread by the chat WebUI handler.
  // Keep this method non-blocking — Chromium's hang watchdog DCHECKs any
  // filesystem I/O on the UI thread. Filesystem work (mkdir, stat()s) is
  // deferred to RefreshModelStatus(), which runs on a worker thread before
  // any model is loaded or downloaded.

  // llama.cpp requires a one-time global backend init before any model load.
  // Registers compiled-in backends (Metal on Apple Silicon, CPU elsewhere).
  // This does NOT do filesystem I/O — Metal device probing only.
  static std::once_flag llama_backend_once;
  std::call_once(llama_backend_once, []() {
    llama_backend_init();
    std::cerr << "[MoltAI] llama.cpp backend initialized" << std::endl;
  });

  // Detect hardware capabilities (sysctl/sysfs reads — non-blocking)
  impl_->DetectHardware();

  // Set up model directory string. Directory creation deferred to first
  // download (see HandleDownloadModel) or RefreshModelStatus() on worker.
  const char* home = getenv("HOME");
  if (home) {
    impl_->model_directory = std::string(home) + "/.moltbrowser/models";
  } else {
    impl_->model_directory = "/tmp/moltbrowser/models";
  }

  // Initialize model registry without checking disk. is_downloaded starts
  // false; RefreshModelStatus() updates it from a worker thread.
  impl_->InitializeModelRegistry();

  impl_->initialized = true;
  return true;
}

void BrowserAIRuntime::Shutdown() {
  if (!impl_->initialized) return;

  CancelGeneration();

  // Free llama.cpp resources
  impl_->FreeLlamaResources();

  // Mark all models as unloaded
  for (auto& [id, info] : impl_->models) {
    info.is_loaded = false;
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

  if (info.is_loaded && impl_->loaded_model_id == model_id) {
    return true;  // Already loaded
  }

  // Check if we have enough RAM
  if (!CanRunModel(model_id)) {
    return false;
  }

  // If a different model is loaded, unload it first
  if (impl_->llama_model_handle && impl_->loaded_model_id != model_id) {
    UnloadModel(impl_->loaded_model_id);
  }

  // ---- llama.cpp model loading ----
  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = -1;  // Offload all layers to GPU (Metal on macOS)

  impl_->llama_model_handle = llama_model_load_from_file(
      info.file_path.c_str(), model_params);

  if (!impl_->llama_model_handle) {
    std::cerr << "[MoltAI] Failed to load model: " << info.file_path
              << std::endl;
    return false;
  }

  // Create inference context
  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = kDefaultContextSize;
  // n_batch MUST be >= the longest prompt decoded in a single llama_decode()
  // call. The system prompt alone tokenises to ~800 tokens, so n_batch=512
  // caused GGML_ASSERT(n_tokens_all <= cparams.n_batch) to fire → SIGABRT.
  // Set n_batch = n_ctx so the full context always fits in one decode pass.
  ctx_params.n_batch = kDefaultContextSize;
  ctx_params.n_threads = impl_->hardware.cpu_cores > 4
                             ? impl_->hardware.cpu_cores / 2
                             : impl_->hardware.cpu_cores;
  ctx_params.n_threads_batch = ctx_params.n_threads;

  impl_->llama_ctx = llama_init_from_model(
      impl_->llama_model_handle, ctx_params);

  if (!impl_->llama_ctx) {
    std::cerr << "[MoltAI] Failed to create context for: " << model_id
              << std::endl;
    llama_model_free(impl_->llama_model_handle);
    impl_->llama_model_handle = nullptr;
    return false;
  }

  info.is_loaded = true;
  impl_->active_model_id = model_id;
  impl_->loaded_model_id = model_id;

  std::cerr << "[MoltAI] Model loaded: " << info.display_name
            << " (ctx=" << llama_n_ctx(impl_->llama_ctx) << ")" << std::endl;
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

  // Free llama.cpp resources if this is the loaded model
  if (impl_->loaded_model_id == model_id) {
    impl_->FreeLlamaResources();
  }

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

  // TODO: Implement HuggingFace Hub download via Chromium network stack
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

  if (!impl_->llama_model_handle || !impl_->llama_ctx) {
    result.error_message = "llama.cpp context not initialized";
    return result;
  }

  impl_->is_generating = true;
  impl_->cancel_requested = false;

  auto start = std::chrono::steady_clock::now();

  // Get vocabulary from model
  const llama_vocab* vocab = llama_model_get_vocab(impl_->llama_model_handle);
  if (!vocab) {
    result.error_message = "Failed to get vocabulary from model";
    impl_->is_generating = false;
    return result;
  }

  // Build the full prompt with system prompt
  std::string full_prompt;
  if (!options.system_prompt.empty()) {
    full_prompt = options.system_prompt + "\n\n" + prompt;
  } else {
    full_prompt = prompt;
  }

  // ---- Step 1: Tokenize the prompt ----
  std::vector<llama_token> tokens(full_prompt.length() + 256);
  int n_tokens = llama_tokenize(
      vocab,
      full_prompt.c_str(),
      static_cast<int32_t>(full_prompt.length()),
      tokens.data(),
      static_cast<int32_t>(tokens.size()),
      true,   // add_special (BOS token)
      false); // parse_special

  if (n_tokens < 0) {
    // Need more space
    tokens.resize(static_cast<size_t>(-n_tokens));
    n_tokens = llama_tokenize(
        vocab,
        full_prompt.c_str(),
        static_cast<int32_t>(full_prompt.length()),
        tokens.data(),
        static_cast<int32_t>(tokens.size()),
        true,
        false);
  }

  if (n_tokens <= 0) {
    result.error_message = "Failed to tokenize prompt";
    impl_->is_generating = false;
    return result;
  }

  tokens.resize(static_cast<size_t>(n_tokens));
  result.tokens_prompt = n_tokens;

  // Check context length
  int n_ctx = static_cast<int>(llama_n_ctx(impl_->llama_ctx));
  if (n_tokens + options.max_tokens > n_ctx) {
    result.error_message = "Prompt + max_tokens exceeds context length";
    impl_->is_generating = false;
    return result;
  }

  // ---- Step 2: Evaluate prompt tokens ----
  llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
  if (llama_decode(impl_->llama_ctx, batch) != 0) {
    result.error_message = "Failed to evaluate prompt";
    impl_->is_generating = false;
    return result;
  }

  // ---- Step 3: Set up sampler ----
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  llama_sampler* sampler = llama_sampler_chain_init(sparams);

  // Add sampling layers: top-k → top-p → temperature → dist
  llama_sampler_chain_add(sampler, llama_sampler_init_top_k(options.top_k));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_top_p(options.top_p, 1));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_temp(options.temperature));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

  // ---- Step 4: Generate tokens ----
  std::string generated_text;
  int n_generated = 0;
  llama_token eos_token = llama_vocab_eos(vocab);

  for (int i = 0; i < options.max_tokens; ++i) {
    // Check for cancellation
    if (impl_->cancel_requested) {
      result.was_cancelled = true;
      break;
    }

    // Sample next token
    llama_token new_token = llama_sampler_sample(
        sampler, impl_->llama_ctx, -1);

    // Accept token in sampler
    llama_sampler_accept(sampler, new_token);

    // Check for end of sequence
    if (new_token == eos_token) {
      break;
    }

    // Convert token to text
    std::string piece = TokenToPiece(vocab, new_token);
    generated_text += piece;
    n_generated++;

    // Decode single token for next iteration
    llama_batch single_batch = llama_batch_get_one(&new_token, 1);
    if (llama_decode(impl_->llama_ctx, single_batch) != 0) {
      result.error_message = "Failed during generation at token " +
                             std::to_string(i);
      break;
    }
  }

  // Cleanup sampler
  llama_sampler_free(sampler);

  // Clear the KV cache for the next prompt
  // (simple approach — for production, we'd manage KV cache more carefully)
  llama_memory_t mem = llama_get_memory(impl_->llama_ctx);
  if (mem) {
    llama_memory_clear(mem, true);
  }

  auto end = std::chrono::steady_clock::now();

  result.text = generated_text;
  result.model_used = model_id;
  result.tokens_generated = n_generated;
  result.generation_time_ms =
      std::chrono::duration<float, std::milli>(end - start).count();
  result.success = !generated_text.empty() || result.was_cancelled;

  impl_->is_generating = false;
  return result;
}

void BrowserAIRuntime::StreamPrompt(const std::string& prompt,
                                     TokenCallback callback,
                                     const PromptOptions& options) {
  // Streaming inference — generates tokens one at a time and calls callback
  // This runs synchronously; for async streaming, the caller should
  // invoke this on a background thread.

  std::lock_guard<std::mutex> lock(impl_->generation_mutex);

  // Select model
  std::string model_id = options.model_id;
  if (model_id.empty()) {
    model_id = impl_->active_model_id;
  }
  if (model_id.empty() || !impl_->llama_model_handle || !impl_->llama_ctx) {
    if (callback) callback("[Error: No model loaded]", true);
    return;
  }

  impl_->is_generating = true;
  impl_->cancel_requested = false;

  const llama_vocab* vocab = llama_model_get_vocab(impl_->llama_model_handle);
  if (!vocab) {
    if (callback) callback("[Error: No vocabulary]", true);
    impl_->is_generating = false;
    return;
  }

  // Build full prompt
  std::string full_prompt;
  if (!options.system_prompt.empty()) {
    full_prompt = options.system_prompt + "\n\n" + prompt;
  } else {
    full_prompt = prompt;
  }

  // Tokenize
  std::vector<llama_token> tokens(full_prompt.length() + 256);
  int n_tokens = llama_tokenize(
      vocab, full_prompt.c_str(),
      static_cast<int32_t>(full_prompt.length()),
      tokens.data(), static_cast<int32_t>(tokens.size()),
      true, false);

  if (n_tokens < 0) {
    tokens.resize(static_cast<size_t>(-n_tokens));
    n_tokens = llama_tokenize(
        vocab, full_prompt.c_str(),
        static_cast<int32_t>(full_prompt.length()),
        tokens.data(), static_cast<int32_t>(tokens.size()),
        true, false);
  }

  if (n_tokens <= 0) {
    if (callback) callback("[Error: Tokenization failed]", true);
    impl_->is_generating = false;
    return;
  }
  tokens.resize(static_cast<size_t>(n_tokens));

  // Guard: truncate from the front if prompt exceeds context window.
  // Context size is fixed at kDefaultContextSize (2048) tokens. Without this
  // guard, llama_decode() calls ggml_abort() and the process dies with SIGABRT.
  int n_ctx = static_cast<int>(llama_n_ctx(impl_->llama_ctx));
  int max_prompt_tokens = n_ctx - options.max_tokens - 4;  // 4 safety margin
  if (max_prompt_tokens < 1) max_prompt_tokens = 1;
  if (n_tokens > max_prompt_tokens) {
    // Keep the tail of the token list (most recent context)
    int drop = n_tokens - max_prompt_tokens;
    tokens.erase(tokens.begin(), tokens.begin() + drop);
    n_tokens = static_cast<int>(tokens.size());
    std::cerr << "[MoltAI] StreamPrompt: prompt truncated by " << drop
              << " tokens to fit context window" << std::endl;
  }

  // CRITICAL: Clear the KV cache BEFORE each inference run.
  // The cache is also cleared at the end, but if a previous run exited early
  // (cancellation, error, or crash recovery), the cache may hold stale tokens.
  // Starting a new decode on a dirty cache shifts the effective sequence
  // position forward, and the combined length can exceed n_ctx → ggml_abort.
  {
    llama_memory_t mem = llama_get_memory(impl_->llama_ctx);
    if (mem) {
      llama_memory_clear(mem, true);
      std::cerr << "[MoltAI] StreamPrompt: KV cache cleared before inference"
                << std::endl;
    }
  }

  // Evaluate prompt
  llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
  if (llama_decode(impl_->llama_ctx, batch) != 0) {
    if (callback) callback("[Error: Prompt evaluation failed]", true);
    impl_->is_generating = false;
    return;
  }

  // Set up sampler
  llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
  llama_sampler* sampler = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(sampler, llama_sampler_init_top_k(options.top_k));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_top_p(options.top_p, 1));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_temp(options.temperature));
  llama_sampler_chain_add(sampler,
                          llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

  llama_token eos_token = llama_vocab_eos(vocab);

  // Hard-cap the generation budget so n_tokens + generated never reaches n_ctx.
  // options.max_tokens is user-configurable (settings.json) and could exceed
  // what's safe. This ensures the loop exits before the KV cache is full,
  // making ggml_abort in llama_decode provably impossible.
  int max_gen = std::min(options.max_tokens, n_ctx - n_tokens - 1);
  if (max_gen < 1) max_gen = 1;
  std::cerr << "[MoltAI] StreamPrompt: n_ctx=" << n_ctx
            << " n_prompt=" << n_tokens
            << " max_gen=" << max_gen << std::endl;

  // Stream tokens
  for (int i = 0; i < max_gen; ++i) {
    if (impl_->cancel_requested) {
      if (callback) callback("", true);
      break;
    }

    llama_token new_token = llama_sampler_sample(
        sampler, impl_->llama_ctx, -1);
    llama_sampler_accept(sampler, new_token);

    if (new_token == eos_token) {
      if (callback) callback("", true);
      break;
    }

    std::string piece = TokenToPiece(vocab, new_token);
    if (callback) callback(piece, false);

    llama_batch single_batch = llama_batch_get_one(&new_token, 1);
    if (llama_decode(impl_->llama_ctx, single_batch) != 0) {
      if (callback) callback("[Error: decode failed]", true);
      break;
    }
  }

  llama_sampler_free(sampler);

  // Clear KV cache for next prompt
  llama_memory_t mem = llama_get_memory(impl_->llama_ctx);
  if (mem) {
    llama_memory_clear(mem, true);
  }

  impl_->is_generating = false;
}

void BrowserAIRuntime::CancelGeneration() {
  impl_->cancel_requested = true;
}

RouteTarget BrowserAIRuntime::RoutePrompt(const std::string& prompt,
                                           const PageContext& context) const {
  // Routing decision matrix from V4 spec:
  // - Page summarization -> LOCAL
  // - Q&A about page -> LOCAL
  // - Agent tasks -> LOCAL
  // - Form fill -> LOCAL
  // - Complex reasoning -> CLOUD
  // - Code generation -> CLOUD
  // - AI Council debate -> HYBRID
  // - Offline -> LOCAL
  //
  // HARD RULE: If PII detected in prompt -> force LOCAL

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
  // Research -> Qwen
  // Reasoning -> LLaMA
  // Summarization -> small fast model (TinyLlama/Phi)
  // Code -> LLaMA or Qwen

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

void BrowserAIRuntime::RefreshModelStatus() {
  for (auto& [id, info] : impl_->models) {
    info.is_downloaded = std::filesystem::exists(info.file_path);
  }
}

}  // namespace molt_ai
