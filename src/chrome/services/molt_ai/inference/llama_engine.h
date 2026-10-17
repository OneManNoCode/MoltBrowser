// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// LlamaEngine: the one llama.cpp model + context of an inference service
// process, with everything that turns a request into tokens and tokens into
// visible text: the chat template, the context budget, special-token
// neutralization, KV-prefix reuse, the reasoning filters and stop sequences.
// Moved here from the browser's BrowserAIRuntime (S5 phase 2), so a GGUF,
// tokenizer or filter bug stays inside the sandbox.
//
// Only std and llama.h: no //base and no mojo, so it is also built and run
// standalone against the vendored llama.cpp. Not thread-safe; the service
// owns it on one sequence (MoltModelImpl, through base::SequenceBound) and
// only the cancel flag behind EngineCallbacks::is_cancelled is read across
// threads.
//
// Never logs prompt or response text (it doesn't log at all; llama.cpp's own
// log goes through the service's llama_log_set callback).

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_ENGINE_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_ENGINE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace molt_ai {

// Owns a stdio stream (fclose on destruction).
struct StdioFileCloser {
  void operator()(FILE* file) const;
};
using ScopedStdioFile = std::unique_ptr<FILE, StdioFileCloser>;

// Plain mirrors of the molt_inference.mojom types the engine needs.

struct EngineLoadParams {
  // Size the browser saw; a file of another size is refused.
  uint64_t file_size = 0;
  uint64_t total_ram_bytes = 0;
  uint32_t cpu_threads = 1;
  // Upper bound on n_ctx; 0 = none.
  uint32_t max_context_tokens = 0;
  bool allow_cpu_repack = true;
  // This process's limit on anonymous memory (RLIMIT_DATA on Linux, the job
  // memory limit on Windows); 0 = none known (macOS).
  uint64_t memory_limit_bytes = 0;
};

enum class EngineLoadStatus {
  kOk,
  kInvalidModelFile,
  kOutOfMemory,
  kBackendUnavailable,
};

struct EngineModelDescription {
  std::string architecture;
  std::string backend;
  uint32_t context_tokens = 0;
  uint32_t train_context_tokens = 0;
  uint32_t vocab_size = 0;
  bool cpu_repack_used = false;
};

struct EngineLoadResult {
  EngineLoadStatus status = EngineLoadStatus::kInvalidModelFile;
  EngineModelDescription description;
};

struct EngineChatTurn {
  bool from_user = true;  // else the assistant
  std::string content;
};

// See ChatPrompt (browser_ai_runtime.h) / mojom ChatRequest: context, history
// and user_message are untrusted.
struct EngineChatRequest {
  std::string system_prompt;
  std::string context_preamble;
  std::string context;
  std::vector<EngineChatTurn> history;
  std::string user_message;
};

struct EngineRawRequest {
  std::string system_prompt;
  std::string prompt;
};

struct EngineGenerationParams {
  uint32_t max_visible_tokens = 0;    // 0 = adaptive
  uint32_t max_reasoning_tokens = 0;  // 0 = adaptive
  bool reasoning_on = false;
  float temperature = 0.7f;
  float top_p = 0.9f;
  uint32_t top_k = 40;
  std::string conversation_key;
  std::vector<std::string> stop_sequences;
};

enum class EngineStatus {
  kEndOfGeneration,
  kMaxTokens,
  kStopSequence,
  kContextFull,
  kCancelled,
  kPromptTooLong,
  kSystemPromptTooLong,
  kTokenizeFailed,
  kDecodeFailed,
};

struct EngineSummary {
  EngineStatus status = EngineStatus::kDecodeFailed;
  uint32_t prompt_tokens = 0;
  uint32_t reused_tokens = 0;
  uint32_t generated_tokens = 0;  // visible + hidden
  uint32_t history_dropped = 0;   // history messages dropped to fit
  bool context_trimmed = false;
  bool user_message_trimmed = false;
};

// Called synchronously on the engine's thread during Chat() / Raw().
struct EngineCallbacks {
  EngineCallbacks();
  ~EngineCallbacks();
  EngineCallbacks(EngineCallbacks&&);
  EngineCallbacks& operator=(EngineCallbacks&&);

  // Polled before every prompt chunk and every sampled token. May be read
  // from another thread's flag.
  std::function<bool()> is_cancelled;
  // Progress while nothing visible is sent, at most every
  // kProgressIntervalMs: prompt evaluation (tokens evaluated / prompt size),
  // then hidden reasoning (reasoning tokens / budget). The browser uses it
  // as liveness.
  std::function<void(uint32_t done_tokens, uint32_t total_tokens)> on_progress;
  // Visible text: complete, valid UTF-8, never empty, at most kMaxChunkBytes.
  std::function<void(std::string text)> on_chunk;
};

class LlamaEngine {
 public:
  // Matches molt_ai.mojom.kMaxChunkBytes.
  static constexpr size_t kMaxChunkBytes = 4096;
  static constexpr int kProgressIntervalMs = 500;

  // llama_backend_init(), once per process. Load() calls it.
  static void InitBackendOnce();

  LlamaEngine();
  ~LlamaEngine();
  LlamaEngine(const LlamaEngine&) = delete;
  LlamaEngine& operator=(const LlamaEngine&) = delete;

  // Loads the GGUF read through |file| (read-only, synchronous) and creates
  // the context. |file| stays open until the model is freed (lazily read
  // tensors go through it); it is closed by the destructor, or here on
  // failure. Call at most once.
  EngineLoadResult Load(ScopedStdioFile file, const EngineLoadParams& params);
  bool loaded() const;

  // Chat with the model's own template (StreamChat). |callbacks| run on this
  // thread before Chat() returns.
  EngineSummary Chat(const EngineChatRequest& request,
                     const EngineGenerationParams& params,
                     const EngineCallbacks& callbacks);
  // Raw prompt without a chat template (StreamPrompt: agents, automation).
  EngineSummary Raw(const EngineRawRequest& request,
                    const EngineGenerationParams& params,
                    const EngineCallbacks& callbacks);

  // Plain-text tokens (no BOS, parse_special=false); nullopt on failure or
  // if |text| is longer than |max_text_bytes|.
  std::optional<std::vector<int32_t>> Tokenize(const std::string& text,
                                               size_t max_text_bytes) const;

  // Drops the cached KV prefix if it belongs to |conversation_key|.
  void ForgetConversation(const std::string& conversation_key);

 private:
  struct Model;
  struct DecodeParams;

  EngineStatus Generate(const std::vector<int32_t>& prompt,
                        const std::string& reuse_key,
                        const DecodeParams& params,
                        const EngineCallbacks& callbacks,
                        EngineSummary* summary);

  std::unique_ptr<Model> model_;
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_ENGINE_H_
