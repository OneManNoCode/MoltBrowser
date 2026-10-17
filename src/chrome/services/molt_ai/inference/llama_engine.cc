// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Exceptions: llama.cpp is built with exceptions, this file isn't, so an
// exception escaping a llama_* call aborts the inference process (not the
// browser any more, but still a crash). Every llama_* function used here was
// checked at b11641:
//  - llama_model_load_from_file_ptr, llama_init_from_model: catch everything
//    and return nullptr.
//  - llama_tokenize, llama_token_to_piece, llama_detokenize: catch (local
//    patch, see third_party/llama_cpp/README.chromium) and return an error.
//  - llama_chat_apply_template: unknown templates return -1 (local patch:
//    find() instead of map::at()).
//  - llama_vocab_get_text / llama_vocab_get_attr use id_to_token.at(): only
//    called with ids in [0, llama_vocab_n_tokens) or ids the sampler
//    returned.
//  - The rest (decode, sampler chain, memory, metadata getters) don't throw
//    on the inputs used here.
// Re-check these when updating the vendored llama.cpp.

#include "chrome/services/molt_ai/inference/llama_engine.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <utility>

#include "chrome/services/molt_ai/inference/engine/context_budget.h"
#include "chrome/services/molt_ai/inference/engine/prompt_utils.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "third_party/llama_cpp/include/llama.h"

namespace molt_ai {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

// Prompt tokens kept free beyond the plan, for template/tokenizer drift.
constexpr int kContextSlack = 4;

// Memory budget under a sandbox limit (design section 2.5): weights that may
// be repacked into anonymous memory (at most the whole file), a minimal KV
// cache and compute buffers must fit in this share of the limit.
constexpr uint64_t kBudgetPercentOfLimit = 85;
constexpr uint64_t kMinKvReserveBytes = 1 * kGiB;
constexpr uint64_t kComputeReserveBytes = 1 * kGiB;

using Clock = std::chrono::steady_clock;

bool SeekFile(FILE* f, int64_t offset, int whence) {
#if defined(_WIN32)
  return _fseeki64(f, offset, whence) == 0;
#else
  return fseeko(f, static_cast<off_t>(offset), whence) == 0;
#endif
}

int64_t TellFile(FILE* f) {
#if defined(_WIN32)
  return _ftelli64(f);
#else
  return static_cast<int64_t>(ftello(f));
#endif
}

// Convert a single token to its text piece.
std::string TokenToPiece(const llama_vocab* vocab, llama_token token) {
  char buf[256];
  int n = llama_token_to_piece(vocab, token, buf, sizeof(buf), 0, false);
  if (n < 0) {
    // Buffer too small: -n is the size needed. INT32_MIN would be an error.
    if (n == INT32_MIN) return std::string();
    std::vector<char> large_buf(static_cast<size_t>(-n));
    n = llama_token_to_piece(vocab, token, large_buf.data(),
                             static_cast<int32_t>(large_buf.size()), 0, false);
    if (n > 0) {
      return std::string(large_buf.data(), static_cast<size_t>(n));
    }
    return std::string();
  }
  return std::string(buf, static_cast<size_t>(n));
}

// Tokenizes |text|; nullopt on a tokenizer error (as opposed to an empty
// result for empty text).
std::optional<std::vector<llama_token>> TryTokenize(const llama_vocab* vocab,
                                                    const std::string& text,
                                                    bool add_special,
                                                    bool parse_special) {
  if (text.size() > static_cast<size_t>(INT32_MAX) - 16) return std::nullopt;
  std::vector<llama_token> tokens(text.size() + 16);
  int n = llama_tokenize(vocab, text.c_str(),
                         static_cast<int32_t>(text.size()), tokens.data(),
                         static_cast<int32_t>(tokens.size()), add_special,
                         parse_special);
  // INT32_MIN is an error (tokenization failed, e.g. malformed vocab data),
  // not a "buffer too small" size; negating it would overflow.
  if (n == INT32_MIN) return std::nullopt;
  if (n < 0) {
    tokens.resize(static_cast<size_t>(-n));
    n = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                       tokens.data(), static_cast<int32_t>(tokens.size()),
                       add_special, parse_special);
    if (n < 0) return std::nullopt;
  }
  tokens.resize(static_cast<size_t>(n));
  return tokens;
}

std::vector<llama_token> TokenizeText(const llama_vocab* vocab,
                                  const std::string& text,
                                  bool add_special,
                                  bool parse_special) {
  std::optional<std::vector<llama_token>> t =
      TryTokenize(vocab, text, add_special, parse_special);
  return t ? std::move(*t) : std::vector<llama_token>();
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
  if (n == INT32_MIN) return std::string();  // error, see TryTokenize()
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
  std::vector<llama_token> t = TokenizeText(vocab, text, false, false);
  if (static_cast<int>(t.size()) <= limit) return text;
  t.resize(static_cast<size_t>(limit));
  return Detokenize(vocab, t) + "\n[...]";
}

// Keep the head and tail of |text| within |limit| tokens.
std::string TruncateTextMiddle(const llama_vocab* vocab,
                               const std::string& text,
                               int limit) {
  std::vector<llama_token> t = TokenizeText(vocab, text, false, false);
  if (limit <= 0 || static_cast<int>(t.size()) <= limit) return text;
  const std::vector<llama_token> marker =
      TokenizeText(vocab, "\n[...]\n", false, false);
  return Detokenize(vocab,
                    TruncateTokensMiddle(t, static_cast<size_t>(limit),
                                         marker));
}

uint32_t ToU32(size_t v) {
  return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
}

uint32_t ToU32(int v) {
  return v < 0 ? 0u : static_cast<uint32_t>(v);
}

// Sends |text| (valid UTF-8) in pieces of at most kMaxChunkBytes, cut on
// character boundaries.
void EmitChunks(std::string text, const EngineCallbacks& callbacks) {
  if (text.empty() || !callbacks.on_chunk) return;
  size_t pos = 0;
  while (text.size() - pos > LlamaEngine::kMaxChunkBytes) {
    size_t cut = pos + LlamaEngine::kMaxChunkBytes;
    while (cut > pos && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
      --cut;  // don't split a character
    }
    if (cut == pos) cut = pos + LlamaEngine::kMaxChunkBytes;  // unreachable
    callbacks.on_chunk(text.substr(pos, cut - pos));
    pos = cut;
  }
  if (pos == 0) {
    callbacks.on_chunk(std::move(text));
  } else if (pos < text.size()) {
    callbacks.on_chunk(text.substr(pos));
  }
}

}  // namespace

void StdioFileCloser::operator()(FILE* file) const {
  if (file) fclose(file);
}

EngineCallbacks::EngineCallbacks() = default;
EngineCallbacks::~EngineCallbacks() = default;
EngineCallbacks::EngineCallbacks(EngineCallbacks&&) = default;
EngineCallbacks& EngineCallbacks::operator=(EngineCallbacks&&) = default;

struct LlamaEngine::Model {
  ~Model() {
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    // Only now: lazily read tensors go through this stream.
    file.reset();
  }

  ScopedStdioFile file;
  llama_model* model = nullptr;
  llama_context* ctx = nullptr;
  const llama_vocab* vocab = nullptr;
  std::string arch;
  int n_ctx = 0;
  int n_batch = kMinContextTokens;
  std::vector<std::string> special_texts;
  // The vocab has Gemma 4's thought-channel markers.
  bool thought_channels = false;
  // Tokens currently in the KV cache (sequence 0) and the conversation they
  // belong to; used to reuse the common prefix across turns.
  std::vector<llama_token> kv_tokens;
  std::string kv_key;
};

struct LlamaEngine::DecodeParams {
  int max_visible = 512;
  int max_hidden = 256;
  bool think_start_inside = false;
  // The prompt ends inside a Gemma 4 thought block.
  bool thought_start_inside = false;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  std::vector<std::string> stop_sequences;
};

// static
void LlamaEngine::InitBackendOnce() {
  static std::once_flag once;
  std::call_once(once, []() { llama_backend_init(); });
}

LlamaEngine::LlamaEngine() = default;
LlamaEngine::~LlamaEngine() = default;

bool LlamaEngine::loaded() const {
  return model_ && model_->ctx;
}

EngineLoadResult LlamaEngine::Load(ScopedStdioFile scoped_file,
                                   const EngineLoadParams& params) {
  EngineLoadResult result;
  if (!scoped_file || model_) return result;  // closes |scoped_file|
  auto m = std::make_unique<Model>();
  FILE* const file = scoped_file.get();
  m->file = std::move(scoped_file);  // closed by ~Model on every path below

  // The handle must be the file the browser opened and checked.
  if (!SeekFile(file, 0, SEEK_END)) return result;
  const int64_t size = TellFile(file);
  if (size < 0 || static_cast<uint64_t>(size) != params.file_size ||
      !SeekFile(file, 0, SEEK_SET)) {
    return result;
  }

  InitBackendOnce();
  if (ggml_backend_dev_count() == 0) {
    result.status = EngineLoadStatus::kBackendUnavailable;
    return result;
  }

  // Memory budget under the sandbox limit (design section 2.5). CPU weight
  // repacking copies the weights into anonymous memory, which the limit
  // counts (the read-only file mapping doesn't).
  const uint64_t limit = params.memory_limit_bytes;
  const uint64_t budget = limit / 100 * kBudgetPercentOfLimit;
  bool repack = params.allow_cpu_repack;
  if (limit > 0 && params.file_size + kMinKvReserveBytes +
                           kComputeReserveBytes >
                       budget) {
    repack = false;
  }

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = -1;  // all layers on the GPU (Metal on macOS)
  model_params.use_extra_bufts = repack;
  m->model = llama_model_load_from_file_ptr(file, model_params);
  if (!m->model) return result;  // kInvalidModelFile

  const bool gpu = llama_supports_gpu_offload();
  // Size the context from the model's training window, RAM and a KV-cache
  // estimate, then cap it by the browser's limit and the sandbox budget.
  ContextSizingInput sizing_in;
  sizing_in.total_ram_bytes = params.total_ram_bytes;
  sizing_in.model_file_bytes = params.file_size;
  sizing_in.n_ctx_train = llama_model_n_ctx_train(m->model);
  sizing_in.kv_bytes_per_token = EstimateKvBytesPerToken(
      llama_model_n_layer(m->model), llama_model_n_embd(m->model),
      llama_model_n_head(m->model), llama_model_n_head_kv(m->model));
  const ContextSizing sizing = ComputeContextSizing(sizing_in);
  // ComputeContextSizing never goes below this.
  const int floor = sizing_in.n_ctx_train > 0
                        ? std::min(kMinContextTokens, sizing_in.n_ctx_train)
                        : kMinContextTokens;
  int n_ctx = sizing.n_ctx;
  if (limit > 0 && sizing_in.kv_bytes_per_token > 0) {
    // What the KV cache may use under the sandbox limit. Below the floor
    // the context is tried anyway; if it can't be allocated the load fails
    // with kOutOfMemory.
    const uint64_t used =
        (repack && !gpu ? params.file_size : 0) + kComputeReserveBytes;
    const uint64_t kv_room = budget > used ? budget - used : 0;
    const uint64_t by_limit = kv_room / sizing_in.kv_bytes_per_token;
    if (by_limit < static_cast<uint64_t>(n_ctx)) {
      n_ctx = std::max(floor, static_cast<int>(by_limit - by_limit % 256));
    }
  }
  // The browser's cap wins (it accepts nothing above it).
  const int cap = params.max_context_tokens > 0
                      ? static_cast<int>(std::min<uint32_t>(
                            params.max_context_tokens, INT32_MAX))
                      : INT32_MAX;
  n_ctx = std::max(1, std::min(n_ctx, cap));

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = static_cast<uint32_t>(n_ctx);
  // Prompts longer than n_batch are decoded in n_batch-sized chunks.
  ctx_params.n_batch = static_cast<uint32_t>(std::min(n_ctx, sizing.n_batch));
  const int threads =
      static_cast<int>(std::clamp<uint32_t>(params.cpu_threads, 1, 256));
  ctx_params.n_threads = threads;
  ctx_params.n_threads_batch = threads;

  m->ctx = llama_init_from_model(m->model, ctx_params);
  const int retry_ctx = std::min(kMinContextTokens, cap);
  if (!m->ctx && n_ctx > retry_ctx) {
    // The KV estimate is approximate (and GPU buffers can fail to
    // allocate); retry with the old fixed size before giving up.
    ctx_params.n_ctx = static_cast<uint32_t>(retry_ctx);
    ctx_params.n_batch = static_cast<uint32_t>(retry_ctx);
    m->ctx = llama_init_from_model(m->model, ctx_params);
  }
  if (!m->ctx) {
    result.status = EngineLoadStatus::kOutOfMemory;
    return result;
  }

  m->vocab = llama_model_get_vocab(m->model);
  if (!m->vocab) return result;
  m->special_texts = CollectSpecialTexts(m->vocab);
  m->thought_channels = UsesGemmaThoughtChannel(m->special_texts);
  m->arch = ReadModelArch(m->model);
  m->n_ctx = static_cast<int>(llama_n_ctx(m->ctx));
  m->n_batch = static_cast<int>(llama_n_batch(m->ctx));
  if (m->n_batch <= 0) m->n_batch = 1;

  result.status = EngineLoadStatus::kOk;
  result.description.architecture = m->arch;
#if defined(GGML_USE_METAL)
  result.description.backend = gpu ? "metal" : "cpu";
#else
  result.description.backend = "cpu";
#endif
  result.description.context_tokens = ToU32(m->n_ctx);
  result.description.train_context_tokens = ToU32(sizing_in.n_ctx_train);
  result.description.vocab_size = ToU32(llama_vocab_n_tokens(m->vocab));
  result.description.cpu_repack_used = repack && !gpu;
  model_ = std::move(m);
  return result;
}

std::optional<std::vector<int32_t>> LlamaEngine::Tokenize(
    const std::string& text,
    size_t max_text_bytes) const {
  if (!loaded() || text.size() > max_text_bytes) return std::nullopt;
  return TryTokenize(model_->vocab, text, /*add_special=*/false,
                     /*parse_special=*/false);
}

void LlamaEngine::ForgetConversation(const std::string& conversation_key) {
  if (!loaded() || conversation_key.empty() ||
      model_->kv_key != conversation_key) {
    return;
  }
  llama_memory_t mem = llama_get_memory(model_->ctx);
  if (mem) llama_memory_clear(mem, true);
  model_->kv_tokens.clear();
  model_->kv_key.clear();
}

EngineSummary LlamaEngine::Raw(const EngineRawRequest& request,
                               const EngineGenerationParams& params,
                               const EngineCallbacks& callbacks) {
  // Raw-prompt path (agents/automation build their own prompt text; no chat
  // template).
  EngineSummary summary;
  if (!loaded()) return summary;  // kDecodeFailed
  Model& m = *model_;
  const int n_ctx = m.n_ctx;
  const int visible = params.max_visible_tokens > 0
                          ? static_cast<int>(std::min<uint32_t>(
                                params.max_visible_tokens, INT32_MAX / 4))
                          : AdaptiveReplyTokens(n_ctx);
  const int hidden = params.max_reasoning_tokens > 0
                         ? static_cast<int>(std::min<uint32_t>(
                               params.max_reasoning_tokens, INT32_MAX / 4))
                         : (IsReasoningArch(m.arch)
                                ? AdaptiveReasoningTokens(n_ctx)
                                : 64);
  const int reply_reserve = std::min(visible + hidden, n_ctx / 2);

  // Agent and automation prompts carry page text. parse_special=false is not
  // enough on its own: llama.cpp still matches USER_DEFINED tokens (only
  // CONTROL and UNKNOWN are skipped), and Gemma 4 defines its turn, channel
  // and tool markers (<|channel>, <|tool_response>, ...) that way, so a page
  // could end the turn or fake a tool result. Neutralize every
  // control-looking vocab text first, as Chat() does for its messages.
  const std::string system_prompt =
      NeutralizeSpecialTokens(request.system_prompt, m.special_texts);
  const std::string body_text =
      NeutralizeSpecialTokens(request.prompt, m.special_texts);

  // The system prompt is kept whole (with BOS); the body is shortened in
  // the middle if needed.
  std::vector<llama_token> tokens;
  if (!system_prompt.empty()) {
    std::optional<std::vector<llama_token>> sys =
        TryTokenize(m.vocab, system_prompt + "\n\n",
                    /*add_special=*/true, /*parse_special=*/false);
    if (!sys) {
      summary.status = EngineStatus::kTokenizeFailed;
      return summary;
    }
    tokens = std::move(*sys);
  }
  std::optional<std::vector<llama_token>> body =
      TryTokenize(m.vocab, body_text, /*add_special=*/tokens.empty(),
                  /*parse_special=*/false);
  if (!body || (body->empty() && tokens.empty())) {
    summary.status = EngineStatus::kTokenizeFailed;
    return summary;
  }
  const int body_limit = n_ctx - kContextSlack - reply_reserve -
                         static_cast<int>(tokens.size());
  if (body_limit < 16) {
    summary.status = EngineStatus::kSystemPromptTooLong;
    return summary;
  }
  if (static_cast<int>(body->size()) > body_limit) {
    *body = TruncateTokensMiddle(*body, static_cast<size_t>(body_limit),
                                 TokenizeText(m.vocab, "\n[...]\n", false, false));
    summary.user_message_trimmed = true;
  }
  tokens.insert(tokens.end(), body->begin(), body->end());
  summary.prompt_tokens = ToU32(tokens.size());

  DecodeParams dp;
  dp.max_visible = visible;
  dp.max_hidden = hidden;
  dp.temperature = params.temperature;
  dp.top_p = params.top_p;
  dp.top_k = static_cast<int>(std::min<uint32_t>(params.top_k, INT32_MAX));
  dp.stop_sequences = params.stop_sequences;
  // Raw prompts never share a conversation prefix.
  summary.status =
      Generate(tokens, /*reuse_key=*/std::string(), dp, callbacks, &summary);
  return summary;
}

EngineSummary LlamaEngine::Chat(const EngineChatRequest& chat,
                                const EngineGenerationParams& params,
                                const EngineCallbacks& callbacks) {
  EngineSummary summary;
  if (!loaded()) return summary;  // kDecodeFailed
  Model& m = *model_;
  const llama_vocab* vocab = m.vocab;
  const int n_ctx = m.n_ctx;
  const std::string& arch = m.arch;

  // ---- Reasoning control ----
  std::string system_text;
  if (arch == "gpt-oss") {
    // Harmony reads the reasoning effort from the system message.
    system_text =
        params.reasoning_on ? "Reasoning: medium\n\n" : "Reasoning: low\n\n";
  }
  system_text += chat.system_prompt;
  if (IsQwen3Arch(arch) && !params.reasoning_on) {
    // Qwen3 soft switch. Kept in the (stable) system message so it doesn't
    // break KV-prefix reuse across turns. Qwen3 still emits an empty
    // <think></think> block, which the stream filter hides.
    system_text += "\n/no_think";
  }

  const int visible = params.max_visible_tokens > 0
                          ? static_cast<int>(std::min<uint32_t>(
                                params.max_visible_tokens, INT32_MAX / 4))
                          : AdaptiveReplyTokens(n_ctx);
  int hidden = 64;  // room for an empty <think></think> block
  if (params.max_reasoning_tokens > 0) {
    hidden = static_cast<int>(
        std::min<uint32_t>(params.max_reasoning_tokens, INT32_MAX / 4));
  } else if (params.reasoning_on || arch == "gpt-oss") {
    hidden = AdaptiveReasoningTokens(n_ctx);
  }

  // ---- Untrusted text can't smuggle in control tokens ----
  const std::vector<std::string>& specials = m.special_texts;
  const std::string context = NeutralizeSpecialTokens(chat.context, specials);
  const std::string user = NeutralizeSpecialTokens(chat.user_message, specials);
  std::vector<std::pair<std::string, std::string>> history;  // role, text
  history.reserve(chat.history.size());
  for (const EngineChatTurn& turn : chat.history) {
    if (turn.content.empty()) continue;
    history.emplace_back(turn.from_user ? "user" : "assistant",
                         NeutralizeSpecialTokens(turn.content, specials));
  }

  // ---- Token budget: the system prompt is never trimmed ----
  auto count = [&](const std::string& s) {
    return static_cast<int>(TokenizeText(vocab, s, false, false).size());
  };
  PromptBudgetInput in;
  in.n_ctx = n_ctx;
  in.reply_reserve = visible + hidden;
  in.system_tokens =
      count(system_text) + (context.empty() ? 0 : count(chat.context_preamble));
  in.context_tokens = context.empty() ? 0 : count(context);
  in.user_tokens = count(user);
  for (const auto& [role, text] : history) {
    in.history_tokens.push_back(count(text));
    in.history_starts_turn.push_back(role == "user");
  }

  // Formats messages with the model's own chat template (the GGUF's
  // tokenizer.chat_template, via llama_chat_apply_template), falling back to
  // llama.cpp's built-in chatml when it ships none or an unrecognized one.
  // nullopt if the conversation is too large to format at all.
  auto apply_template =
      [&](const std::vector<std::pair<std::string, std::string>>& messages)
      -> std::optional<std::string> {
        const char* tmpl = llama_model_chat_template(m.model, /*name=*/nullptr);
        std::vector<llama_chat_message> msgs;
        msgs.reserve(messages.size());
        size_t total_chars = 0;
        for (const auto& [role, content] : messages) {
          msgs.push_back({role.c_str(), content.c_str()});
          total_chars += role.size() + content.size();
        }
        // llama.h recommends ~2x the total message chars up front; if that
        // still isn't enough, the return value is the size needed.
        if (total_chars > static_cast<size_t>(INT32_MAX) / 2) {
          return std::nullopt;
        }
        std::vector<char> buf(std::max<size_t>(total_chars * 2, 1024));
        int32_t n = llama_chat_apply_template(
            tmpl, msgs.data(), msgs.size(), /*add_ass=*/true, buf.data(),
            static_cast<int32_t>(buf.size()));
        if (n < 0 && tmpl != nullptr) {
          tmpl = nullptr;  // chatml
          n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true,
                                        buf.data(),
                                        static_cast<int32_t>(buf.size()));
        }
        if (n > static_cast<int32_t>(buf.size())) {
          // Truncated (strncpy semantics): resize to the reported size.
          buf.resize(static_cast<size_t>(n));
          n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true,
                                        buf.data(),
                                        static_cast<int32_t>(buf.size()));
        }
        if (n < 0 || n > static_cast<int32_t>(buf.size())) {
          // Defensive last resort (chatml is built in): zephyr-style.
          std::string out;
          for (const auto& [role, content] : messages) {
            out += "<|" + role + "|>\n" + content + "</s>\n";
          }
          return out + "<|assistant|>\n";
        }
        // The buffer is not NUL-terminated when the result fills it.
        return std::string(buf.data(), static_cast<size_t>(n));
      };

  {
    // Template overhead per message, measured on two empty messages.
    const std::string empty_chat =
        apply_template({{"system", ""}, {"user", ""}}).value_or(std::string());
    in.per_message_overhead =
        static_cast<int>(TokenizeText(vocab, empty_chat, true, true).size()) / 2 +
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

    std::vector<std::pair<std::string, std::string>> messages;
    messages.emplace_back(
        "system", ctx_text.empty()
                      ? system_text
                      : system_text + chat.context_preamble + ctx_text);
    for (size_t i = plan.history_keep_from; i < history.size(); ++i) {
      messages.push_back(history[i]);
    }
    messages.emplace_back("user", user_text);

    // parse_special=true: the template's control markers (<|im_end|>,
    // <|eot_id|>, </s>, ...) must become real special tokens, not plain
    // text. Message contents were neutralized above.
    const std::optional<std::string> formatted = apply_template(messages);
    if (!formatted) break;  // kPromptTooLong
    std::optional<std::vector<llama_token>> t =
        TryTokenize(vocab, *formatted,
                    /*add_special=*/true, /*parse_special=*/true);
    if (!t) {
      summary.status = EngineStatus::kTokenizeFailed;
      return summary;
    }
    tokens = std::move(*t);
    const int over = static_cast<int>(tokens.size()) + plan.reply_tokens +
                     kContextSlack - n_ctx;
    if (over <= 0) {
      fitted = true;
      reply_tokens = plan.reply_tokens;
      summary.history_dropped = ToU32(plan.history_keep_from);
      summary.context_trimmed = plan.context_token_limit >= 0;
      summary.user_message_trimmed = plan.user_token_limit >= 0;
    } else {
      // Per-message estimates undershot the real template; tighten and
      // re-plan (never by cutting the system prompt).
      in.safety_margin += over + 16;
    }
  }
  if (!fitted || tokens.empty()) {
    summary.status = EngineStatus::kPromptTooLong;
    return summary;
  }
  summary.prompt_tokens = ToU32(tokens.size());

  // Some templates pre-open the reasoning block in the prompt.
  const std::string tail_text = Detokenize(
      vocab, std::vector<llama_token>(
                 tokens.end() - std::min<std::ptrdiff_t>(
                                    4, static_cast<std::ptrdiff_t>(
                                           tokens.size())),
                 tokens.end()));

  DecodeParams dp;
  // Visible and hidden budgets both come out of the reply reserve; when the
  // window is tight, shrink the hidden part first.
  dp.max_visible = std::min(visible, reply_tokens);
  dp.max_hidden = std::max(16, std::min(hidden, reply_tokens - dp.max_visible));
  dp.think_start_inside =
      tail_text.find(ThinkTagFilter::kOpenTag) != std::string::npos &&
      tail_text.find(ThinkTagFilter::kCloseTag) == std::string::npos;
  // Gemma 4 with thinking off pre-fills an empty, closed thought block
  // ("<|channel>thought\n<channel|>"); a template that leaves one open
  // starts the stream hidden.
  dp.thought_start_inside = GemmaThoughtFilter::EndsInsideThought(tail_text);
  dp.temperature = params.temperature;
  dp.top_p = params.top_p;
  dp.top_k = static_cast<int>(std::min<uint32_t>(params.top_k, INT32_MAX));
  dp.stop_sequences = params.stop_sequences;

  summary.status =
      Generate(tokens, params.conversation_key, dp, callbacks, &summary);
  return summary;
}

EngineStatus LlamaEngine::Generate(const std::vector<int32_t>& prompt,
                                   const std::string& reuse_key,
                                   const DecodeParams& p,
                                   const EngineCallbacks& callbacks,
                                   EngineSummary* summary) {
  Model& m = *model_;
  const llama_vocab* vocab = m.vocab;
  const int n_ctx = m.n_ctx;
  llama_memory_t mem = llama_get_memory(m.ctx);
  if (!vocab || !mem || prompt.empty()) return EngineStatus::kDecodeFailed;
  auto cancelled = [&]() {
    return callbacks.is_cancelled && callbacks.is_cancelled();
  };

  // ---- KV prefix reuse ----
  size_t n_keep = 0;
  if (!reuse_key.empty() && reuse_key == m.kv_key) {
    n_keep = CommonPrefixLength(m.kv_tokens, prompt);
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
  m.kv_tokens.assign(prompt.begin(),
                     prompt.begin() + static_cast<std::ptrdiff_t>(n_keep));
  m.kv_key = reuse_key;
  summary->reused_tokens = ToU32(n_keep);

  auto fail_decode = [&]() {
    // The cache state is unknown after a failed decode: start clean next
    // time.
    llama_memory_clear(mem, true);
    m.kv_tokens.clear();
    m.kv_key.clear();
    return EngineStatus::kDecodeFailed;
  };

  // ---- Prompt evaluation in n_batch-sized chunks ----
  const auto start = Clock::now();
  auto last_progress = start;
  const auto interval = std::chrono::milliseconds(kProgressIntervalMs);
  std::vector<llama_token> work(prompt.begin(), prompt.end());
  for (size_t i = n_keep; i < work.size();) {
    if (cancelled()) {
      return EngineStatus::kCancelled;  // kv_tokens matches what was decoded
    }
    const size_t n =
        std::min<size_t>(static_cast<size_t>(m.n_batch), work.size() - i);
    llama_batch batch =
        llama_batch_get_one(work.data() + i, static_cast<int32_t>(n));
    if (llama_decode(m.ctx, batch) != 0) return fail_decode();
    m.kv_tokens.insert(m.kv_tokens.end(),
                       work.begin() + static_cast<std::ptrdiff_t>(i),
                       work.begin() + static_cast<std::ptrdiff_t>(i + n));
    i += n;
    const auto now = Clock::now();
    if (callbacks.on_progress && i < work.size() &&
        now - last_progress >= interval) {
      last_progress = now;
      callbacks.on_progress(ToU32(i), ToU32(work.size()));
    }
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
  const bool gemma_thoughts = m.thought_channels;
  GemmaThoughtFilter gemma(gemma_thoughts && p.thought_start_inside);
  ThinkTagFilter think(p.think_start_inside);
  Utf8Assembler utf8;
  // Last, on the display text, so it sees exactly what the browser's copy
  // (StreamValidator) sees.
  StopSequenceFilter stop(p.stop_sequences);
  // Visible text waiting to be sent: one chunk per sampled token at most.
  std::string out;
  auto feed = [&](const std::string& piece) {
    std::string text = harmony.Feed(piece);
    if (gemma_thoughts) text = gemma.Feed(text);
    out += stop.Feed(ToDisplayText(utf8.Push(think.Feed(text))));
  };
  auto send = [&]() {
    if (out.empty()) return;
    EmitChunks(std::move(out), callbacks);
    out.clear();
  };
  auto hiding = [&]() {
    return harmony.hiding() || gemma.hiding() || think.in_think();
  };

  int visible_tokens = 0;
  int hidden_tokens = 0;
  bool forced_answer = false;
  EngineStatus status = EngineStatus::kEndOfGeneration;

  while (true) {
    if (cancelled()) {
      status = EngineStatus::kCancelled;
      break;
    }
    // Never let the cache reach n_ctx (llama_decode would fail).
    if (static_cast<int>(m.kv_tokens.size()) >= n_ctx - 1) {
      status = EngineStatus::kContextFull;
      break;
    }

    llama_token tok = llama_sampler_sample(sampler, m.ctx, -1);
    llama_sampler_accept(sampler, tok);

    // Stop on ANY end-of-generation token (EOS, <|eot_id|>, <|im_end|>,
    // <|end|>, <|return|>, ...) BEFORE converting to text, so EOG token
    // text is never emitted.
    if (llama_vocab_is_eog(vocab, tok)) {
      status = EngineStatus::kEndOfGeneration;
      break;
    }

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
    send();
    if (stop.stopped()) {
      // The stop text is in the KV cache only up to this token; the next
      // turn re-evaluates from the common prefix anyway.
      status = EngineStatus::kStopSequence;
      break;
    }

    if (llama_decode(m.ctx, llama_batch_get_one(&tok, 1)) != 0) {
      status = fail_decode();
      break;
    }
    m.kv_tokens.push_back(tok);
    ++summary->generated_tokens;

    if (visible_tokens >= p.max_visible) {
      status = EngineStatus::kMaxTokens;
      break;
    }

    if (hiding() && hidden_tokens >= p.max_hidden) {
      status = EngineStatus::kMaxTokens;
      if (forced_answer) break;  // it went back to reasoning; give up
      // Reasoning budget spent: close the reasoning block / switch to the
      // final channel so the model answers instead of thinking on.
      forced_answer = true;
      const char* force =
          gemma.hiding()
              ? GemmaThoughtFilter::kForceClose
              : (think.in_think() ? ThinkTagFilter::kForceClose
                                  : HarmonyChannelFilter::kForceFinal);
      std::vector<llama_token> ft = TokenizeText(vocab, force, false, true);
      if (ft.empty() ||
          static_cast<int>(m.kv_tokens.size() + ft.size()) >= n_ctx - 1) {
        break;
      }
      llama_batch force_batch =
          llama_batch_get_one(ft.data(), static_cast<int32_t>(ft.size()));
      if (llama_decode(m.ctx, force_batch) != 0) {
        status = fail_decode();
        break;
      }
      m.kv_tokens.insert(m.kv_tokens.end(), ft.begin(), ft.end());
      for (llama_token t : ft) feed(TokenToPiece(vocab, t));
      // The marker closed one Gemma block; nested ones would keep the
      // answer hidden, so close them all (no-op when none is open).
      gemma.ForceClose();
      if (think.in_think()) {
        // Control-token form of the tags rendered empty; tell the filter
        // explicitly.
        feed(ThinkTagFilter::kCloseTag);
      }
      send();
      hidden_tokens = 0;
      if (stop.stopped()) {
        status = EngineStatus::kStopSequence;
        break;
      }
    }
  }

  llama_sampler_free(sampler);

  // Release held-back bytes in pipeline order (two statements: the Gemma
  // flush must reach the think filter before it flushes).
  std::string rest =
      gemma_thoughts ? think.Feed(gemma.Flush()) : std::string();
  rest += think.Flush();
  out += stop.Feed(ToDisplayText(utf8.Push(rest) + utf8.Flush()));
  out += stop.Flush();
  send();
  if (stop.stopped() && status != EngineStatus::kCancelled &&
      status != EngineStatus::kDecodeFailed) {
    status = EngineStatus::kStopSequence;
  }
  return status;
}

}  // namespace molt_ai
