// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>  // for int32_t/int64_t — required on Linux under -fmodules
#include <cstdlib>  // for getenv
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Chromium includes
#include "base/cpu.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/system/sys_info.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/browser/molt_ai/runtime/inference_process_host.h"
#include "chrome/browser/molt_ai/runtime/stream_validator.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "chrome/services/molt_ai/inference/public/mojom/molt_inference.mojom.h"
#include "content/public/browser/browser_thread.h"

// No llama.cpp here: the model runs in the sandboxed inference service
// (//third_party/llama_cpp is visible only to chrome/services/molt_ai/
// inference).

namespace molt_ai {

namespace {

using Clock = std::chrono::steady_clock;

// Default idle time after which the model is unloaded to give RAM back.
constexpr base::TimeDelta kDefaultIdleUnload = base::Minutes(10);

// Runtime overhead (compute buffers, a small KV cache, allocator slack) on
// top of the GGUF weights when deciding whether a model fits right now.
constexpr uint64_t kRuntimeOverheadBytes = 1024ULL * 1024 * 1024;

// A model is never allowed to need more than this share of physical RAM.
constexpr double kMaxShareOfTotalRam = 0.9;

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

// Largest context the browser accepts (the RAM tiers of ComputeContextSizing
// top out here).
constexpr uint32_t kMaxContextTokens = 32768;

// How long a blocked caller waits beyond the host's own deadlines (load 10
// min, generation 15 min) before giving up on the UI thread.
constexpr base::TimeDelta kLoadWait = base::Minutes(11);
constexpr base::TimeDelta kGenerationWait = base::Minutes(16);
// An unload waits for running requests, then for the process to exit.
constexpr base::TimeDelta kUnloadWait = base::Minutes(16);

// The blocked worker wakes at least this often (cancellation, shutdown).
constexpr std::chrono::milliseconds kWaitSlice{250};
// Streamed text reaches the TokenCallback at most this often (about 30
// updates a second), so a fast or hostile service can't flood the UI.
constexpr std::chrono::milliseconds kDeliveryInterval{33};
// If the UI thread shows no sign of life for this long, a ping is posted;
// if the ping isn't answered within kUiTimeout, the UI thread is gone
// (shutdown without the app-terminating notification) and the caller
// returns instead of blocking ThreadPool shutdown.
constexpr std::chrono::seconds kHeartbeatInterval{5};
constexpr std::chrono::seconds kUiTimeout{15};
// After asking the host to cancel, how long to wait for it to confirm.
constexpr std::chrono::seconds kCancelConfirmWait{5};

// Model registry — free, open-weight GGUF models that run 100% on-device via
// llama.cpp (nothing is sent to the model's maker). Ordered small -> large.
// `company` drives the brand icon + maker label in the picker. NOTE: "Gemma"
// is used as the maker label (not "Google") to honor the zero-"Google"-string
// product rule.
//
// Supply-chain pinning (S13): every entry carries the Hugging Face commit
// (/*revision=*/), the GGUF's SHA-256 (/*sha256=*/) and its exact size
// (/*size=*/). Downloads come from /resolve/<revision>/ and are hashed while
// streaming; an entry WITH a hash is enforced (mismatch => rejected), an
// entry WITHOUT one is downloaded from "main" as "unverified" (UI badge + log
// warning).
// TODO(pin): run tools/models/pin_models.py --write from a machine that can
// reach huggingface.co to fill revision/sha256/size; the sandbox that wrote
// this could not.
struct ModelRegistryEntry {
  const char* model_id;
  const char* display_name;
  const char* huggingface_id;
  const char* filename;
  const char* revision;      // pinned HF commit; "" = unpinned ("main")
  const char* sha256;        // lowercase hex; "" = unverified
  size_t file_size_bytes;    // exact once pinned
  size_t ram_required_bytes;
  int param_billions;
  const char* quantization;
  const char* company;
  // Key into kLicenses: the terms the weights are distributed under. Shown
  // in the model picker; some must also be accepted before download.
  const char* license;
  // Hidden entries are not offered anywhere until they are pinned (repo,
  // revision and hash confirmed).
  bool hidden;
};

// ONE flagship per maker — the best free, open-weight, on-device model each
// company ships (not the whole family), so the picker stays uncluttered.
// Ordered small -> large. `company` drives the brand tile in the picker.
const ModelRegistryEntry kModelRegistry[] = {
    // TinyLlama — runs on any machine. Apache 2.0.
    {"tinyllama-1.1b",
     "TinyLlama 1.1B Chat",
     "TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF",
     "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/668788096ULL,  // ~0.6GB
     2147483648ULL,          // 2GB RAM
     1,
     "Q4_K_M",
     "TinyLlama",
     "apache-2.0",
     false},

    // Qwen (Alibaba) — Qwen3.5 4B. TODO(pin): the exact GGUF repo/file for
    // the 4B instruct model is unconfirmed; hidden until pinned. Needs the
    // vendored llama.cpp's "qwen35" architecture (present at 00de615).
    {"qwen3.5-4b",
     "Qwen3.5 4B",
     "Qwen/Qwen3.5-4B-Instruct-GGUF",  // TODO(pin): confirm repo name
     "Qwen3.5-4B-Instruct-Q4_K_M.gguf",  // TODO(pin): confirm file name
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/0ULL,
     6442450944ULL,          // 6GB RAM (conservative until measured)
     4,
     "Q4_K_M",
     "Qwen",
     "",  // TODO(pin): confirm the terms; until then "see the model card"
     true},

    // Gemma — Gemma 4 E4B. Hidden until pinned. Runtime support is in place:
    // the vendored llama.cpp (b11641) has the gemma4 architecture, and
    // llama_chat_apply_template() formats Gemma 4 chats (local llama-chat.cpp
    // patch, see third_party/llama_cpp/README.chromium); not yet run with
    // real Gemma 4 weights. TODO(pin): only the pin is missing: confirm the
    // GGUF repo and file name, then fill revision/sha256/size
    // (tools/models/pin_models.py --write); license terms to be confirmed.
    {"gemma4-e4b",
     "Gemma 4 E4B",
     "unsloth/gemma-4-E4B-it-GGUF",  // TODO(pin): confirm repo name
     "gemma-4-E4B-it-Q4_K_M.gguf",   // TODO(pin): confirm file name
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/0ULL,
     8589934592ULL,          // 8GB RAM (conservative until measured)
     4,
     "Q4_K_M",
     "Gemma",
     "gemma",
     true},

    // Meta — Llama 3.1 8B (open weights); proven all-rounder.
    {"llama3.1-8b",
     "Llama 3.1 8B Instruct",
     "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF",
     "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/4920739232ULL,  // ~4.6GB
     6442450944ULL,           // 6GB RAM
     8,
     "Q4_K_M",
     "Meta",
     "llama3.1",
     false},

    // Qwen (Alibaba) — Qwen3 8B, Apache 2.0; recommended default (agentic).
    {"qwen3-8b",
     "Qwen3 8B",
     "Qwen/Qwen3-8B-GGUF",
     "Qwen3-8B-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/5027783488ULL,  // ~4.7GB
     6442450944ULL,           // 6GB RAM
     8,
     "Q4_K_M",
     "Qwen",
     "apache-2.0",
     false},

    // Gemma (Google DeepMind) — Gemma 3 12B, open weights. Labeled "Gemma".
    {"gemma3-12b",
     "Gemma 3 12B",
     "unsloth/gemma-3-12b-it-GGUF",
     "gemma-3-12b-it-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/7300778336ULL,  // ~6.8GB
     10737418240ULL,          // 10GB RAM
     12,
     "Q4_K_M",
     "Gemma",
     "gemma",
     false},

    // Mistral AI — Mistral Nemo 12B (2407), Apache 2.0; 128k context.
    {"mistral-nemo-12b",
     "Mistral Nemo 12B",
     "bartowski/Mistral-Nemo-Instruct-2407-GGUF",
     "Mistral-Nemo-Instruct-2407-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/7477208192ULL,  // ~7.0GB
     10737418240ULL,          // 10GB RAM
     12,
     "Q4_K_M",
     "Mistral AI",
     "apache-2.0",
     false},

    // Microsoft — Phi-4 14B, MIT.
    {"phi-4",
     "Phi-4 14B",
     "bartowski/phi-4-GGUF",
     "phi-4-Q4_K_M.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/9053114816ULL,  // ~8.4GB
     12884901888ULL,          // 12GB RAM
     14,
     "Q4_K_M",
     "Microsoft",
     "mit",
     false},

    // OpenAI — gpt-oss 20B, Apache 2.0 open weights (native MXFP4 quant).
    {"gpt-oss-20b",
     "gpt-oss 20B",
     "ggml-org/gpt-oss-20b-GGUF",
     "gpt-oss-20b-MXFP4.gguf",
     /*revision=*/"",
     /*sha256=*/"",
     /*size=*/12109566624ULL,  // ~11.3GB
     17179869184ULL,           // 16GB RAM
     20,
     "MXFP4",
     "OpenAI",
     "apache-2.0",
     false},
};

constexpr size_t kModelRegistrySize =
    sizeof(kModelRegistry) / sizeof(kModelRegistry[0]);

// The terms each model's weights are distributed under, shown in the model
// picker (A13). Those with requires_acceptance must also be accepted (once
// per license, recorded per model) before downloading; their ids are what
// accepted_licenses.json stores, so they must not change. The links avoid
// vendor domains that would break the zero-"Google"-string product rule;
// the Hugging Face model card shows the full Gemma terms.
struct LicenseEntry {
  const char* id;
  const char* name;
  const char* url;
  bool requires_acceptance;
};
const LicenseEntry kLicenses[] = {
    {"apache-2.0", "Apache License 2.0",
     "https://www.apache.org/licenses/LICENSE-2.0", false},
    {"mit", "MIT License", "https://opensource.org/license/mit", false},
    {"llama3.1", "Llama 3.1 Community License",
     "https://www.llama.com/llama3_1/license/", true},
    {"gemma", "Gemma Terms of Use",
     "https://huggingface.co/unsloth/gemma-3-12b-it-GGUF", true},
};

const LicenseEntry* FindLicense(const std::string& id) {
  for (const LicenseEntry& l : kLicenses) {
    if (id == l.id) return &l;
  }
  return nullptr;
}

std::string ComputeUnsupportedReason() {
#if BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_IOS)
  // The inference service isn't registered in this build's utility process
  // (chrome/utility/services.cc), so there is nothing to launch.
  return "On-device AI isn't available in this version of MoltBrowser. Use "
         "a cloud model instead (AI Settings > Connect AI Providers).";
#else
#if defined(ARCH_CPU_X86_64)
  // //third_party/llama_cpp builds ggml's x86 kernels with -mavx2 -mfma
  // -mf16c. On a CPU without them the first quantized matmul raises SIGILL.
  // (Every shipping AVX2 CPU also has F16C, which base::CPU doesn't
  // expose.) The inference service checks again before loading.
  const base::CPU& cpu = base::CPU::GetInstanceNoAllocation();
  if (!cpu.has_avx2() || !cpu.has_fma3()) {
    return "This CPU doesn't support AVX2, which on-device AI in "
           "MoltBrowser needs. Use a cloud model instead (AI Settings > "
           "Connect AI Providers).";
  }
#endif
  return std::string();
#endif
}

std::string GbString(uint64_t bytes) {
  return base::NumberToString((bytes + 512ULL * 1024 * 1024) / kGiB);
}

// Longest prefix of |text| of at most |cap| bytes that doesn't end inside a
// UTF-8 character.
std::string CapText(const std::string& text, size_t cap) {
  if (text.size() <= cap) return text;
  return text.substr(0, Utf8SafePrefixLength(std::string_view(text).substr(
                            0, cap)));
}

std::string CapRequestText(const std::string& text) {
  return CapText(text, mojom::kMaxRequestTextBytes);
}

// The service caps conversation keys; a longer key is replaced by a short
// digest. A collision would only compare two conversations' token prefixes
// (reuse is exact-prefix), never mix their text.
std::string ConversationKeyForService(const std::string& key) {
  if (key.size() <= mojom::kMaxConversationKeyBytes) return key;
  return "h:" + base::NumberToString(
                    static_cast<uint64_t>(std::hash<std::string>()(key))) +
         ":" + base::NumberToString(static_cast<uint64_t>(key.size()));
}

std::vector<std::string> StopSequencesForService(
    const std::vector<std::string>& stops) {
  std::vector<std::string> out;
  for (const std::string& s : stops) {
    if (out.size() >= mojom::kMaxStopSequences) break;
    if (s.empty() || s.size() > mojom::kMaxStopSequenceBytes ||
        !IsValidUtf8(s)) {
      continue;
    }
    out.push_back(s);
  }
  return out;
}

mojom::GenerationParamsPtr ToServiceParams(const PromptOptions& o) {
  auto p = mojom::GenerationParams::New();
  p->max_visible_tokens = o.max_tokens > 0 ? static_cast<uint32_t>(o.max_tokens)
                                           : 0u;
  p->max_reasoning_tokens =
      o.max_reasoning_tokens > 0 ? static_cast<uint32_t>(o.max_reasoning_tokens)
                                 : 0u;
  p->reasoning = o.reasoning == ReasoningMode::kOn ? mojom::ReasoningMode::kOn
                                                   : mojom::ReasoningMode::kOff;
  p->temperature = o.temperature;
  p->top_p = o.top_p;
  p->top_k = o.top_k > 0 ? static_cast<uint32_t>(o.top_k) : 0u;
  p->conversation_key = ConversationKeyForService(o.conversation_key);
  p->stop_sequences = StopSequencesForService(o.stop_sequences);
  return p;
}

// The trust labels stay structural: browser-authored text goes in
// system_prompt / context_preamble only; page text, history and the user's
// message keep their own fields so the service neutralizes them.
mojom::ChatRequestPtr ToServiceChat(const ChatPrompt& chat) {
  auto r = mojom::ChatRequest::New();
  r->system_prompt = CapRequestText(chat.system_prompt);
  r->context_preamble = CapRequestText(chat.context_preamble);
  r->context = CapRequestText(chat.context);
  r->user_message = CapRequestText(chat.user_message);
  std::vector<const ChatMessage*> turns;
  for (const ChatMessage& m : chat.history) {
    if (m.content.empty()) continue;
    if (m.role != "user" && m.role != "assistant") continue;
    turns.push_back(&m);
  }
  // The newest turns (the budget drops the oldest first anyway).
  const size_t from = turns.size() > mojom::kMaxHistoryTurns
                          ? turns.size() - mojom::kMaxHistoryTurns
                          : 0;
  for (size_t i = from; i < turns.size(); ++i) {
    r->history.push_back(mojom::ChatTurn::New(
        turns[i]->role == "user" ? mojom::ChatRole::kUser
                                 : mojom::ChatRole::kAssistant,
        CapRequestText(turns[i]->content)));
  }
  return r;
}

// User-facing text for a load that didn't succeed.
std::string LoadError(const InferenceOutcome& outcome, const std::string& name) {
  using Kind = InferenceOutcome::Kind;
  switch (outcome.kind) {
    case Kind::kOk:
    case Kind::kCancelled:
      return std::string();
    case Kind::kLoadFailed:
      switch (outcome.load_status) {
        case mojom::LoadModelStatus::kOk:
          return std::string();
        case mojom::LoadModelStatus::kUnsupportedCpu:
          return "This CPU doesn't support AVX2, which on-device AI in "
                 "MoltBrowser needs. Use a cloud model instead (AI Settings "
                 "> Connect AI Providers).";
        case mojom::LoadModelStatus::kInvalidModelFile:
          return "Couldn't load " + name +
                 ". The file may be incomplete or incompatible — try "
                 "deleting and re-downloading it from the model menu.";
        case mojom::LoadModelStatus::kOutOfMemory:
          return "Couldn't start " + name +
                 " — not enough memory for it right now. Close some apps or "
                 "tabs, or pick a smaller model.";
        case mojom::LoadModelStatus::kBackendUnavailable:
          return "Couldn't start " + name +
                 ": on-device AI found no way to run it on this device.";
      }
      return "Couldn't load " + name + ".";
    case Kind::kProcessGone:
      return "The on-device AI process stopped while loading " + name +
             ". Try again, or pick a smaller model.";
    case Kind::kLaunchFailed:
#if BUILDFLAG(IS_WIN)
      return "On-device AI couldn't start its sandboxed process. The "
             "portable ZIP can't run on-device models: install MoltBrowser "
             "with the installer, or use a cloud model.";
#else
      return "On-device AI couldn't start its sandboxed process. Try again, "
             "or use a cloud model.";
#endif
    case Kind::kViolation:
      return "Couldn't load " + name +
             ": the on-device AI process sent an invalid response.";
    case Kind::kTimedOut:
      return "Loading " + name +
             " took too long and was stopped. Try again, or pick a smaller "
             "model.";
    case Kind::kMemoryExceeded:
      return name + " used far more memory than expected and was stopped. "
                    "Pick a smaller model.";
    case Kind::kNoModel:
      return "Couldn't load " + name + ".";
    case Kind::kShuttingDown:
      return "MoltBrowser is closing.";
  }
  return "Couldn't load " + name + ".";
}

// Fills |result| from a generation outcome. Returns the error ("" = none).
std::string GenerationError(const InferenceOutcome& outcome,
                            GenerationResult* result) {
  using Kind = InferenceOutcome::Kind;
  switch (outcome.kind) {
    case Kind::kOk:
      break;
    case Kind::kCancelled:
      result->was_cancelled = true;
      return std::string();
    case Kind::kLoadFailed:
    case Kind::kProcessGone:
    case Kind::kLaunchFailed:
      return "The on-device model stopped unexpectedly (it may have run out "
             "of memory). Try again, or pick a smaller model.";
    case Kind::kViolation:
      return "The on-device model sent an invalid response and was stopped. "
             "Try again.";
    case Kind::kTimedOut:
      return "The on-device model stopped responding and was stopped. Try "
             "again.";
    case Kind::kMemoryExceeded:
      return "The on-device model used far more memory than expected and was "
             "stopped. Try a smaller model.";
    case Kind::kNoModel:
      return "No model loaded";
    case Kind::kShuttingDown:
      return "MoltBrowser is closing.";
  }
  if (!outcome.has_summary) return std::string();  // stopped by the browser
  const StreamSummary& s = outcome.summary;
  result->tokens_prompt = static_cast<int>(s.prompt_tokens);
  result->tokens_reused = static_cast<int>(s.reused_tokens);
  result->tokens_generated = static_cast<int>(s.generated_tokens);
  switch (static_cast<mojom::GenerationStatus>(s.status)) {
    case mojom::GenerationStatus::kEndOfGeneration:
    case mojom::GenerationStatus::kMaxTokens:
    case mojom::GenerationStatus::kStopSequence:
    case mojom::GenerationStatus::kContextFull:
      return std::string();
    case mojom::GenerationStatus::kCancelled:
      result->was_cancelled = true;
      return std::string();
    case mojom::GenerationStatus::kPromptTooLong:
      return "This conversation is too long for the model's context window. "
             "Start a new chat or shorten your message.";
    case mojom::GenerationStatus::kSystemPromptTooLong:
      return "The system prompt is too long for this model's context window.";
    case mojom::GenerationStatus::kTokenizeFailed:
      return "Failed to tokenize prompt";
    case mojom::GenerationStatus::kDecodeFailed:
      return s.generated_tokens == 0
                 ? "The model couldn't process the prompt."
                 : "The model stopped unexpectedly while answering.";
  }
  return "The model stopped unexpectedly while answering.";
}

}  // namespace

// ============================================================
// Implementation (PIMPL pattern)
// ============================================================
struct BrowserAIRuntime::Impl {
  HardwareCapability hardware;

  // Registry. Guarded by models_mutex (read from the UI thread, written by
  // load/refresh on workers).
  mutable std::mutex models_mutex;
  std::unordered_map<std::string, ModelInfo> models;
  std::string active_model_id;

  // Serializes LoadModel / UnloadModel / DeleteModel (never two multi-GB
  // models loading at once). Held while waiting for the inference process,
  // never by the UI thread.
  std::mutex load_mutex;

  // What is loaded, and how busy it is. Guarded by state_mutex; never held
  // while waiting on anything.
  mutable std::mutex state_mutex;
  std::string loaded_model_id;
  std::string loaded_arch;
  std::string loaded_crash_key;
  int active_generations = 0;
  bool is_loading = false;

  std::atomic<bool> initialized{false};
  std::atomic<bool> shutting_down{false};
  // Non-empty => no inference process is ever launched.
  std::string unsupported_reason;
  std::string model_directory;

  // Per-request cancellation: id -> state. A Stop that arrives before the
  // request reaches the inference process is remembered here.
  struct RequestState {
    bool cancelled = false;
    // The request's mailbox while it waits (woken on cancel).
    scoped_refptr<InferenceMailbox> mailbox;
  };
  mutable std::mutex requests_mutex;
  std::unordered_map<uint64_t, RequestState> requests;
  uint64_t next_request_id = 1;

  // Crash-loop guard. Guarded by guard_mutex.
  mutable std::mutex guard_mutex;
  CrashLoopGuard crash_guard;

  // Idle unload.
  std::atomic<uint64_t> activity_epoch{0};
  std::atomic<int64_t> idle_timeout_ms{kDefaultIdleUnload.InMilliseconds()};

  // The UI-thread side. Created by Initialize() on the UI thread and, like
  // this singleton, never destroyed; |host| is only dereferenced there.
  std::unique_ptr<HostDelegate> host_delegate;
  std::unique_ptr<InferenceProcessHost> host;
  std::atomic<InferenceProcessHost*> host_ptr{nullptr};
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner;
  base::CallbackListSubscription app_terminating_subscription;

  // Memory-pressure monitoring; created and destroyed on the UI thread.
  std::unique_ptr<PressureListener> pressure_listener;
  std::unique_ptr<base::MemoryPressureListenerRegistration>
      pressure_registration;

  void DetectHardware() {
    hardware.total_ram_bytes = static_cast<size_t>(
        base::SysInfo::AmountOfTotalPhysicalMemory().InBytes());
    // Refined per load by CheckRamForModel().
    hardware.available_ram_bytes = hardware.total_ram_bytes;
#if BUILDFLAG(IS_MAC)
    // Apple Silicon has unified memory shared with the GPU.
    hardware.gpu_backend = "metal";
    hardware.has_gpu_acceleration = true;
    hardware.gpu_vram_bytes = hardware.total_ram_bytes;
#else
    // The GN build compiles the CPU backend only on Windows/Linux.
    hardware.gpu_backend = "none";
    hardware.has_gpu_acceleration = false;
#endif
#if defined(ARCH_CPU_ARM64)
    hardware.architecture = "arm64";
#else
    hardware.architecture = "x86_64";
#endif
    hardware.cpu_cores = static_cast<int>(std::thread::hardware_concurrency());
  }

  void InitializeModelRegistry() {
    std::lock_guard<std::mutex> lock(models_mutex);
    for (size_t i = 0; i < kModelRegistrySize; ++i) {
      const auto& entry = kModelRegistry[i];
      if (entry.hidden) continue;  // not offered until pinned
      ModelInfo info;
      info.model_id = entry.model_id;
      info.display_name = entry.display_name;
      info.huggingface_id = entry.huggingface_id;
      info.filename = entry.filename;
      info.revision = entry.revision;
      info.sha256 = entry.sha256;
      info.file_size_bytes = entry.file_size_bytes;
      info.ram_required_bytes = entry.ram_required_bytes;
      info.parameter_count_billions = entry.param_billions;
      info.quantization = entry.quantization;
      info.company = entry.company;
      if (const LicenseEntry* lic = FindLicense(entry.license)) {
        info.license_name = lic->name;
        info.license_url = lic->url;
        if (lic->requires_acceptance) {
          info.license_id = lic->id;
        }
      } else {
        // Terms not recorded yet: point at the model card rather than
        // showing nothing.
        info.license_name = "See the model card";
        info.license_url =
            std::string("https://huggingface.co/") + entry.huggingface_id;
      }

      // Set local file path. is_downloaded is left false here and updated
      // by RefreshModelStatus() on a worker thread to keep Initialize()
      // non-blocking on the UI thread.
      info.file_path = model_directory + "/" + entry.filename;
      info.is_downloaded = false;
      info.is_loaded = false;

      models[entry.model_id] = info;
    }
  }

  bool IsCancelled(uint64_t id) const {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(requests_mutex);
    auto it = requests.find(id);
    return it != requests.end() && it->second.cancelled;
  }

  void SetRequestMailbox(uint64_t id, scoped_refptr<InferenceMailbox> mailbox) {
    std::lock_guard<std::mutex> lock(requests_mutex);
    auto it = requests.find(id);
    if (it != requests.end()) it->second.mailbox = std::move(mailbox);
  }

  // Marks |model_id| loaded (or nothing, when empty) in the registry and the
  // state. Caller holds neither lock.
  void SetLoaded(const std::string& model_id,
                 const std::string& arch,
                 const std::string& crash_key) {
    {
      std::lock_guard<std::mutex> lock(models_mutex);
      for (auto& [id, m] : models) {
        m.is_loaded = !model_id.empty() && id == model_id;
      }
      active_model_id = model_id;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    loaded_model_id = model_id;
    loaded_arch = arch;
    loaded_crash_key = crash_key;
  }

  // Posts |task| to the host on the UI thread. False if there is no host or
  // the UI thread no longer takes tasks.
  bool PostToHost(base::OnceCallback<void(InferenceProcessHost*)> task) {
    InferenceProcessHost* h = host_ptr.load();
    if (!h || !ui_task_runner) return false;
    return ui_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(task), base::Unretained(h)));
  }

  // Blocks the calling worker until |mailbox| is finished, delivering text to
  // |on_text| (at most every kDeliveryInterval). Returns early (with a
  // made-up outcome) on app termination, when the UI thread stops answering,
  // or after |deadline|. If |request_id| is cancelled meanwhile, the host is
  // asked to cancel |token|.
  InferenceOutcome Wait(InferenceMailbox* mailbox,
                        uint64_t request_id,
                        uint64_t token,
                        base::TimeDelta deadline,
                        const std::function<void(const std::string&)>& on_text) {
    DCHECK(!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
    base::ScopedBlockingCall blocking(FROM_HERE,
                                      base::BlockingType::WILL_BLOCK);
    const Clock::time_point start = Clock::now();
    const auto limit = std::chrono::milliseconds(deadline.InMilliseconds());
    Clock::time_point last_delivery;
    Clock::time_point cancel_sent;
    bool cancelling = false;
    Clock::time_point ping_sent;
    bool pinging = false;
    std::string pending;

    auto deliver = [&](Clock::time_point now) {
      if (pending.empty()) return;
      if (on_text) on_text(pending);
      pending.clear();
      last_delivery = now;
    };
    auto request_cancel = [&]() {
      if (cancelling || token == 0) return;
      cancelling = true;
      cancel_sent = Clock::now();
      PostToHost(base::BindOnce(
          [](uint64_t t, InferenceProcessHost* h) { h->Cancel(t); }, token));
    };

    while (true) {
      std::chrono::milliseconds timeout = kWaitSlice;
      const bool pacing = !pending.empty();
      if (pacing) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - last_delivery);
        timeout = std::clamp(kDeliveryInterval - since,
                             std::chrono::milliseconds(1), kDeliveryInterval);
      }
      const bool finished = mailbox->Wait(timeout, &pending);
      const Clock::time_point now = Clock::now();
      if (finished) {
        deliver(now);
        return mailbox->outcome();
      }
      if (now - last_delivery >= kDeliveryInterval) deliver(now);

      if (shutting_down.load()) {
        request_cancel();
        deliver(now);
        return InferenceOutcome(InferenceOutcome::Kind::kShuttingDown);
      }
      if (IsCancelled(request_id)) request_cancel();
      if (cancelling && now - cancel_sent > kCancelConfirmWait) {
        deliver(now);
        return InferenceOutcome(InferenceOutcome::Kind::kCancelled);
      }
      // Liveness of the UI thread itself (see kUiTimeout).
      const Clock::time_point heartbeat = mailbox->last_heartbeat();
      if (pinging && heartbeat >= ping_sent) pinging = false;
      if (!pinging && now - heartbeat > kHeartbeatInterval) {
        pinging = true;
        ping_sent = now;
        scoped_refptr<InferenceMailbox> ref(mailbox);
        if (!PostToHost(base::BindOnce(
                [](scoped_refptr<InferenceMailbox> m, InferenceProcessHost* h) {
                  h->Ping(std::move(m));
                },
                std::move(ref)))) {
          deliver(now);
          return InferenceOutcome(InferenceOutcome::Kind::kShuttingDown);
        }
      }
      if (pinging && now - ping_sent > kUiTimeout) {
        LOG(WARNING) << "[MoltAI] UI thread not responding; giving up on an "
                        "on-device AI request";
        deliver(now);
        return InferenceOutcome(InferenceOutcome::Kind::kShuttingDown);
      }
      if (now - start > limit) {
        request_cancel();
        deliver(now);
        return InferenceOutcome(InferenceOutcome::Kind::kTimedOut);
      }
    }
  }
};

// Unloads the model on critical memory pressure (when idle).
class BrowserAIRuntime::PressureListener : public base::MemoryPressureListener {
 public:
  explicit PressureListener(BrowserAIRuntime* runtime) : runtime_(runtime) {}

 private:
  void OnMemoryPressure(base::MemoryPressureLevel level) override {
    if (level != base::MEMORY_PRESSURE_LEVEL_CRITICAL) return;
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce(&BrowserAIRuntime::OnCriticalMemoryPressure,
                       base::Unretained(runtime_.get())));
  }

  // The runtime is an immortal singleton.
  raw_ptr<BrowserAIRuntime> runtime_;
};

// Process events from the host (UI thread).
class BrowserAIRuntime::HostDelegate : public InferenceProcessHost::Delegate {
 public:
  explicit HostDelegate(BrowserAIRuntime* runtime) : runtime_(runtime) {}

  void OnModelProcessLost(const std::string& model_id,
                          const std::string& crash_key,
                          CrashLoopGuard::Phase phase) override {
    runtime_->OnModelProcessLost(model_id, crash_key, static_cast<int>(phase));
  }

 private:
  // The runtime is an immortal singleton.
  raw_ptr<BrowserAIRuntime> runtime_;
};

// ============================================================
// Public API Implementation
// ============================================================

// static
BrowserAIRuntime& BrowserAIRuntime::GetInstance() {
  // Immortal, lazily-constructed, never destroyed — background load/generation
  // tasks hold a raw pointer to it (see the note in the header). NoDestructor
  // is thread-safe to initialize; in practice every caller reaches it from the
  // UI thread before posting work.
  static base::NoDestructor<BrowserAIRuntime> instance;
  return *instance;
}

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
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  impl_->unsupported_reason = ComputeUnsupportedReason();
  if (!impl_->unsupported_reason.empty()) {
    LOG(WARNING) << "[MoltAI] on-device inference disabled: "
                 << impl_->unsupported_reason;
  }

  // Detect hardware capabilities (sysconf/sysctl — non-blocking)
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

  if (impl_->unsupported_reason.empty()) {
    // The client of the sandboxed inference process lives on this (UI)
    // thread. No process is launched until a model is loaded.
    impl_->ui_task_runner = base::SequencedTaskRunner::GetCurrentDefault();
    impl_->host_delegate = std::make_unique<HostDelegate>(this);
    impl_->host =
        std::make_unique<InferenceProcessHost>(impl_->host_delegate.get());
    impl_->host_ptr = impl_->host.get();
    // Workers blocked on the inference process return when the app
    // terminates, so they never hold up ThreadPool shutdown.
    impl_->app_terminating_subscription =
        browser_shutdown::AddAppTerminatingCallback(base::BindOnce(
            &BrowserAIRuntime::OnAppTerminating, base::Unretained(this)));
  }

  impl_->initialized = true;
  return true;
}

void BrowserAIRuntime::OnAppTerminating() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  impl_->shutting_down = true;
  {
    std::lock_guard<std::mutex> lock(impl_->requests_mutex);
    for (auto& [id, request] : impl_->requests) {
      request.cancelled = true;
      if (request.mailbox) request.mailbox->Wake();
    }
  }
  if (impl_->host) impl_->host->Shutdown();
}

void BrowserAIRuntime::Shutdown() {
  if (!impl_->initialized) return;
  CancelGeneration();
  impl_->SetLoaded(std::string(), std::string(), std::string());
  impl_->PostToHost(base::BindOnce([](InferenceProcessHost* h) {
    h->Unload(InferenceProcessHost::UnloadMode::kNow, nullptr);
  }));
}

std::string BrowserAIRuntime::GetLocalInferenceUnsupportedReason() const {
  return impl_->unsupported_reason;
}

void BrowserAIRuntime::StartMemoryPressureMonitoring() {
  if (impl_->pressure_registration) return;
  impl_->pressure_listener = std::make_unique<PressureListener>(this);
  // MemoryPressureListenerTag is a closed enum in //base and only feeds a
  // Finch suppression mask; reuse an existing tag rather than fork base/ for
  // a new value. kOnDeviceTailModelService (used up to 148) was retired at
  // 152; kGlicKeyedService is the closest live one (a browser-process AI
  // service). Critical-level notifications are never suppressed by that
  // mask's "1" setting.
  impl_->pressure_registration =
      std::make_unique<base::MemoryPressureListenerRegistration>(
          base::MemoryPressureListenerTag::kGlicKeyedService,
          impl_->pressure_listener.get());
}

void BrowserAIRuntime::SetIdleUnloadTimeout(base::TimeDelta timeout) {
  impl_->idle_timeout_ms = std::max<int64_t>(0, timeout.InMilliseconds());
  TouchActivity();
}

void BrowserAIRuntime::TouchActivity() {
  const uint64_t epoch = ++impl_->activity_epoch;
  const int64_t ms = impl_->idle_timeout_ms.load();
  if (ms <= 0) return;
  base::ThreadPool::PostDelayedTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&BrowserAIRuntime::MaybeUnloadIdle,
                     base::Unretained(this), epoch),
      base::Milliseconds(ms));
}

void BrowserAIRuntime::MaybeUnloadIdle(uint64_t epoch) {
  // Any newer activity re-armed its own check.
  if (epoch != impl_->activity_epoch.load()) return;
  UnloadIfIdle("idle");
}

void BrowserAIRuntime::OnCriticalMemoryPressure() {
  UnloadIfIdle("critical memory pressure");
}

void BrowserAIRuntime::UnloadIfIdle(const char* reason) {
  // A load in progress holds load_mutex; never wait for it here.
  std::unique_lock<std::mutex> load_lock(impl_->load_mutex, std::try_to_lock);
  if (!load_lock.owns_lock()) return;
  std::string unloading;
  {
    // Check and forget in one step, so a generation starting now either
    // pins the model first or sees it gone ("No model loaded").
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    // Keep today's policy: never interrupt a generation.
    if (impl_->loaded_model_id.empty() || impl_->active_generations > 0 ||
        impl_->is_loading) {
      return;
    }
    unloading = impl_->loaded_model_id;
    impl_->loaded_model_id.clear();
    impl_->loaded_arch.clear();
    impl_->loaded_crash_key.clear();
  }
  LOG(INFO) << "[MoltAI] " << reason << ": ending the on-device model process ("
            << unloading << ")";
  impl_->SetLoaded(std::string(), std::string(), std::string());
  // Ending the process frees the weights, the KV cache and any GPU buffers
  // at once.
  impl_->PostToHost(base::BindOnce([](InferenceProcessHost* h) {
    h->Unload(InferenceProcessHost::UnloadMode::kGraceful, nullptr);
  }));
}

void BrowserAIRuntime::OnModelProcessLost(const std::string& model_id,
                                          const std::string& crash_key,
                                          int phase) {
  {
    std::lock_guard<std::mutex> lock(impl_->guard_mutex);
    impl_->crash_guard.RecordCrash(
        crash_key, static_cast<CrashLoopGuard::Phase>(phase),
        (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds());
  }
  bool was_loaded = false;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    was_loaded = impl_->loaded_model_id == model_id;
  }
  // The next chat send relaunches it (the chat handler loads when
  // GetLoadedModelId() differs); agents and automation report "No model
  // loaded", as after an idle unload.
  if (was_loaded) impl_->SetLoaded(std::string(), std::string(), std::string());
  LOG(WARNING) << "[MoltAI] the on-device model process (" << model_id
               << ") ended unexpectedly";
}

RamCheck BrowserAIRuntime::CheckRamForModel(const std::string& model_id) const {
  RamCheck rc;
  ModelInfo info;
  uint64_t reclaimable = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    auto it = impl_->models.find(model_id);
    if (it == impl_->models.end()) return rc;
    info = it->second;
    // Unloading a different resident model releases its weights first.
    for (const auto& [id, m] : impl_->models) {
      if (m.is_loaded && id != model_id) {
        reclaimable += m.file_size_bytes + kRuntimeOverheadBytes;
      }
    }
  }
  rc.total_bytes = base::SysInfo::AmountOfTotalPhysicalMemory().InBytes();
  rc.available_bytes =
      base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes() + reclaimable;
  // Weights must be resident (mmapped and paged in) plus compute/KV
  // buffers. ram_required_bytes in the registry is a generous "comfortable"
  // figure; the hard requirement is what has to be free right now.
  const uint64_t weights = info.file_size_bytes ? info.file_size_bytes
                                                : info.ram_required_bytes;
  rc.needed_bytes = weights + kRuntimeOverheadBytes;
  rc.exceeds_total =
      static_cast<double>(rc.needed_bytes) >
      static_cast<double>(rc.total_bytes) * kMaxShareOfTotalRam;
  rc.ok = !rc.exceeds_total && rc.needed_bytes <= rc.available_bytes;
  return rc;
}

bool BrowserAIRuntime::CanRunModel(const std::string& model_id) const {
  return CheckRamForModel(model_id).ok;
}

bool BrowserAIRuntime::LoadModel(const std::string& model_id,
                                 std::string* error) {
  auto set_error = [&](std::string e) {
    if (error) *error = std::move(e);
  };
  if (!impl_->unsupported_reason.empty()) {
    set_error(impl_->unsupported_reason);
    return false;
  }
  if (impl_->shutting_down) {
    set_error("MoltBrowser is closing.");
    return false;
  }
  if (!impl_->host_ptr.load()) {
    set_error("On-device AI isn't ready yet. Try again in a moment.");
    return false;
  }

  // Serializes loads/unloads: never two multi-GB models loading at once.
  // The slow part (the process loads the model) runs while another model
  // may keep serving requests in its own process.
  std::lock_guard<std::mutex> load_lock(impl_->load_mutex);

  ModelInfo info;
  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    auto it = impl_->models.find(model_id);
    if (it == impl_->models.end()) {
      set_error("Unknown model '" + model_id + "'.");
      return false;
    }
    info = it->second;
  }
  const std::string name =
      info.display_name.empty() ? model_id : info.display_name;

  if (!info.is_downloaded) {
    set_error(name +
              " isn't downloaded yet. Pick it from the model menu to "
              "download it.");
    return false;
  }

  bool previous_loaded = false;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->loaded_model_id == model_id) {
      TouchActivity();
      return true;
    }
    previous_loaded = !impl_->loaded_model_id.empty();
  }

  // RAM check against what is actually free.
  const RamCheck rc = CheckRamForModel(model_id);
  if (!rc.ok) {
    if (rc.exceeds_total) {
      set_error(name + " needs about " + GbString(rc.needed_bytes) +
                " GB of memory, but this device has " +
                GbString(rc.total_bytes) + " GB. Try a smaller model.");
    } else {
      set_error(name + " needs about " + GbString(rc.needed_bytes) +
                " GB of free memory, but only about " +
                GbString(rc.available_bytes) +
                " GB is available right now. Close some apps or tabs, or "
                "pick a smaller model.");
    }
    return false;
  }

  // The browser opens the file (the sandbox can't) and hands the process a
  // read-only handle. Not async: llama.cpp reads it synchronously. Share
  // delete, so DeleteModel isn't blocked by a process still exiting.
  base::File file(base::FilePath::FromUTF8Unsafe(info.file_path),
                  base::File::FLAG_OPEN | base::File::FLAG_READ |
                      base::File::FLAG_WIN_SHARE_DELETE);
  base::File::Info file_info;
  if (!file.IsValid() || !file.GetInfo(&file_info) ||
      file_info.is_directory || file_info.size <= 0) {
    set_error("Couldn't open " + name +
              ". Try deleting and re-downloading it from the model menu.");
    return false;
  }
  const uint64_t file_size = static_cast<uint64_t>(file_info.size);
  if (info.is_verified() && info.file_size_bytes &&
      file_size != info.file_size_bytes) {
    set_error("Couldn't load " + name +
              ". The file is incomplete — try deleting and re-downloading it "
              "from the model menu.");
    return false;
  }
  const std::string crash_key =
      model_id + ":" + base::NumberToString(file_size) + ":" +
      base::NumberToString(static_cast<int64_t>(
          file_info.last_modified.ToDeltaSinceWindowsEpoch().InMicroseconds()));
  {
    std::lock_guard<std::mutex> lock(impl_->guard_mutex);
    const CrashLoopGuard::Verdict verdict = impl_->crash_guard.Check(
        crash_key,
        (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds());
    if (verdict != CrashLoopGuard::Verdict::kAllowed) {
      set_error(CrashLoopGuard::Explain(verdict));
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->is_loading = true;
  }
  struct LoadingScope {
    raw_ptr<Impl> impl;
    ~LoadingScope() {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      impl->is_loading = false;
    }
  } loading_scope{impl_.get()};

  // End the old model's process FIRST when the new one wouldn't fit next to
  // it (it finishes its running requests); otherwise the new process loads
  // while the old one keeps serving, and replaces it on success.
  if (previous_loaded &&
      base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes() <
          static_cast<int64_t>(rc.needed_bytes)) {
    LOG(INFO) << "[MoltAI] ending the current model's process before loading "
              << model_id << " (low memory)";
    impl_->SetLoaded(std::string(), std::string(), std::string());
    auto exited = base::MakeRefCounted<InferenceMailbox>();
    if (impl_->PostToHost(base::BindOnce(
            [](scoped_refptr<InferenceMailbox> m, InferenceProcessHost* h) {
              h->Unload(InferenceProcessHost::UnloadMode::kGraceful,
                        std::move(m));
            },
            exited))) {
      impl_->Wait(exited.get(), 0, 0, kUnloadWait, nullptr);
    }
  }

  InferenceProcessHost::LoadRequest request;
  request.model_id = model_id;
  request.crash_key = crash_key;
  request.file = std::move(file);
  request.file_size = file_size;
  request.total_ram_bytes = impl_->hardware.total_ram_bytes;
  const int cores = impl_->hardware.cpu_cores;
  request.cpu_threads =
      static_cast<uint32_t>(cores > 4 ? cores / 2 : std::max(1, cores));
  request.max_context_tokens = kMaxContextTokens;
  request.allow_cpu_repack = true;
  // Footprint watchdog: weights (repacked copies count on CPU) + the KV
  // cache the RAM tiers allow + 2 GB, and 50% on top of that.
  const uint64_t ram = impl_->hardware.total_ram_bytes;
  const uint64_t kv_bound = std::max<uint64_t>(
      1 * kGiB, ram > file_size + 4 * kGiB ? (ram - file_size - 4 * kGiB) / 2
                                           : 0);
  const uint64_t expected = file_size + kv_bound + 2 * kGiB;
  request.footprint_limit_bytes = expected + expected / 2;

  auto mailbox = base::MakeRefCounted<InferenceMailbox>();
  if (!impl_->PostToHost(base::BindOnce(
          [](InferenceProcessHost::LoadRequest r,
             scoped_refptr<InferenceMailbox> m, InferenceProcessHost* h) {
            h->Load(std::move(r), std::move(m));
          },
          std::move(request), mailbox))) {
    set_error("MoltBrowser is closing.");
    return false;
  }
  const InferenceOutcome outcome =
      impl_->Wait(mailbox.get(), 0, 0, kLoadWait, nullptr);
  if (outcome.kind != InferenceOutcome::Kind::kOk) {
    std::string message = LoadError(outcome, name);
    if (outcome.kind == InferenceOutcome::Kind::kProcessGone ||
        outcome.kind == InferenceOutcome::Kind::kTimedOut ||
        outcome.kind == InferenceOutcome::Kind::kViolation) {
      std::lock_guard<std::mutex> lock(impl_->guard_mutex);
      const CrashLoopGuard::Verdict verdict = impl_->crash_guard.Check(
          crash_key,
          (base::TimeTicks::Now() - base::TimeTicks()).InMilliseconds());
      if (verdict == CrashLoopGuard::Verdict::kRefusedAfterLoadCrashes) {
        message = CrashLoopGuard::Explain(verdict);
      }
    }
    LOG(ERROR) << "[MoltAI] couldn't load " << model_id << " (outcome "
               << static_cast<int>(outcome.kind) << ")";
    set_error(message.empty() ? "Couldn't load " + name + "." : message);
    return false;
  }

  impl_->SetLoaded(model_id, outcome.architecture, crash_key);
  LOG(INFO) << "[MoltAI] model loaded in the inference process: " << model_id
            << " (arch=" << outcome.architecture
            << " backend=" << outcome.backend
            << " ctx=" << outcome.context_tokens
            << " repack=" << outcome.cpu_repack_used << ")";
  TouchActivity();
  return true;
}

bool BrowserAIRuntime::UnloadModel(const std::string& model_id) {
  // Same serialization as LoadModel.
  std::lock_guard<std::mutex> load_lock(impl_->load_mutex);
  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    if (impl_->models.find(model_id) == impl_->models.end()) return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->loaded_model_id != model_id) return true;
  }
  impl_->SetLoaded(std::string(), std::string(), std::string());
  // The process finishes its running requests, then ends; wait for that,
  // so the memory (and, on Windows, the file) is free when this returns.
  auto exited = base::MakeRefCounted<InferenceMailbox>();
  if (impl_->PostToHost(base::BindOnce(
          [](scoped_refptr<InferenceMailbox> m, InferenceProcessHost* h) {
            h->Unload(InferenceProcessHost::UnloadMode::kGraceful,
                      std::move(m));
          },
          exited))) {
    impl_->Wait(exited.get(), 0, 0, kUnloadWait, nullptr);
  }
  return true;
}

std::vector<ModelInfo> BrowserAIRuntime::GetAvailableModels() const {
  std::lock_guard<std::mutex> lock(impl_->models_mutex);
  std::vector<ModelInfo> result;
  result.reserve(impl_->models.size());
  for (const auto& [id, info] : impl_->models) {
    result.push_back(info);
  }
  return result;
}

std::string BrowserAIRuntime::GetLoadedModelId() const {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  return impl_->loaded_model_id;
}

ModelInfo BrowserAIRuntime::GetModelInfo(const std::string& model_id) const {
  std::lock_guard<std::mutex> lock(impl_->models_mutex);
  auto it = impl_->models.find(model_id);
  if (it != impl_->models.end()) {
    return it->second;
  }
  return {};
}

bool BrowserAIRuntime::DownloadModel(
    const std::string& model_id,
    std::function<void(size_t, size_t)> progress_callback) {
  // Downloads are driven by the chat handler (molt_ai/models/
  // model_download_job.h: pinned revision, streamed SHA-256, resumable).
  // This entry point only reports whether the file is already present.
  return GetModelInfo(model_id).is_downloaded;
}

bool BrowserAIRuntime::DeleteModel(const std::string& model_id) {
  ModelInfo info = GetModelInfo(model_id);
  if (info.model_id.empty()) {
    return false;
  }
  // Waits until the model's process has exited.
  UnloadModel(model_id);
  std::error_code ec;
  std::filesystem::remove(info.file_path, ec);
  std::lock_guard<std::mutex> lock(impl_->models_mutex);
  impl_->models[model_id].is_downloaded = false;
  return true;
}

// ---- Cancellation ----------------------------------------------------------

uint64_t BrowserAIRuntime::NewRequestId() {
  std::lock_guard<std::mutex> lock(impl_->requests_mutex);
  const uint64_t id = impl_->next_request_id++;
  impl_->requests[id] = Impl::RequestState();
  return id;
}

void BrowserAIRuntime::CancelRequest(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(impl_->requests_mutex);
  auto it = impl_->requests.find(request_id);
  if (it == impl_->requests.end()) return;
  it->second.cancelled = true;
  // Wake the blocked worker; it asks the host to close the request's
  // streamer, which cancels it in the inference process.
  if (it->second.mailbox) it->second.mailbox->Wake();
}

bool BrowserAIRuntime::IsRequestCancelled(uint64_t request_id) const {
  return impl_->IsCancelled(request_id);
}

void BrowserAIRuntime::FinishRequest(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(impl_->requests_mutex);
  impl_->requests.erase(request_id);
}

void BrowserAIRuntime::CancelGeneration() {
  std::lock_guard<std::mutex> lock(impl_->requests_mutex);
  for (auto& [id, request] : impl_->requests) {
    request.cancelled = true;
    if (request.mailbox) request.mailbox->Wake();
  }
}

// ---- Inference -------------------------------------------------------------

namespace {

// RAII: allocates a private request id when the caller didn't pass one and
// releases it afterwards.
class ScopedRequest {
 public:
  ScopedRequest(BrowserAIRuntime* rt, uint64_t requested)
      : rt_(rt),
        id_(requested ? requested : rt->NewRequestId()),
        owned_(requested == 0) {}
  ~ScopedRequest() {
    if (owned_) rt_->FinishRequest(id_);
  }
  uint64_t id() const { return id_; }

 private:
  raw_ptr<BrowserAIRuntime> rt_;
  uint64_t id_;
  bool owned_;
};

}  // namespace

GenerationResult BrowserAIRuntime::RunPrompt(const std::string& prompt,
                                             const PromptOptions& options) {
  GenerationResult result = StreamPrompt(prompt, TokenCallback(), options);
  result.success = result.error_message.empty() &&
                   (!result.text.empty() || result.was_cancelled);
  return result;
}

GenerationResult BrowserAIRuntime::StreamPrompt(const std::string& prompt,
                                                TokenCallback callback,
                                                const PromptOptions& options) {
  // Raw-prompt path (agents/automation build their own prompt text; no chat
  // template). Chat UI traffic uses StreamChat.
  return Generate(/*is_chat=*/false, nullptr, &prompt, std::move(callback),
                  options);
}

GenerationResult BrowserAIRuntime::StreamChat(
    const std::vector<ChatMessage>& messages,
    TokenCallback callback,
    const PromptOptions& options) {
  ChatPrompt prompt;
  size_t first = 0;
  size_t last = messages.size();
  if (!messages.empty() && messages.front().role == "system") {
    prompt.system_prompt = messages.front().content;
    first = 1;
  }
  if (last > first && messages[last - 1].role == "user") {
    prompt.user_message = messages[last - 1].content;
    --last;
  }
  prompt.history.assign(messages.begin() + static_cast<std::ptrdiff_t>(first),
                        messages.begin() + static_cast<std::ptrdiff_t>(last));
  return StreamChat(prompt, std::move(callback), options);
}

GenerationResult BrowserAIRuntime::StreamChat(const ChatPrompt& chat,
                                              TokenCallback callback,
                                              const PromptOptions& options) {
  return Generate(/*is_chat=*/true, &chat, nullptr, std::move(callback),
                  options);
}

GenerationResult BrowserAIRuntime::Generate(bool is_chat,
                                            const ChatPrompt* chat,
                                            const std::string* raw_prompt,
                                            TokenCallback callback,
                                            const PromptOptions& options) {
  GenerationResult result;
  auto done = [&]() {
    if (callback) callback("", /*is_done=*/true);
  };
  if (!impl_->unsupported_reason.empty()) {
    result.error_message = impl_->unsupported_reason;
    done();
    return result;
  }

  ScopedRequest req(this, options.request_id);
  const auto start = Clock::now();
  if (impl_->IsCancelled(req.id())) {
    result.was_cancelled = true;
    done();
    return result;
  }

  // Pin the loaded model for the idle / pressure unloaders.
  std::string model_id;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    model_id = impl_->loaded_model_id;
    if (!model_id.empty() &&
        (is_chat || options.model_id.empty() || options.model_id == model_id)) {
      ++impl_->active_generations;
    }
  }
  if (model_id.empty()) {
    result.error_message = "No model loaded";
    done();
    return result;
  }
  if (!is_chat && !options.model_id.empty() && options.model_id != model_id) {
    result.error_message = "Model not loaded: " + options.model_id;
    done();
    return result;
  }
  struct ActiveScope {
    raw_ptr<Impl> impl;
    ~ActiveScope() {
      std::lock_guard<std::mutex> lock(impl->state_mutex);
      --impl->active_generations;
    }
  } active_scope{impl_.get()};

  StreamLimits limits;
  limits.max_visible_tokens =
      options.max_tokens > 0 ? static_cast<uint32_t>(options.max_tokens) : 0u;
  limits.max_reasoning_tokens =
      options.max_reasoning_tokens > 0
          ? static_cast<uint32_t>(options.max_reasoning_tokens)
          : 0u;
  limits.stop_sequences = StopSequencesForService(options.stop_sequences);

  auto mailbox = base::MakeRefCounted<InferenceMailbox>();
  impl_->SetRequestMailbox(req.id(), mailbox);
  const uint64_t token = req.id();
  bool posted;
  if (is_chat) {
    posted = impl_->PostToHost(base::BindOnce(
        [](uint64_t t, mojom::ChatRequestPtr r, mojom::GenerationParamsPtr p,
           StreamLimits l, scoped_refptr<InferenceMailbox> m,
           InferenceProcessHost* h) {
          h->StartChat(t, std::move(r), std::move(p), std::move(l),
                       std::move(m));
        },
        token, ToServiceChat(*chat), ToServiceParams(options),
        std::move(limits), mailbox));
  } else {
    auto raw = mojom::RawPromptRequest::New();
    raw->system_prompt = CapRequestText(options.system_prompt);
    raw->prompt = CapRequestText(*raw_prompt);
    posted = impl_->PostToHost(base::BindOnce(
        [](uint64_t t, mojom::RawPromptRequestPtr r,
           mojom::GenerationParamsPtr p, StreamLimits l,
           scoped_refptr<InferenceMailbox> m, InferenceProcessHost* h) {
          h->StartPrompt(t, std::move(r), std::move(p), std::move(l),
                         std::move(m));
        },
        token, std::move(raw), ToServiceParams(options), std::move(limits),
        mailbox));
  }

  InferenceOutcome outcome(InferenceOutcome::Kind::kShuttingDown);
  if (posted) {
    outcome = impl_->Wait(
        mailbox.get(), req.id(), token, kGenerationWait,
        [&](const std::string& text) {
          result.text += text;
          if (callback) callback(text, /*is_done=*/false);
        });
  }
  impl_->SetRequestMailbox(req.id(), nullptr);
  done();
  if (outcome.kind == InferenceOutcome::Kind::kNoModel) {
    // The process went away before the request reached it (a crash right
    // after loading, an unload): forget the model so the chat's next send
    // loads it again.
    std::string loaded;
    {
      std::lock_guard<std::mutex> lock(impl_->state_mutex);
      loaded = impl_->loaded_model_id;
    }
    if (loaded == model_id) {
      impl_->SetLoaded(std::string(), std::string(), std::string());
    }
  }

  result.model_used = model_id;
  result.error_message = GenerationError(outcome, &result);
  result.success = result.error_message.empty();
  result.generation_time_ms = std::chrono::duration<float, std::milli>(
                                  Clock::now() - start)
                                  .count();
  // Counts only, never text.
  VLOG(1) << "[MoltAI] " << (is_chat ? "StreamChat" : "StreamPrompt")
          << ": n_prompt=" << result.tokens_prompt
          << " reused=" << result.tokens_reused
          << " generated=" << result.tokens_generated
          << (result.was_cancelled ? " (cancelled)" : "");
  TouchActivity();
  return result;
}

HardwareCapability BrowserAIRuntime::GetHardwareCapability() const {
  return impl_->hardware;
}

size_t BrowserAIRuntime::GetModelMemoryUsage() const {
  std::lock_guard<std::mutex> lock(impl_->models_mutex);
  size_t total = 0;
  for (const auto& [id, info] : impl_->models) {
    if (info.is_loaded) {
      total += info.ram_required_bytes;
    }
  }
  return total;
}

void BrowserAIRuntime::RefreshModelStatus() {
  // Stat the files without holding the registry lock.
  std::vector<std::pair<std::string, std::string>> paths;
  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    for (const auto& [id, info] : impl_->models) {
      paths.emplace_back(id, info.file_path);
    }
  }
  std::vector<std::pair<std::string, bool>> present;
  for (const auto& [id, path] : paths) {
    std::error_code ec;
    present.emplace_back(id, std::filesystem::exists(path, ec));
  }
  std::lock_guard<std::mutex> lock(impl_->models_mutex);
  for (const auto& [id, exists] : present) {
    impl_->models[id].is_downloaded = exists;
  }
}

}  // namespace molt_ai
