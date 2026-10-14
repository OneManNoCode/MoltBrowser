// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_
#define MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/time/time.h"

namespace molt_ai {

// Hardware capability detection for model selection
struct HardwareCapability {
  size_t total_ram_bytes = 0;
  // Snapshot taken at Initialize(); CheckRamForModel() samples it fresh.
  size_t available_ram_bytes = 0;
  size_t gpu_vram_bytes = 0;
  std::string gpu_backend;  // "metal", "vulkan", "directx", "cuda", "none"
  int cpu_cores = 0;
  bool has_gpu_acceleration = false;
  std::string architecture;  // "arm64", "x86_64"
};

// Model information
struct ModelInfo {
  std::string model_id;
  std::string display_name;
  std::string file_path;      // Local GGUF file path
  std::string huggingface_id; // HuggingFace repo for download
  std::string filename;       // GGUF file name inside the repo
  // Pinned Hugging Face commit. Empty = not pinned yet: downloads come from
  // "main" and are treated as unverified.
  std::string revision;
  // Expected SHA-256 (lowercase hex) of the GGUF. Empty = unverified
  // download (allowed, but flagged in the UI and logged). When set, the
  // download is hashed while streaming and rejected on mismatch.
  std::string sha256;
  // Non-empty when the model's terms must be accepted before download
  // (Llama / Gemma). license_name / license_url describe them.
  std::string license_id;
  std::string license_name;
  std::string license_url;
  size_t file_size_bytes = 0;  // exact when pinned (verified on download)
  size_t ram_required_bytes = 0;
  int parameter_count_billions = 0;
  std::string quantization;   // "Q4_K_M", "Q5_K_M", etc.
  bool is_downloaded = false;
  bool is_loaded = false;
  std::string company;        // Model maker for attribution, e.g. "Meta",
                              // "Alibaba", "Google", "Mistral AI", "Microsoft",
                              // "OpenAI". Drives the brand icon in the picker.

  bool is_verified() const { return !sha256.empty(); }
};

// Prompt routing decision
enum class RouteTarget {
  LOCAL,    // Route to local LLM
  CLOUD,    // Route to cloud API
  HYBRID,   // Use both local and cloud
  OFFLINE   // Force local (no network)
};

// Whether a reasoning-capable model should think before answering.
//  kOff:  ask it not to (Qwen3 "/no_think", gpt-oss "Reasoning: low");
//         any reasoning it still emits is hidden.
//  kOn:   let it reason (hidden from the user, bounded by
//         PromptOptions::max_reasoning_tokens).
enum class ReasoningMode { kOff, kOn };

// Token streaming callback. Called with visible text only (reasoning,
// Harmony analysis and template control markers are filtered out), and
// exactly once with is_done=true at the end. Errors are reported through
// the returned GenerationResult, not as streamed text.
using TokenCallback = std::function<void(const std::string& token, bool is_done)>;

// A single chat turn for template-aware prompting (StreamChat /
// ApplyChatTemplate). role is "system", "user" or "assistant".
struct ChatMessage {
  std::string role;
  std::string content;
};

// Structured chat request. The runtime owns the token budget, so it needs
// to know which parts may be trimmed when the conversation outgrows the
// context window:
//   system_prompt  never trimmed
//   history        oldest whole turns are dropped first
//   context        retrieved page / PDF / attachment / memory text, trimmed
//                  (keeping its head) after history
//   user_message   only its middle is cut, as a last resort
// The system message sent to the model is
//   system_prompt + context_preamble + context   (preamble only if context).
// context, history and user_message are treated as untrusted: text that
// spells a special token of the loaded model is neutralized.
struct ChatPrompt {
  std::string system_prompt;
  std::string context_preamble;
  std::string context;
  std::vector<ChatMessage> history;  // oldest first; "user" / "assistant"
  std::string user_message;
};

// Prompt options
struct PromptOptions {
  std::string model_id;          // Specific model to use, or empty for auto
  // Visible-answer token budget. <= 0 picks an adaptive default from the
  // context size. Hidden reasoning tokens don't count against it.
  int max_tokens = 512;
  // Hidden reasoning budget (<think> blocks, Harmony analysis). <= 0 picks
  // an adaptive default. When exhausted the model is forced to answer.
  int max_reasoning_tokens = 0;
  ReasoningMode reasoning = ReasoningMode::kOff;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  std::string system_prompt;
  std::string persona_id;        // Apply persona context
  bool stream = true;
  RouteTarget route = RouteTarget::LOCAL;
  int timeout_ms = 30000;
  // Per-request cancellation handle from NewRequestId(). 0 = the runtime
  // allocates a private one (only CancelGeneration() can stop it).
  uint64_t request_id = 0;
  // KV-cache reuse key: consecutive StreamChat calls with the same
  // non-empty key (one conversation) reuse the common token prefix of their
  // prompts instead of re-evaluating it. Empty = no reuse.
  std::string conversation_key;
};

// Generation result
struct GenerationResult {
  std::string text;
  std::string model_used;
  int tokens_generated = 0;
  int tokens_prompt = 0;
  // Prompt tokens served from the KV cache of the previous turn.
  int tokens_reused = 0;
  float generation_time_ms = 0.0f;
  bool was_cancelled = false;
  bool success = false;
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

// Result of the "can this model load right now?" RAM check.
struct RamCheck {
  bool ok = false;
  // Model larger than (most of) physical RAM: will never fit.
  bool exceeds_total = false;
  uint64_t needed_bytes = 0;     // weights + runtime overhead
  uint64_t available_bytes = 0;  // free now + what unloading the current
                                 // model would release
  uint64_t total_bytes = 0;
};

// ============================================================
// BrowserAIRuntime — Core AI orchestration engine
// ============================================================
// Owns the one on-device llama.cpp model/context, chat templating, the
// context-window budget, streaming output filtering and per-request
// cancellation.
//
// Threading: load and generation are blocking and run on ThreadPool
// workers. generation_mutex guards the llama handles during use;
// load_mutex serializes loads/unloads (see Impl).
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

  // Initialize the runtime with hardware detection. Does not call into
  // llama.cpp/ggml at all when the CPU lacks the instruction set the build
  // was compiled for (see GetLocalInferenceUnsupportedReason()).
  bool Initialize();

  // Shutdown and cleanup all loaded models
  void Shutdown();

  // Empty when on-device inference can run on this machine. Otherwise a
  // user-facing explanation, e.g. the x64 build uses AVX2/FMA and the CPU
  // lacks them (calling into ggml would crash the browser with SIGILL).
  std::string GetLocalInferenceUnsupportedReason() const;

  // Registers a critical-memory-pressure listener that unloads the model
  // when no generation is running. Must be called on the UI (main) thread;
  // idempotent.
  void StartMemoryPressureMonitoring();

  // Unload the model after it has been idle for |timeout| (0 = never).
  // Default 10 minutes.
  void SetIdleUnloadTimeout(base::TimeDelta timeout);

  // ---- Model Management ----

  // Load a model from local GGUF file into memory. Returns true if the
  // model is loaded. On failure |error| (optional) gets a user-facing
  // reason (not downloaded, not enough free memory, unsupported CPU,
  // unreadable file, ...).
  bool LoadModel(const std::string& model_id, std::string* error = nullptr);

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

  // Run a raw prompt synchronously and return the full result. A prompt
  // that doesn't fit the context window is shortened in the middle (the
  // system prompt is kept whole) instead of failing.
  GenerationResult RunPrompt(const std::string& prompt,
                             const PromptOptions& options = {});

  // Run a raw prompt with streaming token output
  GenerationResult StreamPrompt(const std::string& prompt,
                                TokenCallback callback,
                                const PromptOptions& options = {});

  // Format |messages| with the loaded model's own chat template (the
  // GGUF's tokenizer.chat_template, via llama_chat_apply_template).
  // Falls back to llama.cpp's built-in "chatml" format when the model
  // ships no template or its template is unrecognized. When
  // |add_assistant| is true, the assistant-turn start marker is
  // appended so the model continues as the assistant. Caller must hold
  // the generation lock (only called from StreamChat).
  std::string ApplyChatTemplate(const std::vector<ChatMessage>& messages,
                                bool add_assistant) const;

  // Chat-aware streaming inference: applies the loaded model's chat
  // template, fits the request into the context window (see ChatPrompt),
  // reuses the KV cache across turns of one conversation
  // (options.conversation_key) and streams the visible reply. Template
  // markers are tokenized as real special tokens; generation stops on ANY
  // end-of-generation token (llama_vocab_is_eog).
  GenerationResult StreamChat(const ChatPrompt& prompt,
                              TokenCallback callback,
                              const PromptOptions& options = {});

  // Legacy flat form: the first "system" message becomes the system
  // prompt, the last "user" message the new turn, the rest history.
  GenerationResult StreamChat(const std::vector<ChatMessage>& messages,
                              TokenCallback callback,
                              const PromptOptions& options = {});

  // ---- Cancellation ----

  // Allocates a cancellation handle for one request. A Stop that arrives
  // before the request starts generating (while the model loads or while
  // it waits for another generation) is remembered.
  uint64_t NewRequestId();
  // Cancels that request only.
  void CancelRequest(uint64_t request_id);
  bool IsRequestCancelled(uint64_t request_id) const;
  // Forget a request id once its owner no longer needs it.
  void FinishRequest(uint64_t request_id);
  // Cancels every in-flight request (shutdown / legacy callers).
  void CancelGeneration();

  // ---- Routing ----

  // Determine the best route for a given prompt
  RouteTarget RoutePrompt(const std::string& prompt,
                          const PageContext& context) const;

  // Select the best model for a given task type
  std::string SelectModelForTask(const std::string& task_type) const;

  // The id of the model currently resident in memory ("" if none). Takes
  // generation_mutex, so call it from a worker thread (it can briefly block
  // behind an in-flight decode), never the UI thread.
  std::string GetLoadedModelId() const;

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

  // Whether |model_id| fits in memory right now (free RAM plus what
  // unloading the current model would release). Samples available memory,
  // so call it off the UI thread.
  RamCheck CheckRamForModel(const std::string& model_id) const;
  bool CanRunModel(const std::string& model_id) const;

  // Get current memory usage of all loaded models
  size_t GetModelMemoryUsage() const;

  // Refresh model download status (re-check file existence on disk)
  void RefreshModelStatus();

 private:
  struct Impl;
  class PressureListener;

  // Called (on a worker) after a critical memory-pressure signal.
  void OnCriticalMemoryPressure();
  // Records activity and (re)arms the idle-unload check.
  void TouchActivity();
  void MaybeUnloadIdle(uint64_t epoch);

  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // MOLT_AI_RUNTIME_BROWSER_AI_RUNTIME_H_
