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
// Only std, llama.h and its exception guard (third_party/llama_cpp/chromium/
// llama_guard.h): no //base and no mojo, so it is also built and run
// standalone against the vendored llama.cpp. Not thread-safe; the service
// owns it on one sequence (MoltModelImpl, through base::SequenceBound) and
// only the cancel flag behind EngineCallbacks::is_cancelled is read across
// threads.
//
// Image input (section 10 of the design): with a vision projector loaded
// (a second GGUF, through llama.cpp's multimodal library), user turns may
// carry images as raw RGB pixels within engine/image_input.h's bounds.
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

#include "chrome/services/molt_ai/inference/engine/cpu_kernels.h"

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
  // Size of the vision projector file passed to Load() (0 = none); a file of
  // another size is refused.
  uint64_t vision_projector_size = 0;
  // This process's limit on anonymous memory (RLIMIT_DATA on Linux, the job
  // memory limit on Windows); 0 = none known (macOS).
  uint64_t memory_limit_bytes = 0;
  // Try the GPU through llama.cpp's Vulkan backend (Linux and Windows builds
  // with molt_llama_vulkan; design section 11). The CPU is the fallback.
  // macOS ignores it: Metal is always used there.
  bool use_gpu = false;
};

// What became of the GPU (Vulkan) for this load. Mirrors mojom GpuStatus.
enum class EngineGpuStatus {
  // The browser didn't ask for it (and on macOS, where Metal is used).
  kNotRequested,
  // This build has no Vulkan backend.
  kNotBuilt,
  // No Vulkan loader, no instance, or no GPU llama.cpp's backend accepts.
  kNoDevice,
  // The device policy refused every GPU (engine/gpu_offload.h).
  kNoUsableDevice,
  // Too little of the model fits in the GPU's free memory.
  kTooLittleMemory,
  // The model, its projector or its context couldn't be loaded on the GPU;
  // it was loaded on the CPU instead.
  kLoadFailed,
  // The model runs on the GPU, wholly or partly (gpu_layers).
  kOn,
};

enum class EngineLoadStatus {
  kOk,
  kInvalidModelFile,
  kOutOfMemory,
  kBackendUnavailable,
  // An x86-64 CPU below every kernel level llama.cpp is built for
  // (x86-64-v2: SSE4.2, POPCNT; design section 12).
  kUnsupportedCpu,
  // The vision projector isn't one llama.cpp's multimodal library can load
  // for this model (wrong size, not a projector, made for another model).
  kInvalidVisionProjector,
};

struct EngineModelDescription {
  std::string architecture;
  std::string backend;
  uint32_t context_tokens = 0;
  uint32_t train_context_tokens = 0;
  uint32_t vocab_size = 0;
  bool cpu_repack_used = false;
  // ggml's x86-64 CPU kernel level ("x86-64-v2", "avx", "avx2"); "" on other
  // CPUs (design section 12).
  std::string cpu_kernels;
  // A vision projector is loaded: user turns may carry images.
  bool vision = false;
  EngineGpuStatus gpu_status = EngineGpuStatus::kNotRequested;
  // Layers on the Vulkan GPU, the output layer counting as one (0 = none).
  uint32_t gpu_layers = 0;
};

struct EngineLoadResult {
  EngineLoadStatus status = EngineLoadStatus::kInvalidModelFile;
  EngineModelDescription description;
};

// An image for the model: 8-bit RGB, row-major, no row padding, decoded and
// bounded by the browser (engine/image_input.h); Chat() checks it again.
// Untrusted content, like page text.
struct EngineImage {
  EngineImage();
  EngineImage(uint32_t width, uint32_t height, std::vector<uint8_t> rgb);
  ~EngineImage();
  EngineImage(EngineImage&&);
  EngineImage& operator=(EngineImage&&);
  EngineImage(const EngineImage&);
  EngineImage& operator=(const EngineImage&);

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
};

struct EngineChatTurn {
  EngineChatTurn();
  EngineChatTurn(bool from_user, std::string content);
  ~EngineChatTurn();
  EngineChatTurn(EngineChatTurn&&);
  EngineChatTurn& operator=(EngineChatTurn&&);
  EngineChatTurn(const EngineChatTurn&);
  EngineChatTurn& operator=(const EngineChatTurn&);

  bool from_user = true;  // else the assistant
  std::string content;
  // A user turn's images, shown to the model before its text. Never on an
  // assistant turn.
  std::vector<EngineImage> images;
};

// See ChatPrompt (browser_ai_runtime.h) / mojom ChatRequest: context, history
// and user_message (and the images) are untrusted.
struct EngineChatRequest {
  EngineChatRequest();
  ~EngineChatRequest();
  EngineChatRequest(EngineChatRequest&&);
  EngineChatRequest& operator=(EngineChatRequest&&);
  EngineChatRequest(const EngineChatRequest&);
  EngineChatRequest& operator=(const EngineChatRequest&);

  std::string system_prompt;
  std::string context_preamble;
  std::string context;
  std::vector<EngineChatTurn> history;
  std::string user_message;
  // The new message's images, shown before its text. At most
  // kMaxImagesPerRequest images in the request, history included.
  std::vector<EngineImage> user_images;
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
  // Constrained decoding: GBNF (start rule "root") from
  // JsonSchemaToGrammar(), never from elsewhere; "" = none. The visible
  // answer then matches the grammar: reasoning is skipped (the prompt is
  // pre-filled past any reasoning block) and the reasoning filters don't
  // run, so the text is exactly what the grammar matched.
  std::string grammar;
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
  // llama.cpp refused the grammar, or no token of this vocabulary can
  // continue it.
  kConstraintFailed,
  // The request has images and no vision projector is loaded. Nothing was
  // generated.
  kImagesUnsupported,
  // The GPU failed under the model (device lost, a driver error): llama.cpp
  // threw (third_party/llama_cpp/chromium/llama_guard.h). The context can't
  // be used again; every later request fails the same way, and the browser
  // reloads the model on the CPU.
  kBackendFailed,
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

  // Whether this CPU can run llama.cpp's CPU kernels: on x86-64, at least
  // the x86-64-v2 level (SSE4.2, POPCNT; design section 12). True
  // elsewhere.
  static bool CpuSupported();

  // Selects ggml's x86-64 CPU kernels for this CPU (the best level it and
  // its OS support), then calls llama_backend_init() once per process.
  // False, with nothing initialized, if CpuSupported() is false. Load() and
  // LlamaEmbeddingEngine::Load() call it before anything else of llama.cpp.
  static bool InitBackendOnce();

  // The level InitBackendOnce() selected ("x86-64-v2", "avx", "avx2"); ""
  // when not built for x86-64.
  static std::string CpuKernelsName();

  // Tests only: from the next load on, run |kernels| instead of the best
  // level for this CPU (nullopt: back to that). The CPU must support it.
  static void SetCpuKernelsForTesting(std::optional<X86CpuKernels> kernels);

  // Tests only: lets the GPU device policy take software Vulkan drivers
  // (lavapipe, SwiftShader), which it always refuses otherwise, so the GPU
  // path can run on a machine without a GPU.
  static void SetAllowSoftwareGpuForTesting(bool allow);

  LlamaEngine();
  ~LlamaEngine();
  LlamaEngine(const LlamaEngine&) = delete;
  LlamaEngine& operator=(const LlamaEngine&) = delete;

  // Loads the GGUF read through |file| (read-only, synchronous) and creates
  // the context. |file| stays open until the model is freed (lazily read
  // tensors go through it); it is closed by the destructor, or here on
  // failure. With |vision_projector| (a second GGUF of
  // params.vision_projector_size bytes), also loads llama.cpp's multimodal
  // context for image input; that file is read completely here and closed.
  // Call at most once.
  EngineLoadResult Load(ScopedStdioFile file,
                        const EngineLoadParams& params,
                        ScopedStdioFile vision_projector = nullptr);
  bool loaded() const;
  // A vision projector is loaded.
  bool vision() const;

  // Chat with the model's own template (StreamChat). |callbacks| run on this
  // thread before Chat() returns. A request with images needs vision()
  // (kImagesUnsupported otherwise) and never reuses or leaves a reusable KV
  // prefix.
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
  struct MediaPrompt;

  EngineStatus Generate(const std::vector<int32_t>& prompt,
                        const std::string& reuse_key,
                        const DecodeParams& params,
                        const EngineCallbacks& callbacks,
                        EngineSummary* summary);
  // Evaluates a prompt with images into an empty KV cache. nullopt on
  // success, else why it stopped.
  std::optional<EngineStatus> EvalMediaPrompt(
      const MediaPrompt& media,
      const EngineCallbacks& callbacks);

  std::unique_ptr<Model> model_;
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_LLAMA_ENGINE_H_
