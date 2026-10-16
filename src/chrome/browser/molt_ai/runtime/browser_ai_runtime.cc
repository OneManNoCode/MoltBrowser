// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>  // for int32_t/int64_t — required on Linux under -fmodules
#include <cstdlib>  // for getenv
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <ratio>
#include <thread>
#include <utility>
#include <vector>

// Chromium includes
#include "base/cpu.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/no_destructor.h"
#include "base/system/sys_info.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/runtime/context_budget.h"
#include "chrome/browser/molt_ai/runtime/prompt_utils.h"
#include "chrome/browser/molt_ai/runtime/stream_filters.h"

// llama.cpp headers. NOTE: on x64 parts of ggml are compiled with
// -mavx2/-mfma/-mf16c (see //third_party/llama_cpp/BUILD.gn); nothing in
// this file may call into llama/ggml before Initialize() has checked the
// CPU (impl_->unsupported_reason).
#include "third_party/llama_cpp/include/llama.h"

namespace molt_ai {

namespace {

// Default idle time after which the model is unloaded to give RAM back.
constexpr base::TimeDelta kDefaultIdleUnload = base::Minutes(10);

// Runtime overhead (compute buffers, a small KV cache, allocator slack) on
// top of the GGUF weights when deciding whether a model fits right now.
constexpr uint64_t kRuntimeOverheadBytes = 1024ULL * 1024 * 1024;

// A model is never allowed to need more than this share of physical RAM.
constexpr double kMaxShareOfTotalRam = 0.9;

// Prompt tokens kept free beyond the plan, for template/tokenizer drift.
constexpr int kContextSlack = 4;

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
#if defined(ARCH_CPU_X86_64)
  // //third_party/llama_cpp builds ggml's x86 kernels with -mavx2 -mfma
  // -mf16c. On a CPU without them the first quantized matmul raises SIGILL
  // and takes the whole browser process down. (Every shipping AVX2 CPU
  // also has F16C, which base::CPU doesn't expose.) Runtime ISA dispatch
  // (GGML_CPU_ALL_VARIANTS-style) is a separate, later project.
  const base::CPU& cpu = base::CPU::GetInstanceNoAllocation();
  if (!cpu.has_avx2() || !cpu.has_fma3()) {
    return "This CPU doesn't support AVX2, which on-device AI in "
           "MoltBrowser needs. Use a cloud model instead (AI Settings > "
           "Connect AI Providers).";
  }
#endif
  return std::string();
}

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

std::vector<llama_token> Tokenize(const llama_vocab* vocab,
                                  const std::string& text,
                                  bool add_special,
                                  bool parse_special) {
  std::vector<llama_token> tokens(text.size() + 16);
  int n = llama_tokenize(vocab, text.c_str(),
                         static_cast<int32_t>(text.size()), tokens.data(),
                         static_cast<int32_t>(tokens.size()), add_special,
                         parse_special);
  // INT32_MIN is an error (tokenization failed, e.g. malformed vocab data),
  // not a "buffer too small" size; negating it would overflow.
  if (n == INT32_MIN) return {};
  if (n < 0) {
    tokens.resize(static_cast<size_t>(-n));
    n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                       tokens.data(), static_cast<int32_t>(tokens.size()),
                       add_special, parse_special);
  }
  tokens.resize(n > 0 ? static_cast<size_t>(n) : 0);
  return tokens;
}

std::string Detokenize(const llama_vocab* vocab,
                       const std::vector<llama_token>& tokens) {
  if (tokens.empty()) return std::string();
  std::vector<char> buf(tokens.size() * 8 + 64);
  int n = llama_detokenize(vocab, tokens.data(),
                           static_cast<int32_t>(tokens.size()), buf.data(),
                           static_cast<int32_t>(buf.size()),
                           /*remove_special=*/false,
                           /*unparse_special=*/false);
  if (n == INT32_MIN) return std::string();  // error, see Tokenize()
  if (n < 0) {
    buf.resize(static_cast<size_t>(-n));
    n = llama_detokenize(vocab, tokens.data(),
                         static_cast<int32_t>(tokens.size()), buf.data(),
                         static_cast<int32_t>(buf.size()), false, false);
  }
  return n > 0 ? std::string(buf.data(), static_cast<size_t>(n))
               : std::string();
}

std::string ReadModelArch(const llama_model* model) {
  char buf[64] = {0};
  int n = llama_model_meta_val_str(model, "general.architecture", buf,
                                   sizeof(buf));
  return n > 0 ? std::string(buf, std::min<size_t>(static_cast<size_t>(n),
                                                   sizeof(buf) - 1))
               : std::string();
}

// Texts of the vocab's control-looking tokens, for neutralizing untrusted
// text before it is tokenized with parse_special=true.
std::vector<std::string> CollectSpecialTexts(const llama_vocab* vocab) {
  std::vector<std::string> out;
  const int32_t n_vocab = llama_vocab_n_tokens(vocab);
  for (llama_token t = 0; t < n_vocab; ++t) {
    const llama_token_attr attr = llama_vocab_get_attr(vocab, t);
    if (!(attr & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_USER_DEFINED))) {
      continue;
    }
    const char* text = llama_vocab_get_text(vocab, t);
    if (!text) continue;
    std::string s(text);
    if (LooksLikeSpecialTokenText(s)) out.push_back(std::move(s));
  }
  return out;
}

// Gemma 4 vocabularies define its thought-channel markers as special tokens
// (see GemmaThoughtFilter). Only those models' output goes through that
// filter, so another model's answer that mentions the markers is shown.
bool UsesGemmaThoughtChannel(const std::vector<std::string>& special_texts) {
  bool open = false;
  bool close = false;
  for (const std::string& s : special_texts) {
    open = open || s == GemmaThoughtFilter::kOpenMarker;
    close = close || s == GemmaThoughtFilter::kCloseMarker;
  }
  return open && close;
}

bool IsQwen3Arch(const std::string& arch) {
  return arch == "qwen3" || arch == "qwen3moe";
}

bool IsReasoningArch(const std::string& arch) {
  return IsQwen3Arch(arch) || arch == "qwen35" || arch == "qwen35moe" ||
         arch == "gpt-oss";
}

// Keep the first |limit| tokens of |text| (context excerpts are ranked
// best-first, so the head is the most relevant part).
std::string TruncateTextHead(const llama_vocab* vocab,
                             const std::string& text,
                             int limit) {
  if (limit <= 0) return std::string();
  std::vector<llama_token> t = Tokenize(vocab, text, false, false);
  if (static_cast<int>(t.size()) <= limit) return text;
  t.resize(static_cast<size_t>(limit));
  return Detokenize(vocab, t) + "\n[...]";
}

// Keep the head and tail of |text| within |limit| tokens.
std::string TruncateTextMiddle(const llama_vocab* vocab,
                               const std::string& text,
                               int limit) {
  std::vector<llama_token> t = Tokenize(vocab, text, false, false);
  if (limit <= 0 || static_cast<int>(t.size()) <= limit) return text;
  const std::vector<llama_token> marker =
      Tokenize(vocab, "\n[...]\n", false, false);
  return Detokenize(vocab,
                    TruncateTokensMiddle(t, static_cast<size_t>(limit),
                                         marker));
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

  // generation_mutex guards the live llama handles and the per-model state
  // below during use: RunPrompt / StreamPrompt / StreamChat hold it for
  // their whole run, and LoadModel/UnloadModel/Shutdown take it to swap or
  // free those handles — so a model can never be freed under an in-flight
  // decode or a chat-template read. load_mutex serializes LoadModel /
  // UnloadModel against each other and is held across the slow disk load
  // WITHOUT holding generation_mutex. Ordering is always load_mutex ->
  // generation_mutex; generation paths never take load_mutex. Fields
  // written during a swap are written with BOTH locks held.
  std::mutex generation_mutex;
  std::mutex load_mutex;
  std::atomic<bool> is_generating{false};
  std::atomic<bool> is_loading{false};
  bool initialized = false;
  // Non-empty => local inference must not touch llama/ggml at all.
  std::string unsupported_reason;
  std::string model_directory;

  // llama.cpp context handles — C library types, excluded from raw_ptr check
  RAW_PTR_EXCLUSION llama_model* llama_model_handle = nullptr;
  RAW_PTR_EXCLUSION llama_context* llama_ctx = nullptr;
  std::string loaded_model_id;  // Track which model is currently loaded
  std::string model_arch;       // GGUF general.architecture
  int n_batch = kMinContextTokens;
  std::vector<std::string> special_texts;
  // The loaded vocab has Gemma 4's thought-channel markers.
  bool thought_channels = false;
  // Tokens currently in the KV cache (sequence 0) and the conversation they
  // belong to; used to reuse the common prefix across turns.
  std::vector<llama_token> kv_tokens;
  std::string kv_key;

  // Per-request cancellation: id -> cancelled.
  mutable std::mutex requests_mutex;
  std::unordered_map<uint64_t, bool> requests;
  uint64_t next_request_id = 1;

  // Idle unload.
  std::atomic<uint64_t> activity_epoch{0};
  std::atomic<int64_t> idle_timeout_ms{kDefaultIdleUnload.InMilliseconds()};

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

  // Free llama.cpp resources for the currently loaded model. Caller holds
  // load_mutex and generation_mutex.
  void FreeLlamaResources() {
    if (llama_ctx) {
      llama_free(llama_ctx);
      llama_ctx = nullptr;
    }
    if (llama_model_handle) {
      llama_model_free(llama_model_handle);
      llama_model_handle = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(models_mutex);
      auto it = models.find(loaded_model_id);
      if (it != models.end()) it->second.is_loaded = false;
      if (active_model_id == loaded_model_id) active_model_id.clear();
    }
    loaded_model_id.clear();
    model_arch.clear();
    special_texts.clear();
    thought_channels = false;
    kv_tokens.clear();
    kv_key.clear();
  }

  bool IsCancelled(uint64_t id) const {
    if (id == 0) return false;
    std::lock_guard<std::mutex> lock(requests_mutex);
    auto it = requests.find(id);
    return it != requests.end() && it->second;
  }

  struct GenerationParams {
    int max_visible = 512;
    int max_hidden = 256;
    bool think_start_inside = false;
    // The prompt ends inside a Gemma 4 thought block.
    bool thought_start_inside = false;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    uint64_t request_id = 0;
  };

  struct GenerationOutcome {
    int n_generated = 0;
    int n_reused = 0;
    bool cancelled = false;
    std::string error;
  };

  // Decodes |prompt| (reusing the KV prefix shared with the previous call
  // when |reuse_key| matches) and samples until end-of-generation, budget,
  // cancellation or a full window. Visible text goes to |emit|. Caller
  // holds generation_mutex and has checked that the handles exist.
  GenerationOutcome GenerateLocked(
      const std::vector<llama_token>& prompt,
      const std::string& reuse_key,
      const GenerationParams& p,
      const std::function<void(const std::string&)>& emit) {
    GenerationOutcome out;
    const llama_vocab* vocab = llama_model_get_vocab(llama_model_handle);
    const int n_ctx = static_cast<int>(llama_n_ctx(llama_ctx));
    llama_memory_t mem = llama_get_memory(llama_ctx);
    if (!vocab || !mem || prompt.empty()) {
      out.error = "Model is not ready.";
      return out;
    }

    // ---- KV prefix reuse (A2/KV) ----
    size_t n_keep = 0;
    if (!reuse_key.empty() && reuse_key == kv_key) {
      n_keep = CommonPrefixLength(kv_tokens, prompt);
      // At least one prompt token must be decoded to get fresh logits.
      if (n_keep >= prompt.size()) n_keep = prompt.size() - 1;
    }
    if (n_keep > 0) {
      // Drop everything after the shared prefix. Bail out to a full
      // re-evaluation if the cache can't do a partial removal (recurrent
      // memory) or no longer holds the start of the sequence (SWA).
      if (!llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(n_keep), -1) ||
          llama_memory_seq_pos_min(mem, 0) > 0) {
        n_keep = 0;
      }
    }
    if (n_keep == 0) {
      llama_memory_clear(mem, true);
    }
    kv_tokens.assign(prompt.begin(),
                     prompt.begin() + static_cast<std::ptrdiff_t>(n_keep));
    kv_key = reuse_key;
    out.n_reused = static_cast<int>(n_keep);

    auto fail_decode = [&](const char* what) {
      // The cache state is unknown after a failed decode: start clean next
      // time.
      llama_memory_clear(mem, true);
      kv_tokens.clear();
      kv_key.clear();
      out.error = what;
    };

    // ---- Prompt evaluation in n_batch-sized chunks ----
    std::vector<llama_token> work(prompt.begin(), prompt.end());
    for (size_t i = n_keep; i < work.size();) {
      if (IsCancelled(p.request_id)) {
        out.cancelled = true;
        return out;  // kv_tokens matches exactly what was decoded
      }
      const size_t n = std::min<size_t>(static_cast<size_t>(n_batch),
                                        work.size() - i);
      llama_batch batch =
          llama_batch_get_one(work.data() + i, static_cast<int32_t>(n));
      if (llama_decode(llama_ctx, batch) != 0) {
        fail_decode("The model couldn't process the prompt.");
        return out;
      }
      kv_tokens.insert(kv_tokens.end(),
                       work.begin() + static_cast<std::ptrdiff_t>(i),
                       work.begin() + static_cast<std::ptrdiff_t>(i + n));
      i += n;
    }

    // ---- Sampling ----
    llama_sampler* sampler =
        llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(p.top_k));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(p.top_p, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(p.temperature));
    llama_sampler_chain_add(sampler,
                            llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    HarmonyChannelFilter harmony;
    // Gemma 4's thought channel, for vocabularies that define its markers.
    const bool gemma_thoughts = thought_channels;
    GemmaThoughtFilter gemma(gemma_thoughts && p.thought_start_inside);
    ThinkTagFilter think(p.think_start_inside);
    Utf8Assembler utf8;
    auto feed = [&](const std::string& piece) {
      std::string text = harmony.Feed(piece);
      if (gemma_thoughts) text = gemma.Feed(text);
      std::string visible = think.Feed(text);
      std::string complete = utf8.Push(visible);
      if (!complete.empty()) emit(complete);
    };
    auto hiding = [&]() {
      return harmony.hiding() || gemma.hiding() || think.in_think();
    };

    int visible_tokens = 0;
    int hidden_tokens = 0;
    bool forced_answer = false;

    while (true) {
      if (IsCancelled(p.request_id)) {
        out.cancelled = true;
        break;
      }
      // Never let the cache reach n_ctx (llama_decode would fail).
      if (static_cast<int>(kv_tokens.size()) >= n_ctx - 1) break;

      llama_token tok = llama_sampler_sample(sampler, llama_ctx, -1);
      llama_sampler_accept(sampler, tok);

      // Stop on ANY end-of-generation token (EOS, <|eot_id|>, <|im_end|>,
      // <|end|>, <|return|>, ...) BEFORE converting to text, so EOG token
      // text is never emitted.
      if (llama_vocab_is_eog(vocab, tok)) break;

      std::string piece = TokenToPiece(vocab, tok);
      if (piece.empty()) {
        // Reasoning tags may be CONTROL tokens that render empty without
        // special=true; recover their text so the filters see them.
        // (Gemma 4's markers are USER_DEFINED and render anyway; this
        // keeps a vocab that marks them CONTROL from leaking thoughts.)
        const char* raw = llama_vocab_get_text(vocab, tok);
        if (raw &&
            (std::strcmp(raw, ThinkTagFilter::kOpenTag) == 0 ||
             std::strcmp(raw, ThinkTagFilter::kCloseTag) == 0 ||
             (gemma_thoughts &&
              (std::strcmp(raw, GemmaThoughtFilter::kOpenMarker) == 0 ||
               std::strcmp(raw, GemmaThoughtFilter::kCloseMarker) == 0)))) {
          piece = raw;
        }
      }
      const bool hidden_before = hiding();
      feed(piece);
      if (hidden_before || hiding()) {
        ++hidden_tokens;
      } else {
        ++visible_tokens;
      }

      if (llama_decode(llama_ctx, llama_batch_get_one(&tok, 1)) != 0) {
        fail_decode("The model stopped unexpectedly while answering.");
        break;
      }
      kv_tokens.push_back(tok);
      ++out.n_generated;

      if (visible_tokens >= p.max_visible) break;

      if (hiding() && hidden_tokens >= p.max_hidden) {
        if (forced_answer) break;  // it went back to reasoning; give up
        // Reasoning budget spent: close the reasoning block / switch to
        // the final channel so the model answers instead of thinking on.
        forced_answer = true;
        const char* force =
            gemma.hiding()
                ? GemmaThoughtFilter::kForceClose
                : (think.in_think() ? ThinkTagFilter::kForceClose
                                    : HarmonyChannelFilter::kForceFinal);
        std::vector<llama_token> ft = Tokenize(vocab, force, false, true);
        if (ft.empty() ||
            static_cast<int>(kv_tokens.size() + ft.size()) >= n_ctx - 1) {
          break;
        }
        llama_batch force_batch =
            llama_batch_get_one(ft.data(), static_cast<int32_t>(ft.size()));
        if (llama_decode(llama_ctx, force_batch) != 0) {
          fail_decode("The model stopped unexpectedly while answering.");
          break;
        }
        kv_tokens.insert(kv_tokens.end(), ft.begin(), ft.end());
        for (llama_token t : ft) feed(TokenToPiece(vocab, t));
        // The marker closed one Gemma block; nested ones would keep the
        // answer hidden, so close them all (no-op when none is open).
        gemma.ForceClose();
        if (think.in_think()) {
          // Control-token form of the tags rendered empty; tell the filter
          // explicitly.
          feed(ThinkTagFilter::kCloseTag);
        }
        hidden_tokens = 0;
      }
    }

    llama_sampler_free(sampler);

    // Release held-back bytes in pipeline order (two statements: the
    // Gemma flush must reach the think filter before it flushes).
    std::string rest =
        gemma_thoughts ? think.Feed(gemma.Flush()) : std::string();
    rest += think.Flush();
    std::string tail = utf8.Push(rest);
    tail += utf8.Flush();
    if (!tail.empty()) emit(tail);
    return out;
  }
};

// Unloads the model on critical memory pressure (when idle).
class BrowserAIRuntime::PressureListener : public base::MemoryPressureListener {
 public:
  explicit PressureListener(BrowserAIRuntime* runtime) : runtime_(runtime) {}

 private:
  void OnMemoryPressure(base::MemoryPressureLevel level) override {
    if (level != base::MEMORY_PRESSURE_LEVEL_CRITICAL) return;
    // Unloading takes locks a running load may hold for a minute — never on
    // the UI thread.
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

  // Must run before ANY llama/ggml call (A9).
  impl_->unsupported_reason = ComputeUnsupportedReason();

  if (impl_->unsupported_reason.empty()) {
    // llama.cpp requires a one-time global backend init before any model
    // load. Registers compiled-in backends (Metal on Apple Silicon, CPU
    // elsewhere). No filesystem I/O — Metal device probing only.
    static std::once_flag llama_backend_once;
    std::call_once(llama_backend_once, []() {
      llama_backend_init();
      std::cerr << "[MoltAI] llama.cpp backend initialized" << std::endl;
    });
  } else {
    std::cerr << "[MoltAI] on-device inference disabled: "
              << impl_->unsupported_reason << std::endl;
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

  impl_->initialized = true;
  return true;
}

void BrowserAIRuntime::Shutdown() {
  if (!impl_->initialized) return;

  // Ask any in-flight decode to stop, then take the same locks LoadModel/Unload
  // take (load_mutex -> generation_mutex) before freeing the handles, so this
  // can never free a model out from under a running decode. (Unreachable today —
  // the singleton is never destroyed — but this keeps it safe if ever wired to
  // browser/profile teardown.)
  CancelGeneration();
  std::lock_guard<std::mutex> load_lock(impl_->load_mutex);
  std::lock_guard<std::mutex> gen_lock(impl_->generation_mutex);

  impl_->FreeLlamaResources();
  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    for (auto& [id, info] : impl_->models) {
      info.is_loaded = false;
    }
  }

  impl_->initialized = false;
}

std::string BrowserAIRuntime::GetLocalInferenceUnsupportedReason() const {
  return impl_->unsupported_reason;
}

void BrowserAIRuntime::StartMemoryPressureMonitoring() {
  if (impl_->pressure_registration) return;
  impl_->pressure_listener = std::make_unique<PressureListener>(this);
  // MemoryPressureListenerTag is a closed enum in //base and only feeds a
  // Finch suppression mask; reuse the on-device-model tag rather than fork
  // base/ for a new value. Critical-level notifications are never
  // suppressed by that mask's "1" setting.
  impl_->pressure_registration =
      std::make_unique<base::MemoryPressureListenerRegistration>(
          base::MemoryPressureListenerTag::kOnDeviceTailModelService,
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
  if (impl_->is_generating || impl_->is_loading) return;
  std::unique_lock<std::mutex> load_lock(impl_->load_mutex, std::try_to_lock);
  if (!load_lock.owns_lock()) return;
  std::unique_lock<std::mutex> gen_lock(impl_->generation_mutex,
                                        std::try_to_lock);
  if (!gen_lock.owns_lock()) return;
  if (epoch != impl_->activity_epoch.load() || !impl_->llama_model_handle) {
    return;
  }
  std::cerr << "[MoltAI] unloading idle model " << impl_->loaded_model_id
            << std::endl;
  impl_->FreeLlamaResources();
}

void BrowserAIRuntime::OnCriticalMemoryPressure() {
  if (impl_->is_generating || impl_->is_loading) return;
  std::unique_lock<std::mutex> load_lock(impl_->load_mutex, std::try_to_lock);
  if (!load_lock.owns_lock()) return;
  std::unique_lock<std::mutex> gen_lock(impl_->generation_mutex,
                                        std::try_to_lock);
  if (!gen_lock.owns_lock() || !impl_->llama_model_handle) return;
  std::cerr << "[MoltAI] critical memory pressure: unloading "
            << impl_->loaded_model_id << std::endl;
  impl_->FreeLlamaResources();
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

  // load_mutex serializes loads/unloads (never allocate two multi-GB models at
  // once → avoids OOM, and avoids swap races). CRUCIALLY, the slow part below —
  // the disk read + llama_init_from_model + Metal shader compile, 14-60s — runs
  // into LOCAL handles WITHOUT holding generation_mutex, so it never freezes an
  // in-flight decode or an on-UI-thread caller. generation_mutex is taken only
  // for the brief pointer swap, which waits for any active decode to finish
  // before the old model is detached and freed → no use-after-free.
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

  // load_mutex makes loaded_model_id stable here (all writers hold it).
  if (impl_->loaded_model_id == model_id && impl_->llama_model_handle) {
    TouchActivity();
    return true;
  }

  // RAM check against what is actually free (A8).
  const RamCheck rc = CheckRamForModel(model_id);
  if (!rc.ok) {
    const auto gb = [](uint64_t b) {
      return std::to_string((b + 512ULL * 1024 * 1024) /
                            (1024ULL * 1024 * 1024));
    };
    if (rc.exceeds_total) {
      set_error(name + " needs about " + gb(rc.needed_bytes) +
                " GB of memory, but this device has " + gb(rc.total_bytes) +
                " GB. Try a smaller model.");
    } else {
      set_error(name + " needs about " + gb(rc.needed_bytes) +
                " GB of free memory, but only about " +
                gb(rc.available_bytes) +
                " GB is available right now. Close some apps or tabs, or "
                "pick a smaller model.");
    }
    return false;
  }

  impl_->is_loading = true;

  // Free the old model FIRST when the new one wouldn't fit next to it —
  // loading new-before-free briefly needs both in RAM.
  {
    const uint64_t free_now =
        base::SysInfo::AmountOfAvailablePhysicalMemory().InBytes();
    if (free_now < rc.needed_bytes && impl_->llama_model_handle) {
      std::lock_guard<std::mutex> gen_lock(impl_->generation_mutex);
      std::cerr << "[MoltAI] freeing " << impl_->loaded_model_id
                << " before loading " << model_id << " (low memory)"
                << std::endl;
      impl_->FreeLlamaResources();
    }
  }

  // ---- llama.cpp model loading into LOCAL handles (no generation_mutex) ----
  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = -1;  // Offload all layers to GPU (Metal on macOS)

  llama_model* new_model = llama_model_load_from_file(
      info.file_path.c_str(), model_params);
  if (!new_model) {
    std::cerr << "[MoltAI] Failed to load model: " << info.file_path
              << std::endl;
    impl_->is_loading = false;
    set_error("Couldn't load " + name +
              ". The file may be incomplete or incompatible — try deleting "
              "and re-downloading it from the model menu.");
    return false;
  }

  // Size the context from the model's training window, RAM and a KV-cache
  // memory estimate (A2) instead of a fixed 2048.
  ContextSizingInput sizing_in;
  sizing_in.total_ram_bytes = impl_->hardware.total_ram_bytes;
  sizing_in.model_file_bytes = info.file_size_bytes;
  sizing_in.n_ctx_train = llama_model_n_ctx_train(new_model);
  sizing_in.kv_bytes_per_token = EstimateKvBytesPerToken(
      llama_model_n_layer(new_model), llama_model_n_embd(new_model),
      llama_model_n_head(new_model), llama_model_n_head_kv(new_model));
  const ContextSizing sizing = ComputeContextSizing(sizing_in);

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = static_cast<uint32_t>(sizing.n_ctx);
  // Prompts longer than n_batch are decoded in n_batch-sized chunks
  // (GenerateLocked), so n_batch no longer has to cover the whole window.
  ctx_params.n_batch = static_cast<uint32_t>(sizing.n_batch);
  ctx_params.n_threads = impl_->hardware.cpu_cores > 4
                             ? impl_->hardware.cpu_cores / 2
                             : std::max(1, impl_->hardware.cpu_cores);
  ctx_params.n_threads_batch = ctx_params.n_threads;

  llama_context* new_ctx = llama_init_from_model(new_model, ctx_params);
  if (!new_ctx && sizing.n_ctx > kMinContextTokens) {
    // The KV estimate is approximate (and GPU buffers can fail to
    // allocate); retry with the old fixed size before giving up.
    std::cerr << "[MoltAI] context of " << sizing.n_ctx
              << " failed; retrying with " << kMinContextTokens << std::endl;
    ctx_params.n_ctx = kMinContextTokens;
    ctx_params.n_batch = kMinContextTokens;
    new_ctx = llama_init_from_model(new_model, ctx_params);
  }
  if (!new_ctx) {
    std::cerr << "[MoltAI] Failed to create context for: " << model_id
              << std::endl;
    llama_model_free(new_model);
    impl_->is_loading = false;
    set_error("Couldn't start " + name + " — not enough memory for it right "
              "now. Close some apps or tabs, or pick a smaller model.");
    return false;
  }

  const llama_vocab* vocab = llama_model_get_vocab(new_model);
  std::vector<std::string> specials =
      vocab ? CollectSpecialTexts(vocab) : std::vector<std::string>();
  const bool thought_channels = UsesGemmaThoughtChannel(specials);
  std::string arch = ReadModelArch(new_model);

  // ---- Brief critical section: install the new handles, detach the old ----
  // Acquiring generation_mutex waits for any in-flight decode/template read to
  // finish; once we swap impl_->llama_ctx to the new one, every later generation
  // reads the new handles, so the old ones are unreferenced afterwards.
  llama_model* old_model = nullptr;
  llama_context* old_ctx = nullptr;
  {
    std::lock_guard<std::mutex> gen_lock(impl_->generation_mutex);
    old_ctx = impl_->llama_ctx;
    old_model = impl_->llama_model_handle;
    {
      std::lock_guard<std::mutex> lock(impl_->models_mutex);
      for (auto& [id, m] : impl_->models) m.is_loaded = (id == model_id);
      impl_->active_model_id = model_id;
    }
    impl_->llama_ctx = new_ctx;
    impl_->llama_model_handle = new_model;
    impl_->loaded_model_id = model_id;
    impl_->model_arch = std::move(arch);
    impl_->n_batch = static_cast<int>(llama_n_batch(new_ctx));
    impl_->special_texts = std::move(specials);
    impl_->thought_channels = thought_channels;
    // A new model invalidates any cached conversation prefix.
    impl_->kv_tokens.clear();
    impl_->kv_key.clear();
  }

  // Free the previous model OUTSIDE the lock — it is now detached (unreachable by
  // any current or future generation), so this can't race a decode, and freeing
  // a multi-GB model + its Metal buffers should not extend the critical section.
  if (old_ctx) {
    llama_free(old_ctx);
  }
  if (old_model) {
    llama_model_free(old_model);
  }

  impl_->is_loading = false;
  std::cerr << "[MoltAI] Model loaded: " << info.display_name
            << " (arch=" << impl_->model_arch
            << " ctx=" << llama_n_ctx(new_ctx)
            << " batch=" << impl_->n_batch << ")" << std::endl;
  TouchActivity();
  return true;
}

bool BrowserAIRuntime::UnloadModel(const std::string& model_id) {
  // Same serialization as LoadModel, same lock ordering (load_mutex ->
  // generation_mutex): never free llama resources while a generation or a load
  // is touching them. Callers (DeleteModel, the public API) hold neither lock,
  // so this cannot self-deadlock. Unload is fast, so holding generation_mutex
  // across it is fine (unlike LoadModel's slow disk read).
  std::lock_guard<std::mutex> load_lock(impl_->load_mutex);
  std::lock_guard<std::mutex> gen_lock(impl_->generation_mutex);

  {
    std::lock_guard<std::mutex> lock(impl_->models_mutex);
    if (impl_->models.find(model_id) == impl_->models.end()) return false;
  }
  if (impl_->loaded_model_id == model_id) {
    impl_->FreeLlamaResources();
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
  // Guarded by generation_mutex — loaded_model_id is written under it during the
  // brief model swap. Worker-thread only (see header).
  std::lock_guard<std::mutex> lock(impl_->generation_mutex);
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
  impl_->requests[id] = false;
  return id;
}

void BrowserAIRuntime::CancelRequest(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(impl_->requests_mutex);
  auto it = impl_->requests.find(request_id);
  if (it != impl_->requests.end()) it->second = true;
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
  for (auto& [id, cancelled] : impl_->requests) cancelled = true;
}

// ---- Inference -------------------------------------------------------------

namespace {

// RAII: allocates a private request id when the caller didn't pass one and
// releases it afterwards; marks the runtime busy for the idle/pressure
// unloaders.
class ScopedRequest {
 public:
  ScopedRequest(BrowserAIRuntime* rt,
                uint64_t requested,
                std::atomic<bool>* busy)
      : rt_(rt),
        id_(requested ? requested : rt->NewRequestId()),
        owned_(requested == 0),
        busy_(busy) {
    *busy_ = true;
  }
  ~ScopedRequest() {
    *busy_ = false;
    if (owned_) rt_->FinishRequest(id_);
  }
  uint64_t id() const { return id_; }

 private:
  raw_ptr<BrowserAIRuntime> rt_;
  uint64_t id_;
  bool owned_;
  raw_ptr<std::atomic<bool>> busy_;
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
  GenerationResult result;
  auto done = [&]() {
    if (callback) callback("", /*is_done=*/true);
  };
  if (!impl_->unsupported_reason.empty()) {
    result.error_message = impl_->unsupported_reason;
    done();
    return result;
  }

  std::lock_guard<std::mutex> lock(impl_->generation_mutex);
  ScopedRequest req(this, options.request_id, &impl_->is_generating);
  const auto start = std::chrono::steady_clock::now();

  const std::string model_id =
      options.model_id.empty() ? impl_->loaded_model_id : options.model_id;
  if (model_id.empty() || !impl_->llama_model_handle || !impl_->llama_ctx) {
    result.error_message = "No model loaded";
    done();
    return result;
  }
  if (model_id != impl_->loaded_model_id) {
    result.error_message = "Model not loaded: " + model_id;
    done();
    return result;
  }
  if (impl_->IsCancelled(req.id())) {
    result.was_cancelled = true;
    done();
    return result;
  }

  const llama_vocab* vocab = llama_model_get_vocab(impl_->llama_model_handle);
  const int n_ctx = static_cast<int>(llama_n_ctx(impl_->llama_ctx));
  const int visible =
      options.max_tokens > 0 ? options.max_tokens : AdaptiveReplyTokens(n_ctx);
  const int hidden = options.max_reasoning_tokens > 0
                         ? options.max_reasoning_tokens
                         : (IsReasoningArch(impl_->model_arch)
                                ? AdaptiveReasoningTokens(n_ctx)
                                : 64);
  const int reply_reserve = std::min(visible + hidden, n_ctx / 2);

  // The system prompt is kept whole (with BOS); the body is shortened in
  // the middle if needed (A2) — previously this failed outright.
  std::vector<llama_token> tokens;
  if (!options.system_prompt.empty()) {
    tokens = Tokenize(vocab, options.system_prompt + "\n\n",
                      /*add_special=*/true, /*parse_special=*/false);
  }
  std::vector<llama_token> body =
      Tokenize(vocab, prompt, /*add_special=*/tokens.empty(),
               /*parse_special=*/false);
  if (body.empty() && tokens.empty()) {
    result.error_message = "Failed to tokenize prompt";
    done();
    return result;
  }
  const int body_limit = n_ctx - kContextSlack - reply_reserve -
                         static_cast<int>(tokens.size());
  if (body_limit < 16) {
    result.error_message =
        "The system prompt is too long for this model's context window.";
    done();
    return result;
  }
  if (static_cast<int>(body.size()) > body_limit) {
    std::cerr << "[MoltAI] RunPrompt: prompt shortened from " << body.size()
              << " to " << body_limit << " tokens" << std::endl;
    body = TruncateTokensMiddle(body, static_cast<size_t>(body_limit),
                                Tokenize(vocab, "\n[...]\n", false, false));
  }
  tokens.insert(tokens.end(), body.begin(), body.end());
  result.tokens_prompt = static_cast<int>(tokens.size());

  Impl::GenerationParams gp;
  gp.max_visible = visible;
  gp.max_hidden = hidden;
  gp.temperature = options.temperature;
  gp.top_p = options.top_p;
  gp.top_k = options.top_k;
  gp.request_id = req.id();
  // Raw prompts never share a conversation prefix.
  Impl::GenerationOutcome outcome = impl_->GenerateLocked(
      tokens, /*reuse_key=*/std::string(), gp,
      [&](const std::string& visible_text) {
        result.text += visible_text;
        if (callback) callback(visible_text, /*is_done=*/false);
      });
  done();

  result.model_used = model_id;
  result.tokens_generated = outcome.n_generated;
  result.was_cancelled = outcome.cancelled;
  result.error_message = outcome.error;
  result.success = outcome.error.empty();
  result.generation_time_ms = std::chrono::duration<float, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  TouchActivity();
  return result;
}

std::string BrowserAIRuntime::ApplyChatTemplate(
    const std::vector<ChatMessage>& messages,
    bool add_assistant) const {
  // Fetch the template the model ships in its GGUF metadata
  // (tokenizer.chat_template). nullptr → llama_chat_apply_template
  // falls back to llama.cpp's built-in "chatml" format.
  const char* tmpl = nullptr;
  if (impl_->llama_model_handle) {
    tmpl = llama_model_chat_template(impl_->llama_model_handle,
                                     /*name=*/nullptr);
  }

  std::vector<llama_chat_message> chat;
  chat.reserve(messages.size());
  size_t total_chars = 0;
  for (const auto& m : messages) {
    chat.push_back({m.role.c_str(), m.content.c_str()});
    total_chars += m.role.size() + m.content.size();
  }

  // llama.h recommends allocating ~2x the total message chars up front;
  // if that still isn't enough, the return value is the required size.
  std::vector<char> buf(std::max<size_t>(total_chars * 2, 1024));
  int32_t n = llama_chat_apply_template(
      tmpl, chat.data(), chat.size(), add_assistant,
      buf.data(), static_cast<int32_t>(buf.size()));

  if (n < 0 && tmpl != nullptr) {
    // The GGUF's template isn't one of llama.cpp's recognized formats —
    // retry with the chatml default (tmpl=nullptr never fails).
    std::cerr << "[MoltAI] ApplyChatTemplate: model chat template "
                 "unrecognized, falling back to chatml" << std::endl;
    tmpl = nullptr;
    n = llama_chat_apply_template(
        tmpl, chat.data(), chat.size(), add_assistant,
        buf.data(), static_cast<int32_t>(buf.size()));
  }

  if (n > static_cast<int32_t>(buf.size())) {
    // Result was truncated (strncpy semantics) — resize to the reported
    // total byte count and format again.
    buf.resize(static_cast<size_t>(n));
    n = llama_chat_apply_template(
        tmpl, chat.data(), chat.size(), add_assistant,
        buf.data(), static_cast<int32_t>(buf.size()));
  }

  if (n < 0) {
    // Defensive last resort (should be unreachable — chatml is a
    // built-in): hand-format zephyr-style so generation can proceed.
    std::string out;
    for (const auto& m : messages) {
      out += "<|" + m.role + "|>\n" + m.content + "</s>\n";
    }
    if (add_assistant) {
      out += "<|assistant|>\n";
    }
    return out;
  }

  // Always use the returned byte count — the buffer is NOT
  // null-terminated when the result exactly fills it.
  return std::string(buf.data(), static_cast<size_t>(n));
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
  GenerationResult result;
  auto done = [&]() {
    if (callback) callback("", /*is_done=*/true);
  };
  if (!impl_->unsupported_reason.empty()) {
    result.error_message = impl_->unsupported_reason;
    done();
    return result;
  }

  // Hold generation_mutex across the token counting, the chat-template
  // build (which dereferences the model handle) AND the decode, so a
  // concurrent LoadModel can't free or swap the model in between (the
  // reported use-after-free), and the prompt is built and decoded against
  // the same model.
  std::lock_guard<std::mutex> lock(impl_->generation_mutex);
  ScopedRequest req(this, options.request_id, &impl_->is_generating);
  const auto start = std::chrono::steady_clock::now();

  if (!impl_->llama_model_handle || !impl_->llama_ctx) {
    result.error_message = "No model loaded";
    done();
    return result;
  }
  if (impl_->IsCancelled(req.id())) {
    result.was_cancelled = true;
    done();
    return result;
  }

  const llama_vocab* vocab = llama_model_get_vocab(impl_->llama_model_handle);
  const int n_ctx = static_cast<int>(llama_n_ctx(impl_->llama_ctx));
  const std::string& arch = impl_->model_arch;

  // ---- Reasoning control (A3) ----
  std::string system_text;
  if (arch == "gpt-oss") {
    // Harmony reads the reasoning effort from the system message.
    system_text = options.reasoning == ReasoningMode::kOn
                      ? "Reasoning: medium\n\n"
                      : "Reasoning: low\n\n";
  }
  system_text += chat.system_prompt;
  if (IsQwen3Arch(arch) && options.reasoning == ReasoningMode::kOff) {
    // Qwen3 soft switch. Kept in the (stable) system message so it doesn't
    // break KV-prefix reuse across turns. Qwen3 still emits an empty
    // <think></think> block, which the stream filter hides.
    system_text += "\n/no_think";
  }

  const int visible =
      options.max_tokens > 0 ? options.max_tokens : AdaptiveReplyTokens(n_ctx);
  int hidden = 64;  // room for an empty <think></think> block
  if (options.max_reasoning_tokens > 0) {
    hidden = options.max_reasoning_tokens;
  } else if (options.reasoning == ReasoningMode::kOn || arch == "gpt-oss") {
    hidden = AdaptiveReasoningTokens(n_ctx);
  }

  // ---- Untrusted text can't smuggle in control tokens ----
  const std::vector<std::string>& specials = impl_->special_texts;
  const std::string context = NeutralizeSpecialTokens(chat.context, specials);
  const std::string user = NeutralizeSpecialTokens(chat.user_message, specials);
  std::vector<ChatMessage> history;
  history.reserve(chat.history.size());
  for (const ChatMessage& m : chat.history) {
    if (m.content.empty()) continue;
    if (m.role != "user" && m.role != "assistant") continue;
    history.push_back({m.role, NeutralizeSpecialTokens(m.content, specials)});
  }

  // ---- Token budget (A2): system never trimmed ----
  auto count = [&](const std::string& s) {
    return static_cast<int>(Tokenize(vocab, s, false, false).size());
  };
  PromptBudgetInput in;
  in.n_ctx = n_ctx;
  in.reply_reserve = visible + hidden;
  in.system_tokens =
      count(system_text) + (context.empty() ? 0 : count(chat.context_preamble));
  in.context_tokens = context.empty() ? 0 : count(context);
  in.user_tokens = count(user);
  for (const ChatMessage& m : history) {
    in.history_tokens.push_back(count(m.content));
    in.history_starts_turn.push_back(m.role == "user");
  }
  {
    // Template overhead per message, measured on two empty messages.
    const std::string empty_chat = ApplyChatTemplate(
        {{"system", ""}, {"user", ""}}, /*add_assistant=*/true);
    in.per_message_overhead =
        static_cast<int>(Tokenize(vocab, empty_chat, true, true).size()) / 2 +
        1;
  }

  std::vector<llama_token> tokens;
  int reply_tokens = 0;
  bool fitted = false;
  for (int attempt = 0; attempt < 4 && !fitted; ++attempt) {
    const PromptBudgetPlan plan = PlanPromptBudget(in);
    if (!plan.fits) break;

    std::string ctx_text = context;
    if (plan.context_token_limit >= 0) {
      ctx_text = TruncateTextHead(vocab, context, plan.context_token_limit);
    }
    std::string user_text = user;
    if (plan.user_token_limit >= 0) {
      user_text = TruncateTextMiddle(vocab, user, plan.user_token_limit);
    }

    std::vector<ChatMessage> messages;
    messages.push_back(
        {"system",
         ctx_text.empty() ? system_text
                          : system_text + chat.context_preamble + ctx_text});
    for (size_t i = plan.history_keep_from; i < history.size(); ++i) {
      messages.push_back(history[i]);
    }
    messages.push_back({"user", user_text});

    // parse_special=true: the template's control markers (<|im_end|>,
    // <|eot_id|>, </s>, ...) must become real special tokens, not plain
    // text. Message contents were neutralized above.
    tokens = Tokenize(vocab, ApplyChatTemplate(messages, true),
                      /*add_special=*/true, /*parse_special=*/true);
    const int over = static_cast<int>(tokens.size()) + plan.reply_tokens +
                     kContextSlack - n_ctx;
    if (over <= 0) {
      fitted = true;
      reply_tokens = plan.reply_tokens;
      if (plan.history_keep_from > 0 || plan.context_token_limit >= 0 ||
          plan.user_token_limit >= 0) {
        std::cerr << "[MoltAI] StreamChat: fitted to n_ctx=" << n_ctx
                  << " (dropped " << plan.history_keep_from
                  << " history msgs, context_limit=" << plan.context_token_limit
                  << ", user_limit=" << plan.user_token_limit << ")"
                  << std::endl;
      }
    } else {
      // Per-message estimates undershot the real template; tighten and
      // re-plan (never by cutting the system prompt).
      in.safety_margin += over + 16;
    }
  }
  if (!fitted || tokens.empty()) {
    result.error_message =
        "This conversation is too long for the model's context window. "
        "Start a new chat or shorten your message.";
    done();
    return result;
  }
  result.tokens_prompt = static_cast<int>(tokens.size());

  // Some templates pre-open the reasoning block in the prompt.
  const std::string tail_text = Detokenize(
      vocab, std::vector<llama_token>(
                 tokens.end() - std::min<std::ptrdiff_t>(
                                    4, static_cast<std::ptrdiff_t>(
                                           tokens.size())),
                 tokens.end()));

  Impl::GenerationParams gp;
  // Visible and hidden budgets both come out of the reply reserve; when the
  // window is tight, shrink the hidden part first.
  gp.max_visible = std::min(visible, reply_tokens);
  gp.max_hidden = std::max(16, std::min(hidden, reply_tokens - gp.max_visible));
  gp.think_start_inside =
      tail_text.find(ThinkTagFilter::kOpenTag) != std::string::npos &&
      tail_text.find(ThinkTagFilter::kCloseTag) == std::string::npos;
  // Gemma 4 with thinking off pre-fills an empty, closed thought block
  // ("<|channel>thought\n<channel|>"); a template that leaves one open
  // starts the stream hidden.
  gp.thought_start_inside = GemmaThoughtFilter::EndsInsideThought(tail_text);
  gp.temperature = options.temperature;
  gp.top_p = options.top_p;
  gp.top_k = options.top_k;
  gp.request_id = req.id();

  Impl::GenerationOutcome outcome = impl_->GenerateLocked(
      tokens, options.conversation_key, gp,
      [&](const std::string& visible_text) {
        result.text += visible_text;
        if (callback) callback(visible_text, /*is_done=*/false);
      });
  done();

  result.model_used = impl_->loaded_model_id;
  result.tokens_generated = outcome.n_generated;
  result.tokens_reused = outcome.n_reused;
  result.was_cancelled = outcome.cancelled;
  result.error_message = outcome.error;
  result.success = outcome.error.empty();
  result.generation_time_ms = std::chrono::duration<float, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  std::cerr << "[MoltAI] StreamChat: n_ctx=" << n_ctx
            << " n_prompt=" << result.tokens_prompt
            << " reused=" << outcome.n_reused
            << " generated=" << outcome.n_generated
            << (outcome.cancelled ? " (cancelled)" : "") << std::endl;
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
