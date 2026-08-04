// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_
#define MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace molt_ai {

// Hardware capability detection for model selection
struct HardwareCapability {
  size_t total_ram_bytes;
  size_t available_ram_bytes;
  size_t gpu_vram_bytes;
  std::string gpu_backend;  // "metal", "vulkan", "directx", "cuda", "none"
  int cpu_cores;
  bool has_gpu_acceleration;
  std::string architecture;  // "arm64", "x86_64"
};

// Model information
struct ModelInfo {
  std::string model_id;
  std::string display_name;
  std::string file_path;      // Local GGUF file path
  std::string huggingface_id; // HuggingFace repo for download
  size_t file_size_bytes;
  size_t ram_required_bytes;
  int parameter_count_billions;
  std::string quantization;   // "Q4_K_M", "Q5_K_M", etc.
  bool is_downloaded;
  bool is_loaded;
  std::string company;        // Model maker for attribution, e.g. "Meta",
                              // "Alibaba", "Google", "Mistral AI", "Microsoft",
                              // "OpenAI". Drives the brand icon in the picker.
};

// Prompt routing decision
enum class RouteTarget {
  LOCAL,    // Route to local LLM
  CLOUD,    // Route to cloud API
  HYBRID,   // Use both local and cloud
  OFFLINE   // Force local (no network)
};

// Token streaming callback
using TokenCallback = std::function<void(const std::string& token, bool is_done)>;

// A single chat turn for template-aware prompting (StreamChat /
// ApplyChatTemplate). role is "system", "user" or "assistant".
struct ChatMessage {
  std::string role;
  std::string content;
};

// Prompt options
struct PromptOptions {
  std::string model_id;          // Specific model to use, or empty for auto
  int max_tokens = 512;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  std::string system_prompt;
  std::string persona_id;        // Apply persona context
  bool stream = true;
  RouteTarget route = RouteTarget::LOCAL;
  int timeout_ms = 30000;
};

// Generation result
struct GenerationResult {
  std::string text;
  std::string model_used;
  int tokens_generated;
  int tokens_prompt;
  float generation_time_ms;
  bool was_cancelled;
  bool success;
  std::string error_message;
};

// Page context for AI reasoning
struct PageContext {
  std::string url;
  std::string title;
  std::string main_content;
  std::vector<std::string> headings;
  std::vector<std::string> links;
  std::vector<std::string> buttons;
  std::string selected_text;
  std::string meta_description;
};

// ============================================================
// BrowserAIRuntime — Core AI orchestration engine
// ============================================================
// This is the central class that manages all AI operations
// within MoltBrowser. It handles model loading, prompt routing,
// token streaming, and integration with the browser's UI layer.
//
// Architecture:
//   Browser UI → BrowserAIRuntime → PromptRouter → llama.cpp
//                                 → ModelManager
//                                 → PersonaSystem
//                                 → MemoryEngine
// ============================================================
class BrowserAIRuntime {
 public:
  BrowserAIRuntime();
  ~BrowserAIRuntime();

  // Disallow copy/assign
  BrowserAIRuntime(const BrowserAIRuntime&) = delete;
  BrowserAIRuntime& operator=(const BrowserAIRuntime&) = delete;

  // Process-wide singleton. The on-device model is a global resource — only one
  // fits in RAM at a time — and model load / token generation run on background
  // ThreadPool tasks that capture a raw pointer to the runtime. Owning it
  // per-WebUI-page meant that closing the page (side-panel close, tab close,
  // AI-chat <-> Agent-mode swap) while a load was in flight freed the runtime
  // out from under the running task → use-after-free crash of the whole
  // browser process. This instance lives for the process lifetime, so every
  // captured pointer stays valid. All callers must route through here rather
  // than constructing their own instance.
  static BrowserAIRuntime& GetInstance();

  // ---- Lifecycle ----

  // Initialize the runtime with hardware detection
  bool Initialize();

  // Shutdown and cleanup all loaded models
  void Shutdown();

  // ---- Model Management ----

  // Load a model from local GGUF file into memory
  // Returns true if model loaded successfully
  bool LoadModel(const std::string& model_id);

  // Unload a model from memory to free resources
  bool UnloadModel(const std::string& model_id);

  // Get list of all available models (downloaded + available for download)
  std::vector<ModelInfo> GetAvailableModels() const;

  // Get info about a specific model
  ModelInfo GetModelInfo(const std::string& model_id) const;

  // Download a model from HuggingFace Hub
  // Progress callback receives (bytes_downloaded, total_bytes)
  bool DownloadModel(
      const std::string& model_id,
      std::function<void(size_t, size_t)> progress_callback = nullptr);

  // Delete a downloaded model from local storage
  bool DeleteModel(const std::string& model_id);

  // ---- Inference ----

  // Run a prompt synchronously and return the full result
  GenerationResult RunPrompt(const std::string& prompt,
                             const PromptOptions& options = {});

  // Run a prompt with streaming token output
  void StreamPrompt(const std::string& prompt,
                    TokenCallback callback,
                    const PromptOptions& options = {});

  // Format |messages| with the loaded model's own chat template (the
  // GGUF's tokenizer.chat_template, via llama_chat_apply_template).
  // Falls back to llama.cpp's built-in "chatml" format when the model
  // ships no template or its template is unrecognized. When
  // |add_assistant| is true, the assistant-turn start marker is
  // appended so the model continues as the assistant.
  std::string ApplyChatTemplate(const std::vector<ChatMessage>& messages,
                                bool add_assistant) const;

  // Chat-aware streaming inference: applies the loaded model's chat
  // template to |messages| (system + history + final user turn) and
  // streams the reply. Unlike StreamPrompt, the template's control
  // markers are tokenized as real special tokens and generation stops
  // on ANY end-of-generation token (llama_vocab_is_eog), so template
  // or EOS text never leaks into the token stream. Model-agnostic:
  // works for zephyr/chatml/llama3/phi/gemma-style templates alike.
  void StreamChat(const std::vector<ChatMessage>& messages,
                  TokenCallback callback,
                  const PromptOptions& options = {});

  // Cancel an in-progress generation
  void CancelGeneration();

  // ---- Routing ----

  // Determine the best route for a given prompt
  RouteTarget RoutePrompt(const std::string& prompt,
                          const PageContext& context) const;

  // Select the best model for a given task type
  std::string SelectModelForTask(const std::string& task_type) const;

  // ---- Page Intelligence ----

  // Get structured context from the current page
  PageContext GetPageContext(int tab_id) const;

  // Generate a summary of the current page
  GenerationResult SummarizePage(int tab_id,
                                 const PromptOptions& options = {});

  // Extract structured data from page elements
  std::string ExtractData(int tab_id, const std::string& selector);

  // ---- Hardware ----

  // Get current hardware capabilities
  HardwareCapability GetHardwareCapability() const;

  // Check if a model can run on current hardware
  bool CanRunModel(const std::string& model_id) const;

  // Get current memory usage of all loaded models
  size_t GetModelMemoryUsage() const;

  // Refresh model download status (re-check file existence on disk)
  void RefreshModelStatus();

 private:
  // Shared streaming core behind StreamPrompt (parse_special=false,
  // legacy raw-prompt callers) and StreamChat (parse_special=true so
  // templated control markers become real special tokens).
  // StreamWithPrompt acquires generation_mutex then delegates to the *Locked
  // core. StreamChat instead takes generation_mutex ITSELF and calls the
  // *Locked core directly, so the chat-template build (which reads the model
  // handle) and the decode happen under one continuous lock — a concurrent
  // LoadModel can't free the model between them.
  void StreamWithPrompt(const std::string& full_prompt,
                        bool parse_special,
                        TokenCallback callback,
                        const PromptOptions& options);
  // Caller MUST hold generation_mutex.
  void StreamWithPromptLocked(const std::string& full_prompt,
                              bool parse_special,
                              TokenCallback callback,
                              const PromptOptions& options);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_
