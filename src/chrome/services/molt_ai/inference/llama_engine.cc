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
//  - Embeddings (llama_embedding_engine.cc, which details each): decode
//    with embeddings on (and through llama_context::encode() for models
//    without a KV cache), llama_get_embeddings_seq, llama_pooling_type,
//    llama_n_ubatch, llama_model_n_embd_out, llama_vocab_get_add_eos,
//    llama_vocab_get_add_sep: no throw on the inputs used there.
//  - Constrained decoding (grammar_sampler.cc, which details each):
//    llama_sampler_init_grammar (the GBNF parser catches its own throws and
//    returns nullptr), llama_sampler_apply on the grammar (asserts that can
//    only fail after a bad accept), llama_sampler_accept on the grammar
//    (throws on a token the grammar rejects, aborts on an early
//    end-of-generation token: only tokens that passed the same grammar
//    check, strict UTF-8 and the token bans are accepted, never an
//    end-of-generation token), llama_sampler_apply on the chain, and
//    llama_get_logits_ith (catches; nullptr on a bad index).
//  - Image input (tools/mtmd, vendored at the same tag; section 10 of
//    docs/design/SANDBOXED_INFERENCE.md):
//    - mtmd_init_from_file_ptr (local patch): catches std::exception from
//      the context and clip_init_from_file_ptr (whose loader throws and
//      catches its own) and returns nullptr. A short read of the projector
//      is an exception there, not uninitialized weights.
//    - mtmd_tokenize_from_parts: catches std::exception and returns 2; its
//      asserts need a mergeable or audio bitmap (never made here), a
//      projector without a preprocessor (init throws first) or a tokenizer
//      whose second call disagrees with its first (the first call's buffer
//      is always large enough).
//    - mtmd_encode_chunk: catches std::exception and returns 1.
//    - mtmd_get_output_embd: the last encode's buffer, n_embd_inp floats per
//      image token (the context checked the widths match).
//    - mtmd_bitmap_init: copies width * height * 3 bytes; only called after
//      CheckModelImage() matched the buffer to that size.
//    - mtmd_image_tokens_get_decoder_pos: aborts on an index past the
//      image's tokens; only called with indexes below
//      mtmd_input_chunk_get_n_tokens().
//    - mtmd_input_chunk_get_n_tokens / _get_n_pos: abort on an unknown chunk
//      type; mtmd makes only text, image and audio chunks.
//    - mtmd_context_params_default, mtmd_free, mtmd_support_vision,
//      mtmd_decode_use_mrope, mtmd_decode_use_non_causal,
//      mtmd_input_chunks_init / _size / _get (nullptr past the end) /
//      _free, mtmd_input_chunk_get_type / _get_tokens_text /
//      _get_tokens_image, mtmd_bitmap_free: no throw.
//    - llama_batch_ext_init / _free / _clear / _add_token / _add_embd /
//      _set_pos / _set_output_logits: return error codes (a full batch, a
//      bad token id or embedding width), no throw.
//    - llama_process(DECODE): llama_decode()'s path. Its asserts: at most
//      n_batch tokens (a batch never holds more), and with non-causal
//      attention the whole batch in one ubatch (checked before the call).
//    - llama_set_causal_attn, llama_n_ubatch, llama_model_n_embd_inp:
//      setters and getters.
//  - The GPU (llama.cpp's Vulkan backend, GN switch molt_llama_vulkan;
//    design section 11.6): ggml-vulkan throws vk::DeviceLostError and
//    vk::SystemError from graph compute, synchronization and buffer clears.
//    Model loading and context creation catch them (std::exception, above);
//    llama_decode, llama_process, llama_get_logits_ith,
//    llama_sampler_sample and llama_memory_clear don't, so this file calls
//    those through third_party/llama_cpp/chromium/llama_guard.h, which
//    catches everything (on the CPU too, where nothing throws). After a
//    caught exception the context is never used or freed again
//    (EngineStatus::kBackendFailed; ~Model leaks it). mtmd_encode_chunk
//    catches on its own. The Vulkan backend is brought up and its devices
//    listed only through the guard (ListVulkanDevices).
// Re-check these when updating the vendored llama.cpp.

#include "chrome/services/molt_ai/inference/llama_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <utility>

#include "chrome/services/molt_ai/inference/engine/context_budget.h"
#include "chrome/services/molt_ai/inference/engine/gpu_offload.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"
#include "chrome/services/molt_ai/inference/engine/prompt_utils.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"
#include "chrome/services/molt_ai/inference/grammar_sampler.h"
#include "third_party/llama_cpp/chromium/llama_guard.h"
#include "third_party/llama_cpp/include/llama.h"
#include "third_party/llama_cpp/tools/mtmd/mtmd.h"

namespace molt_ai {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

// Prompt tokens kept free beyond the plan, for template/tokenizer drift.
constexpr int kContextSlack = 4;

// Constrained answers: prompt room for the prefill that skips reasoning,
// and the prefills themselves (special tokens of those vocabularies).
constexpr int kPrefillReserveTokens = 16;
// gpt-oss: the generation prompt ends "<|start|>assistant"; the answer goes
// in the final channel.
constexpr char kHarmonyFinalPrefill[] = "<|channel|>final<|message|>";
// Qwen3 without thinking, as its own chat template writes it.
constexpr char kQwen3NoThinkPrefill[] = "<think>\n\n</think>\n\n";

// Memory budget under a sandbox limit (design section 2.5): weights that may
// be repacked into anonymous memory (at most the whole file), a minimal KV
// cache and compute buffers must fit in this share of the limit.
constexpr uint64_t kBudgetPercentOfLimit = 85;
constexpr uint64_t kMinKvReserveBytes = 1 * kGiB;
constexpr uint64_t kComputeReserveBytes = 1 * kGiB;
// The vision encoder's compute buffers, on top of the projector's weights
// (which clip copies into memory: it doesn't map the file).
constexpr uint64_t kVisionComputeReserveBytes = 512ULL * 1024 * 1024;

// Image input. An image takes at most this many tokens on a model whose
// vision encoder has a dynamic resolution (llama.cpp's image_max_tokens; a
// 1024 x 1024 image is 1024 tokens on Qwen-VL's 32-pixel token grid).
constexpr int kMaxImageTokens = 1024;
// Where an image goes in the chat template's output. Never produced by
// untrusted text: it is neutralized like a special token in every untrusted
// field of a request with images, and the markers must pair up with the
// images one for one.
constexpr char kMediaMarker[] = "<__media__>";

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

std::string MediaMarkers(size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) out += kMediaMarker;
  return out;
}

size_t CountChunkTokens(const mtmd_input_chunks* chunks) {
  size_t n = 0;
  for (size_t i = 0; i < mtmd_input_chunks_size(chunks); ++i) {
    n += mtmd_input_chunk_get_n_tokens(mtmd_input_chunks_get(chunks, i));
  }
  return n;
}

// Tokenizes the chat template's output |formatted|, in which each image is
// kMediaMarker, with |bitmaps| (in order) in place of the markers. Text is
// tokenized with parse_special=true, as without images (untrusted fields
// were neutralized, the marker included), and BOS / EOS are added as the
// vocab wants. nullptr if the markers and the images don't pair up one for
// one, or llama.cpp refuses (an image its preprocessing can't take).
mtmd::input_chunks_ptr TokenizeWithImages(
    mtmd_context* ctx,
    const std::string& formatted,
    const std::vector<const mtmd_bitmap*>& bitmaps) {
  std::vector<std::string> segments;
  const size_t marker_len = sizeof(kMediaMarker) - 1;
  size_t start = 0;
  while (true) {
    const size_t at = formatted.find(kMediaMarker, start);
    if (at == std::string::npos) break;
    segments.push_back(formatted.substr(start, at - start));
    start = at + marker_len;
  }
  segments.push_back(formatted.substr(start));
  if (segments.size() != bitmaps.size() + 1) return nullptr;

  std::vector<mtmd_input_text> texts;
  texts.reserve(segments.size());  // |parts| points into it
  std::vector<mtmd_input_part> parts;
  parts.reserve(segments.size() + bitmaps.size());
  for (size_t i = 0; i < segments.size(); ++i) {
    if (!segments[i].empty()) {
      texts.push_back({segments[i].data(), segments[i].size(),
                       /*add_special=*/false, /*parse_special=*/true});
      parts.push_back({&texts.back(), nullptr});
    }
    if (i < bitmaps.size()) parts.push_back({nullptr, bitmaps[i]});
  }
  std::vector<const mtmd_input_part*> part_ptrs;
  part_ptrs.reserve(parts.size());
  for (const mtmd_input_part& part : parts) part_ptrs.push_back(&part);

  mtmd::input_chunks_ptr chunks(mtmd_input_chunks_init());
  if (!chunks ||
      mtmd_tokenize_from_parts(ctx, chunks.get(), part_ptrs.data(),
                               part_ptrs.size(), /*add_special=*/true) != 0) {
    return nullptr;
  }
  return chunks;
}

// The prompt tokens (KV cells) an image takes, its begin / end markers
// included, from a placeholder of its size (no pixels: nothing is
// preprocessed). 0 if llama.cpp refuses it.
size_t ImageTokens(mtmd_context* ctx, const mtmd_bitmap* placeholder) {
  const mtmd_input_part part = {nullptr, placeholder};
  const mtmd_input_part* parts[] = {&part};
  mtmd::input_chunks_ptr chunks(mtmd_input_chunks_init());
  if (!chunks || mtmd_tokenize_from_parts(ctx, chunks.get(), parts, 1,
                                          /*add_special=*/false) != 0) {
    return 0;
  }
  return CountChunkTokens(chunks.get());
}

// A request's images as llama.cpp bitmaps, in prompt order.
struct RequestImages {
  // Size only, for counting tokens while the prompt is fitted.
  std::vector<mtmd::bitmap_ptr> placeholders;
  // The pixels, for the prompt that is evaluated.
  std::vector<mtmd::bitmap_ptr> bitmaps;
  // Prompt tokens of each.
  std::vector<size_t> tokens;

  // False if |image| isn't a valid model image or llama.cpp can't take it.
  bool Add(mtmd_context* ctx, const EngineImage& image) {
    if (CheckModelImage(image.width, image.height, image.rgb.size()) !=
        ImageCheck::kOk) {
      return false;
    }
    mtmd::bitmap_ptr placeholder(
        mtmd_bitmap_init(image.width, image.height, nullptr));
    mtmd::bitmap_ptr bitmap(
        mtmd_bitmap_init(image.width, image.height, image.rgb.data()));
    if (!placeholder || !bitmap) return false;
    const size_t n = ImageTokens(ctx, placeholder.get());
    if (n == 0) return false;
    placeholders.push_back(std::move(placeholder));
    bitmaps.push_back(std::move(bitmap));
    tokens.push_back(n);
    return true;
  }

  // |which| (indexes) of |from|, as llama.cpp wants them.
  static std::vector<const mtmd_bitmap*> Pick(
      const std::vector<mtmd::bitmap_ptr>& from,
      const std::vector<size_t>& which) {
    std::vector<const mtmd_bitmap*> out;
    out.reserve(which.size());
    for (size_t i : which) out.push_back(from[i].get());
    return out;
  }
};

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

EngineImage::EngineImage() = default;
EngineImage::EngineImage(uint32_t w, uint32_t h, std::vector<uint8_t> pixels)
    : width(w), height(h), rgb(std::move(pixels)) {}
EngineImage::~EngineImage() = default;
EngineImage::EngineImage(EngineImage&&) = default;
EngineImage& EngineImage::operator=(EngineImage&&) = default;
EngineImage::EngineImage(const EngineImage&) = default;
EngineImage& EngineImage::operator=(const EngineImage&) = default;

EngineChatTurn::EngineChatTurn() = default;
EngineChatTurn::EngineChatTurn(bool user, std::string text)
    : from_user(user), content(std::move(text)) {}
EngineChatTurn::~EngineChatTurn() = default;
EngineChatTurn::EngineChatTurn(EngineChatTurn&&) = default;
EngineChatTurn& EngineChatTurn::operator=(EngineChatTurn&&) = default;
EngineChatTurn::EngineChatTurn(const EngineChatTurn&) = default;
EngineChatTurn& EngineChatTurn::operator=(const EngineChatTurn&) = default;

EngineChatRequest::EngineChatRequest() = default;
EngineChatRequest::~EngineChatRequest() = default;
EngineChatRequest::EngineChatRequest(EngineChatRequest&&) = default;
EngineChatRequest& EngineChatRequest::operator=(EngineChatRequest&&) = default;
EngineChatRequest::EngineChatRequest(const EngineChatRequest&) = default;
EngineChatRequest& EngineChatRequest::operator=(const EngineChatRequest&) =
    default;

EngineCallbacks::EngineCallbacks() = default;
EngineCallbacks::~EngineCallbacks() = default;
EngineCallbacks::EngineCallbacks(EngineCallbacks&&) = default;
EngineCallbacks& EngineCallbacks::operator=(EngineCallbacks&&) = default;

struct LlamaEngine::Model {
  ~Model() {
    // After a GPU failure nothing is freed: freeing on a lost device can
    // throw from a destructor, which terminates even with exceptions on.
    // The browser ends the process anyway.
    if (!backend_failed) FreeLlama();
    // Only now: lazily read tensors go through this stream.
    file.reset();
  }

  void FreeLlama() {
    // Before the model: it holds the model's vocab.
    if (mtmd) mtmd_free(mtmd);
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    mtmd = nullptr;
    ctx = nullptr;
    model = nullptr;
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
  // Built on the first constrained generation (GrammarVocab::Build reads
  // every token once). Doesn't use the vocab after Build().
  std::unique_ptr<GrammarVocab> grammar_vocab;
  // The vision projector's context (image input); null without one.
  mtmd_context* mtmd = nullptr;
  // A guarded llama.cpp call threw (the GPU failed): the context's state is
  // unknown. Every later request fails with kBackendFailed.
  bool backend_failed = false;
};

// A prompt with images: llama.cpp's chunks (text and images, in order), then
// |suffix| (a constrained answer's prefill), |n_tokens| KV cells in all.
struct LlamaEngine::MediaPrompt {
  const mtmd_input_chunks* chunks = nullptr;
  std::vector<llama_token> suffix;
  size_t n_tokens = 0;
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
  // GBNF; "" = unconstrained.
  std::string grammar;
  // A prompt with images, evaluated instead of Generate()'s token prompt.
  const MediaPrompt* media = nullptr;
};

// static
void LlamaEngine::InitBackendOnce() {
  static std::once_flag once;
  std::call_once(once, []() { llama_backend_init(); });
}

namespace {
std::atomic<bool> g_allow_software_gpu_for_testing{false};
}  // namespace

// static
void LlamaEngine::SetAllowSoftwareGpuForTesting(bool allow) {
  g_allow_software_gpu_for_testing.store(allow);
}

LlamaEngine::LlamaEngine() = default;
LlamaEngine::~LlamaEngine() = default;

bool LlamaEngine::loaded() const {
  return model_ && model_->ctx;
}

bool LlamaEngine::vision() const {
  return loaded() && model_->mtmd;
}

EngineLoadResult LlamaEngine::Load(ScopedStdioFile scoped_file,
                                   const EngineLoadParams& params,
                                   ScopedStdioFile vision_projector) {
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

  // The vision projector (image input): the size the browser saw. It is read
  // in each load attempt below (clip copies the weights out of the file).
  FILE* const proj = vision_projector.get();
  if (proj) {
    int64_t proj_size = -1;
    if (SeekFile(proj, 0, SEEK_END)) proj_size = TellFile(proj);
    if (proj_size < 0 ||
        static_cast<uint64_t>(proj_size) != params.vision_projector_size) {
      result.status = EngineLoadStatus::kInvalidVisionProjector;
      return result;
    }
  }

  // Memory budget under the sandbox limit (design section 2.5). CPU weight
  // repacking copies the weights into anonymous memory, which the limit
  // counts (the read-only file mapping doesn't).
  const uint64_t limit = params.memory_limit_bytes;
  const uint64_t budget = limit / 100 * kBudgetPercentOfLimit;
  // The vision projector is copied into memory, wherever the model's
  // weights live.
  const uint64_t vision_bytes =
      proj ? params.vision_projector_size + kVisionComputeReserveBytes : 0;
  bool repack = params.allow_cpu_repack;
  if (limit > 0 && params.file_size + kMinKvReserveBytes +
                           kComputeReserveBytes + vision_bytes >
                       budget) {
    repack = false;
  }

  // ---- The GPU through llama.cpp's Vulkan backend (design section 11.5) ----
  // Only in builds with it (Linux and Windows, molt_llama_vulkan) and only
  // when the browser asks; the CPU is the fallback. macOS uses Metal as
  // before, whatever |use_gpu| says.
  const bool vulkan = molt_llama::VulkanBuilt();
  EngineGpuStatus gpu_status = EngineGpuStatus::kNotRequested;
  ggml_backend_dev_t gpu_device = nullptr;
  GpuOffloadInput gpu_input;
  GpuOffloadPlan gpu_plan;
  if (params.use_gpu && !vulkan) {
    gpu_status = EngineGpuStatus::kNotBuilt;
  } else if (params.use_gpu) {
    std::vector<molt_llama::GpuDevice> devices;
    if (!molt_llama::ListVulkanDevices(&devices)) {
      gpu_status = EngineGpuStatus::kNoDevice;
    } else {
      std::vector<GpuDeviceInfo> infos;
      infos.reserve(devices.size());
      for (const molt_llama::GpuDevice& d : devices) {
        GpuDeviceInfo info;
        info.vendor_id = d.vendor_id;
        info.device_id = d.device_id;
        info.device_type = d.device_type;
        info.driver_id = d.driver_id;
        info.driver_version = d.driver_version;
        info.api_version = d.api_version;
        info.memory_total = d.memory_total;
        info.memory_free = d.memory_free;
        if (g_allow_software_gpu_for_testing.load() &&
            CheckGpuDevice(info, {}) == GpuDeviceVerdict::kSoftware) {
          // Tests only (SetAllowSoftwareGpuForTesting): judge it as an
          // integrated GPU.
          info.device_type = static_cast<uint32_t>(GpuDeviceType::kIntegrated);
          info.driver_id = 0;
        }
        infos.push_back(info);
      }
      const std::optional<size_t> chosen =
          ChooseGpuDevice(infos, DefaultGpuDriverBlocklist());
      if (!chosen) {
        gpu_status = EngineGpuStatus::kNoUsableDevice;
      } else {
        gpu_input.model_file_bytes = params.file_size;
        // From the GGUF header (llama.cpp parses it again right after); it
        // leaves |file| at its start.
        gpu_input.layer_count = molt_llama::ReadLayerCount(file);
        gpu_input.memory_total = infos[*chosen].memory_total;
        gpu_input.memory_free = infos[*chosen].memory_free;
        gpu_plan = PlanGpuOffload(gpu_input);
        if (gpu_plan.gpu_layers == 0) {
          gpu_status = EngineGpuStatus::kTooLittleMemory;
        } else {
          gpu_device = devices[*chosen].device;
          gpu_status = EngineGpuStatus::kOn;
        }
      }
    }
    if (!SeekFile(file, 0, SEEK_SET)) return result;
  }

  ContextSizingInput sizing_in;
  bool on_gpu = false;
  bool all_on_gpu = false;
  // One attempt at the model, its projector and its context on |device|
  // (nullptr: the CPU alone in a Vulkan build, Metal or the CPU otherwise).
  // On failure the caller frees what was loaded.
  auto attempt = [&](ggml_backend_dev_t device) -> EngineLoadStatus {
    llama_model_params model_params = llama_model_default_params();
    ggml_backend_dev_t devices[2] = {device, nullptr};
    if (vulkan) {
      // Never "every registered device": the chosen GPU, or none.
      model_params.devices = devices;
      model_params.n_gpu_layers = device ? gpu_plan.gpu_layers : 0;
    } else {
      model_params.n_gpu_layers = -1;  // all layers on the GPU (Metal)
    }
    model_params.use_extra_bufts = repack;
    m->model = llama_model_load_from_file_ptr(file, model_params);
    if (!m->model) return EngineLoadStatus::kInvalidModelFile;

    on_gpu = vulkan ? device != nullptr : llama_supports_gpu_offload();
    all_on_gpu = on_gpu && (!vulkan || gpu_plan.gpu_layers < 0);
    const int threads =
        static_cast<int>(std::clamp<uint32_t>(params.cpu_threads, 1, 256));

    // ---- Vision projector (image input) ----
    // Before the context, so the KV cache is sized with the projector
    // already in memory. clip copies the weights out of the file.
    if (proj) {
      if (!SeekFile(proj, 0, SEEK_SET)) {
        return EngineLoadStatus::kInvalidVisionProjector;
      }
      mtmd_context_params mparams = mtmd_context_params_default();
      mparams.use_gpu = on_gpu;
      // The model's GPU, not the first one ggml would pick.
      mparams.device = vulkan ? device : nullptr;
      mparams.print_timings = false;
      mparams.n_threads = threads;
      // Size the encoder's buffers for the largest image now (warmup), so an
      // image never fails for memory later.
      mparams.warmup = true;
      mparams.image_max_tokens = kMaxImageTokens;
      m->mtmd = mtmd_init_from_file_ptr(proj, m->model, mparams);
      if (!m->mtmd || !mtmd_support_vision(m->mtmd)) {
        return EngineLoadStatus::kInvalidVisionProjector;
      }
    }

    // Size the context from the model's training window, RAM and a KV-cache
    // estimate, then cap it by the browser's limit, the sandbox budget and
    // the GPU's memory.
    sizing_in = ContextSizingInput();
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
      // the context is tried anyway; if it can't be allocated the load
      // fails with kOutOfMemory.
      const uint64_t used = (repack && !all_on_gpu ? params.file_size : 0) +
                            kComputeReserveBytes + vision_bytes;
      const uint64_t kv_room = budget > used ? budget - used : 0;
      const uint64_t by_limit = kv_room / sizing_in.kv_bytes_per_token;
      if (by_limit < static_cast<uint64_t>(n_ctx)) {
        n_ctx = std::max(floor, static_cast<int>(by_limit - by_limit % 256));
      }
    }
    if (device) {
      // The offloaded layers' KV cache lives on the GPU, next to their
      // weights.
      if (const std::optional<uint32_t> by_gpu = GpuContextLimit(
              gpu_input, gpu_plan, sizing_in.kv_bytes_per_token)) {
        const int cap = static_cast<int>(
            std::min<uint32_t>(*by_gpu, static_cast<uint32_t>(INT32_MAX)));
        n_ctx = std::min(n_ctx, std::max(floor, cap - cap % 256));
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
    ctx_params.n_batch =
        static_cast<uint32_t>(std::min(n_ctx, sizing.n_batch));
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
    if (!m->ctx) return EngineLoadStatus::kOutOfMemory;
    return EngineLoadStatus::kOk;
  };

  EngineLoadStatus status = attempt(gpu_device);
  if (status != EngineLoadStatus::kOk && gpu_device) {
    // The GPU couldn't take the model, its projector or its context (out of
    // device memory, a driver error llama.cpp caught): the CPU alone. Those
    // failures were reported, not thrown at us, so the device is fine to
    // free on.
    m->FreeLlama();
    gpu_device = nullptr;
    gpu_status = EngineGpuStatus::kLoadFailed;
    if (!SeekFile(file, 0, SEEK_SET)) return result;
    status = attempt(nullptr);
  }
  if (status != EngineLoadStatus::kOk) {
    result.status = status;
    return result;  // ~Model frees whatever was loaded
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
  result.description.backend = on_gpu ? "metal" : "cpu";
#else
  result.description.backend = on_gpu ? "vulkan" : "cpu";
#endif
  result.description.context_tokens = ToU32(m->n_ctx);
  result.description.train_context_tokens = ToU32(sizing_in.n_ctx_train);
  result.description.vocab_size = ToU32(llama_vocab_n_tokens(m->vocab));
  result.description.cpu_repack_used = repack && !all_on_gpu;
  result.description.vision = m->mtmd != nullptr;
  result.description.gpu_status = gpu_status;
  if (gpu_device) {
    // The repeating layers plus the output layer when all of them are on it.
    const int32_t n_layer = std::max(0, llama_model_n_layer(m->model));
    result.description.gpu_layers =
        gpu_plan.gpu_layers < 0
            ? ToU32(n_layer + 1)
            : ToU32(std::min(gpu_plan.gpu_layers, n_layer + 1));
  }
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
  if (model_->backend_failed) return;
  llama_memory_t mem = llama_get_memory(model_->ctx);
  if (mem && !molt_llama::MemoryClear(mem, true)) {
    model_->backend_failed = true;
  }
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
  if (model_->backend_failed) {
    summary.status = EngineStatus::kBackendFailed;
    return summary;
  }
  Model& m = *model_;
  const int n_ctx = m.n_ctx;
  const int visible = params.max_visible_tokens > 0
                          ? static_cast<int>(std::min<uint32_t>(
                                params.max_visible_tokens, INT32_MAX / 4))
                          : AdaptiveReplyTokens(n_ctx);
  // A constrained answer has no hidden reasoning (the grammar applies from
  // the first token).
  int hidden = 0;
  if (params.grammar.empty()) {
    hidden = params.max_reasoning_tokens > 0
                 ? static_cast<int>(std::min<uint32_t>(
                       params.max_reasoning_tokens, INT32_MAX / 4))
                 : (IsReasoningArch(m.arch) ? AdaptiveReasoningTokens(n_ctx)
                                            : 64);
  }
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
  dp.grammar = params.grammar;
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
  if (model_->backend_failed) {
    summary.status = EngineStatus::kBackendFailed;
    return summary;
  }
  Model& m = *model_;
  const llama_vocab* vocab = m.vocab;
  const int n_ctx = m.n_ctx;
  const std::string& arch = m.arch;
  // A constrained answer starts at the first token: no reasoning.
  const bool constrained = !params.grammar.empty();
  const bool reasoning_on = params.reasoning_on && !constrained;

  // ---- Reasoning control ----
  std::string system_text;
  if (arch == "gpt-oss") {
    // Harmony reads the reasoning effort from the system message.
    system_text =
        reasoning_on ? "Reasoning: medium\n\n" : "Reasoning: low\n\n";
  }
  system_text += chat.system_prompt;
  if (IsQwen3Arch(arch) && !reasoning_on) {
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
  if (constrained) {
    hidden = 0;
  } else if (params.max_reasoning_tokens > 0) {
    hidden = static_cast<int>(
        std::min<uint32_t>(params.max_reasoning_tokens, INT32_MAX / 4));
  } else if (reasoning_on || arch == "gpt-oss") {
    hidden = AdaptiveReasoningTokens(n_ctx);
  }

  // ---- Images (user turns only; untrusted, like page text) ----
  size_t n_images = chat.user_images.size();
  for (const EngineChatTurn& turn : chat.history) {
    n_images += turn.images.size();
    if (!turn.images.empty() && !turn.from_user) {
      summary.status = EngineStatus::kTokenizeFailed;  // the service refuses
      return summary;
    }
  }
  const bool with_images = n_images > 0;
  if (with_images && !m.mtmd) {
    summary.status = EngineStatus::kImagesUnsupported;
    return summary;
  }
  if (n_images > kMaxImagesPerRequest) {
    summary.status = EngineStatus::kTokenizeFailed;  // the service refuses
    return summary;
  }
  // In prompt order: the history's (oldest first), then the new message's.
  RequestImages images;

  // ---- Untrusted text can't smuggle in control tokens ----
  // With images, nor an image marker: the markers must pair up with the
  // images one for one (TokenizeWithImages).
  std::vector<std::string> specials_with_marker;
  const std::vector<std::string>* specials_ptr = &m.special_texts;
  if (with_images) {
    specials_with_marker = m.special_texts;
    specials_with_marker.push_back(kMediaMarker);
    specials_ptr = &specials_with_marker;
  }
  const std::vector<std::string>& specials = *specials_ptr;
  const std::string context = NeutralizeSpecialTokens(chat.context, specials);
  const std::string user = NeutralizeSpecialTokens(chat.user_message, specials);
  std::vector<std::pair<std::string, std::string>> history;  // role, text
  // Per history message: its images (indexes into |images|).
  std::vector<std::vector<size_t>> history_images;
  history.reserve(chat.history.size());
  history_images.reserve(chat.history.size());
  for (const EngineChatTurn& turn : chat.history) {
    if (turn.content.empty() && turn.images.empty()) continue;
    std::vector<size_t> turn_images;
    for (const EngineImage& image : turn.images) {
      if (!images.Add(m.mtmd, image)) {
        summary.status = EngineStatus::kTokenizeFailed;
        return summary;
      }
      turn_images.push_back(images.bitmaps.size() - 1);
    }
    history.emplace_back(turn.from_user ? "user" : "assistant",
                         NeutralizeSpecialTokens(turn.content, specials));
    history_images.push_back(std::move(turn_images));
  }
  std::vector<size_t> user_images;
  size_t user_image_tokens = 0;
  for (const EngineImage& image : chat.user_images) {
    if (!images.Add(m.mtmd, image)) {
      summary.status = EngineStatus::kTokenizeFailed;
      return summary;
    }
    user_images.push_back(images.bitmaps.size() - 1);
    user_image_tokens += images.tokens.back();
  }

  // ---- Token budget: the system prompt is never trimmed ----
  auto count = [&](const std::string& s) {
    return static_cast<int>(TokenizeText(vocab, s, false, false).size());
  };
  PromptBudgetInput in;
  in.n_ctx = n_ctx;
  in.reply_reserve = visible + hidden;
  // The new message's images are never trimmed (only its text is); a
  // history message's go with it when it is dropped.
  in.system_tokens =
      count(system_text) + (context.empty() ? 0 : count(chat.context_preamble)) +
      static_cast<int>(std::min<size_t>(user_image_tokens, INT32_MAX / 4));
  in.context_tokens = context.empty() ? 0 : count(context);
  in.user_tokens = count(user);
  for (size_t i = 0; i < history.size(); ++i) {
    size_t image_tokens = 0;
    for (size_t image : history_images[i]) image_tokens += images.tokens[image];
    in.history_tokens.push_back(
        count(history[i].second) +
        static_cast<int>(std::min<size_t>(image_tokens, INT32_MAX / 4)));
    in.history_starts_turn.push_back(history[i].first == "user");
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
  if (constrained) {
    // Room for the prefill below.
    in.safety_margin += kPrefillReserveTokens;
  }

  std::vector<llama_token> tokens;
  // With images: the formatted prompt and its images, in order.
  std::string media_formatted;
  std::vector<size_t> media_order;
  size_t media_tokens = 0;
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

    // A message's images go before its text.
    std::vector<std::pair<std::string, std::string>> messages;
    std::vector<size_t> order;
    messages.emplace_back(
        "system", ctx_text.empty()
                      ? system_text
                      : system_text + chat.context_preamble + ctx_text);
    for (size_t i = plan.history_keep_from; i < history.size(); ++i) {
      messages.emplace_back(
          history[i].first,
          MediaMarkers(history_images[i].size()) + history[i].second);
      order.insert(order.end(), history_images[i].begin(),
                   history_images[i].end());
    }
    messages.emplace_back("user",
                          MediaMarkers(user_images.size()) + user_text);
    order.insert(order.end(), user_images.begin(), user_images.end());

    // parse_special=true: the template's control markers (<|im_end|>,
    // <|eot_id|>, </s>, ...) must become real special tokens, not plain
    // text. Message contents were neutralized above.
    std::optional<std::string> formatted = apply_template(messages);
    if (!formatted) break;  // kPromptTooLong
    size_t n_prompt = 0;
    if (with_images) {
      // Counted with placeholders: the images are preprocessed once, below.
      mtmd::input_chunks_ptr counted = TokenizeWithImages(
          m.mtmd, *formatted, RequestImages::Pick(images.placeholders, order));
      if (!counted) {
        summary.status = EngineStatus::kTokenizeFailed;
        return summary;
      }
      n_prompt = CountChunkTokens(counted.get());
      media_formatted = std::move(*formatted);
      media_order = std::move(order);
      media_tokens = n_prompt;
    } else {
      std::optional<std::vector<llama_token>> t =
          TryTokenize(vocab, *formatted,
                      /*add_special=*/true, /*parse_special=*/true);
      if (!t) {
        summary.status = EngineStatus::kTokenizeFailed;
        return summary;
      }
      tokens = std::move(*t);
      n_prompt = tokens.size();
    }
    const int over = static_cast<int>(std::min<size_t>(n_prompt, INT32_MAX / 2)) +
                     plan.reply_tokens + kContextSlack - n_ctx;
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
  if (!fitted || (tokens.empty() && media_tokens == 0)) {
    summary.status = EngineStatus::kPromptTooLong;
    return summary;
  }

  // With images: the prompt that is evaluated, with the pixels. The image
  // preprocessing (llama.cpp's resize and normalize) runs here, once.
  mtmd::input_chunks_ptr media_chunks;
  MediaPrompt media;
  if (with_images) {
    media_chunks = TokenizeWithImages(
        m.mtmd, media_formatted,
        RequestImages::Pick(images.bitmaps, media_order));
    if (!media_chunks ||
        CountChunkTokens(media_chunks.get()) != media_tokens) {
      summary.status = EngineStatus::kTokenizeFailed;
      return summary;
    }
    // The answer's first token needs the logits of a text token: the
    // template always ends with one (the assistant's header).
    const size_t n_chunks = mtmd_input_chunks_size(media_chunks.get());
    const mtmd_input_chunk* last =
        n_chunks ? mtmd_input_chunks_get(media_chunks.get(), n_chunks - 1)
                 : nullptr;
    size_t n_last = 0;
    const llama_token* last_tokens =
        last && mtmd_input_chunk_get_type(last) == MTMD_INPUT_CHUNK_TYPE_TEXT
            ? mtmd_input_chunk_get_tokens_text(last, &n_last)
            : nullptr;
    if (!last_tokens || n_last == 0) {
      summary.status = EngineStatus::kTokenizeFailed;
      return summary;
    }
    // Only the tail matters below (reasoning-block detection).
    tokens.assign(last_tokens + (n_last - std::min<size_t>(n_last, 4)),
                  last_tokens + n_last);
    media.chunks = media_chunks.get();
    media.n_tokens = media_tokens;
  }
  summary.prompt_tokens = ToU32(with_images ? media_tokens : tokens.size());

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

  if (constrained) {
    // The grammar applies from the first generated token, and the
    // reasoning filters don't run: start the answer past any reasoning
    // block the template opened or the model would open. Our own text,
    // tokenized with its special tokens.
    const char* prefill = nullptr;
    if (arch == "gpt-oss") {
      prefill = kHarmonyFinalPrefill;
    } else if (m.thought_channels && dp.thought_start_inside) {
      prefill = GemmaThoughtFilter::kForceClose;
    } else if (dp.think_start_inside) {
      prefill = ThinkTagFilter::kForceClose;
    } else if (IsQwen3Arch(arch)) {
      prefill = kQwen3NoThinkPrefill;
    }
    if (prefill) {
      const std::vector<llama_token> t =
          TokenizeText(vocab, prefill, /*add_special=*/false,
                       /*parse_special=*/true);
      if (with_images) {
        media.suffix = t;
        media.n_tokens += t.size();
        summary.prompt_tokens = ToU32(media.n_tokens);
      } else {
        tokens.insert(tokens.end(), t.begin(), t.end());
        summary.prompt_tokens = ToU32(tokens.size());
      }
    }
    dp.grammar = params.grammar;
    dp.think_start_inside = false;
    dp.thought_start_inside = false;
  }

  if (with_images) {
    // Image embeddings have no token ids for KV-prefix reuse to compare:
    // evaluated from an empty cache, leaving nothing to reuse.
    dp.media = &media;
    summary.status = Generate(std::vector<llama_token>(), std::string(), dp,
                              callbacks, &summary);
    return summary;
  }
  summary.status =
      Generate(tokens, params.conversation_key, dp, callbacks, &summary);
  return summary;
}

std::optional<EngineStatus> LlamaEngine::EvalMediaPrompt(
    const MediaPrompt& media,
    const EngineCallbacks& callbacks) {
  Model& m = *model_;
  auto cancelled = [&]() {
    return callbacks.is_cancelled && callbacks.is_cancelled();
  };
  // A failed decode leaves the cache in an unknown state: start clean next
  // time (the prompt is never reused anyway). After a GPU failure nothing
  // more is done with the context.
  auto fail = [&](EngineStatus status) -> std::optional<EngineStatus> {
    m.kv_tokens.clear();
    m.kv_key.clear();
    if (molt_llama::BackendFailed()) {
      m.backend_failed = true;
      return EngineStatus::kBackendFailed;
    }
    llama_set_causal_attn(m.ctx, true);
    if (llama_memory_t mem = llama_get_memory(m.ctx)) {
      if (!molt_llama::MemoryClear(mem, true)) {
        m.backend_failed = true;
        return EngineStatus::kBackendFailed;
      }
    }
    return status;
  };
  struct BatchFree {
    void operator()(llama_batch_ext* b) const { llama_batch_ext_free(b); }
  };
  std::unique_ptr<llama_batch_ext, BatchFree> batch(
      llama_batch_ext_init(m.ctx));
  const int n_embd = llama_model_n_embd_inp(m.model);
  const size_t n_batch = static_cast<size_t>(std::max(1, m.n_batch));
  if (!batch || n_embd <= 0) return fail(EngineStatus::kDecodeFailed);
  const bool mrope = mtmd_decode_use_mrope(m.mtmd);

  const auto interval = std::chrono::milliseconds(kProgressIntervalMs);
  auto last_progress = Clock::now();
  size_t done = 0;
  auto progress = [&]() {
    const auto now = Clock::now();
    if (callbacks.on_progress && now - last_progress >= interval) {
      last_progress = now;
      callbacks.on_progress(ToU32(done), ToU32(media.n_tokens));
    }
  };

  // Text with positions from |n_past| on (an image before it may have taken
  // fewer positions than tokens: M-RoPE); logits for the last token when
  // |logits_last|.
  llama_pos n_past = 0;
  enum class Step { kOk, kCancelled, kFailed };
  auto decode_text = [&](const llama_token* toks, size_t n,
                         bool logits_last) -> Step {
    for (size_t j = 0; j < n;) {
      if (cancelled()) return Step::kCancelled;
      llama_batch_ext_clear(batch.get());
      const size_t take = std::min(n_batch, n - j);
      int32_t idx = -1;
      for (size_t k = 0; k < take; ++k) {
        idx = llama_batch_ext_add_token(batch.get(), 0, toks[j + k]);
        const llama_pos pos = n_past++;
        if (idx < 0 || !llama_batch_ext_set_pos(batch.get(), idx, &pos)) {
          return Step::kFailed;
        }
      }
      if (logits_last && j + take == n &&
          !llama_batch_ext_set_output_logits(batch.get(), idx, true)) {
        return Step::kFailed;
      }
      if (molt_llama::Process(m.ctx, LLAMA_PROCESS_TYPE_DECODE,
                              batch.get()) != 0) {
        return Step::kFailed;
      }
      m.kv_tokens.insert(m.kv_tokens.end(), toks + j, toks + j + take);
      j += take;
      done += take;
      progress();
    }
    return Step::kOk;
  };

  const size_t n_chunks = mtmd_input_chunks_size(media.chunks);
  for (size_t i = 0; i < n_chunks; ++i) {
    if (cancelled()) return EngineStatus::kCancelled;
    const mtmd_input_chunk* chunk = mtmd_input_chunks_get(media.chunks, i);
    if (!chunk) return fail(EngineStatus::kDecodeFailed);
    const bool last = i + 1 == n_chunks && media.suffix.empty();
    const mtmd_input_chunk_type type = mtmd_input_chunk_get_type(chunk);
    if (type == MTMD_INPUT_CHUNK_TYPE_TEXT) {
      size_t n = 0;
      const llama_token* toks = mtmd_input_chunk_get_tokens_text(chunk, &n);
      if (!toks && n > 0) return fail(EngineStatus::kDecodeFailed);
      const Step step = decode_text(toks, n, last);
      if (step == Step::kCancelled) return EngineStatus::kCancelled;
      if (step == Step::kFailed) return fail(EngineStatus::kDecodeFailed);
      continue;
    }
    // Only images are made from a request (no audio bitmaps), and the
    // prompt ends with text.
    const mtmd_image_tokens* image = mtmd_input_chunk_get_tokens_image(chunk);
    const size_t n_tok = mtmd_input_chunk_get_n_tokens(chunk);
    if (type != MTMD_INPUT_CHUNK_TYPE_IMAGE || !image || n_tok == 0 || last) {
      return fail(EngineStatus::kDecodeFailed);
    }
    // Some encoders (Gemma 3) attend to the whole image at once: it must
    // fit one batch and one ubatch (llama_decode asserts the latter).
    const bool non_causal = mtmd_decode_use_non_causal(m.mtmd, chunk);
    if (non_causal &&
        n_tok > std::min<size_t>(n_batch, llama_n_ubatch(m.ctx))) {
      return fail(EngineStatus::kPromptTooLong);
    }
    // The vision encoder: seconds on a CPU for a large image; cancellation
    // is checked before and after it.
    if (mtmd_encode_chunk(m.mtmd, chunk) != 0) {
      return fail(EngineStatus::kDecodeFailed);
    }
    if (cancelled()) return EngineStatus::kCancelled;
    const float* embd = mtmd_get_output_embd(m.mtmd);
    if (!embd) return fail(EngineStatus::kDecodeFailed);
    if (non_causal) llama_set_causal_attn(m.ctx, false);
    for (size_t j = 0; j < n_tok;) {
      if (cancelled()) {
        llama_set_causal_attn(m.ctx, true);
        return EngineStatus::kCancelled;
      }
      llama_batch_ext_clear(batch.get());
      const size_t take = std::min(n_batch, n_tok - j);
      for (size_t k = 0; k < take; ++k) {
        const size_t t = j + k;
        const llama_embd row = {embd + t * static_cast<size_t>(n_embd), 1,
                                static_cast<size_t>(n_embd)};
        const int32_t idx = llama_batch_ext_add_embd(batch.get(), 0, row);
        // [t, y, x, z] for M-RoPE (llama.cpp's mtmd-helper layout), else
        // one position per token.
        llama_pos pos[4];
        if (mrope) {
          const mtmd_decoder_pos d =
              mtmd_image_tokens_get_decoder_pos(image, n_past, t);
          pos[0] = static_cast<llama_pos>(d.t);
          pos[1] = static_cast<llama_pos>(d.y);
          pos[2] = static_cast<llama_pos>(d.x);
          pos[3] = static_cast<llama_pos>(d.z);
        } else {
          std::fill(std::begin(pos), std::end(pos),
                    n_past + static_cast<llama_pos>(t));
        }
        if (idx < 0 || !llama_batch_ext_set_pos(batch.get(), idx, pos)) {
          return fail(EngineStatus::kDecodeFailed);
        }
      }
      if (molt_llama::Process(m.ctx, LLAMA_PROCESS_TYPE_DECODE,
                              batch.get()) != 0) {
        return fail(EngineStatus::kDecodeFailed);
      }
      // No token ids for image cells; the count is what matters (the
      // cache's fill level).
      m.kv_tokens.insert(m.kv_tokens.end(), take, LLAMA_TOKEN_NULL);
      j += take;
      done += take;
      progress();
    }
    if (non_causal) llama_set_causal_attn(m.ctx, true);
    n_past += mtmd_input_chunk_get_n_pos(chunk);
  }
  if (!media.suffix.empty()) {
    const Step step =
        decode_text(media.suffix.data(), media.suffix.size(), true);
    if (step == Step::kCancelled) return EngineStatus::kCancelled;
    if (step == Step::kFailed) return fail(EngineStatus::kDecodeFailed);
  }
  return std::nullopt;
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
  if (!vocab || !mem || (prompt.empty() && !p.media)) {
    return EngineStatus::kDecodeFailed;
  }
  auto cancelled = [&]() {
    return callbacks.is_cancelled && callbacks.is_cancelled();
  };

  // ---- Constrained decoding ----
  // Set up before anything touches the KV cache, so a grammar llama.cpp
  // refuses changes nothing. Bounded work: the converter caps the
  // grammar's size, and the vocabulary is read once per model.
  std::unique_ptr<GrammarSampler> grammar;
  if (!p.grammar.empty()) {
    if (!m.grammar_vocab) m.grammar_vocab = GrammarVocab::Build(vocab);
    if (m.grammar_vocab) {
      grammar = GrammarSampler::Create(m.grammar_vocab.get(), p.grammar);
    }
    if (!grammar) return EngineStatus::kConstraintFailed;
  }

  // ---- KV prefix reuse ----
  size_t n_keep = 0;
  if (!p.media && !reuse_key.empty() && reuse_key == m.kv_key) {
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
  if (n_keep == 0 && !molt_llama::MemoryClear(mem, true)) {
    m.backend_failed = true;
    m.kv_tokens.clear();
    m.kv_key.clear();
    return EngineStatus::kBackendFailed;
  }
  m.kv_tokens.assign(prompt.begin(),
                     prompt.begin() + static_cast<std::ptrdiff_t>(n_keep));
  // A prompt with images leaves nothing reusable (its key is "").
  m.kv_key = p.media ? std::string() : reuse_key;
  summary->reused_tokens = ToU32(n_keep);

  auto fail_decode = [&]() {
    // The cache state is unknown after a failed decode: start clean next
    // time. After a GPU failure nothing more is done with the context.
    m.kv_tokens.clear();
    m.kv_key.clear();
    if (molt_llama::BackendFailed() || !molt_llama::MemoryClear(mem, true)) {
      m.backend_failed = true;
      return EngineStatus::kBackendFailed;
    }
    return EngineStatus::kDecodeFailed;
  };

  // ---- Prompt evaluation in n_batch-sized chunks ----
  const auto start = Clock::now();
  auto last_progress = start;
  const auto interval = std::chrono::milliseconds(kProgressIntervalMs);
  std::vector<llama_token> work(prompt.begin(), prompt.end());
  if (callbacks.on_progress) {
    // Tells the browser this request is running now (it may have waited
    // behind another one); its liveness clock starts here.
    callbacks.on_progress(ToU32(n_keep),
                          ToU32(p.media ? p.media->n_tokens : work.size()));
  }
  if (p.media) {
    if (std::optional<EngineStatus> failed =
            EvalMediaPrompt(*p.media, callbacks)) {
      return *failed;
    }
    last_progress = Clock::now();
  }
  for (size_t i = n_keep; i < work.size();) {
    if (cancelled()) {
      return EngineStatus::kCancelled;  // kv_tokens matches what was decoded
    }
    const size_t n =
        std::min<size_t>(static_cast<size_t>(m.n_batch), work.size() - i);
    llama_batch batch =
        llama_batch_get_one(work.data() + i, static_cast<int32_t>(n));
    if (molt_llama::Decode(m.ctx, batch) != 0) return fail_decode();
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
    if (grammar) {
      // No reasoning filters: the visible text is exactly what the grammar
      // matched (valid UTF-8 by construction; ToDisplayText changes
      // nothing).
      out += stop.Feed(ToDisplayText(utf8.Push(piece)));
      return;
    }
    std::string text = harmony.Feed(piece);
    if (gemma_thoughts) text = gemma.Feed(text);
    out += stop.Feed(ToDisplayText(utf8.Push(think.Feed(text))));
  };
  auto send = [&]() {
    if (out.empty()) return;
    EmitChunks(std::move(out), callbacks);
    out.clear();
    last_progress = Clock::now();
  };
  auto hiding = [&]() {
    return !grammar &&
           (harmony.hiding() || gemma.hiding() || think.in_think());
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

    llama_token tok;
    if (grammar) {
      const float* logits = molt_llama::GetLogitsIth(m.ctx, -1);
      if (!logits) {
        status = fail_decode();
        break;
      }
      const std::optional<llama_token> next = grammar->Sample(sampler, logits);
      if (!next) {
        // No token of this vocabulary continues the grammar.
        status = EngineStatus::kConstraintFailed;
        break;
      }
      tok = *next;
      if (!llama_vocab_is_eog(vocab, tok) && !grammar->Accept(tok)) {
        status = EngineStatus::kConstraintFailed;  // not reached
        break;
      }
    } else {
      tok = molt_llama::SamplerSample(sampler, m.ctx, -1);
      if (molt_llama::BackendFailed()) {
        status = fail_decode();
        break;
      }
    }
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
    if (callbacks.on_progress && hiding() &&
        Clock::now() - last_progress >= interval) {
      // Long hidden reasoning sends no text: keep the browser informed.
      last_progress = Clock::now();
      callbacks.on_progress(ToU32(hidden_tokens), ToU32(p.max_hidden));
    }
    if (stop.stopped()) {
      // The stop text is in the KV cache only up to this token; the next
      // turn re-evaluates from the common prefix anyway.
      status = EngineStatus::kStopSequence;
      break;
    }

    if (molt_llama::Decode(m.ctx, llama_batch_get_one(&tok, 1)) != 0) {
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
      if (molt_llama::Decode(m.ctx, force_batch) != 0) {
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
      status != EngineStatus::kDecodeFailed &&
      status != EngineStatus::kBackendFailed &&
      status != EngineStatus::kConstraintFailed) {
    status = EngineStatus::kStopSequence;
  }
  return status;
}

}  // namespace molt_ai
