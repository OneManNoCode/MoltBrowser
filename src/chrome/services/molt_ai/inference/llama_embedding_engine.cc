// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Exceptions: llama.cpp is built with exceptions, this file isn't, so an
// exception escaping a llama_* call aborts the inference process. Checked at
// b11641, in addition to the list at the top of llama_engine.cc (which this
// file's calls also appear in):
//  - llama_model_load_from_file_ptr, llama_init_from_model: catch everything
//    and return nullptr (an embeddings context with a pooling type is built
//    there too: the context constructor and its first sched_reserve()).
//  - llama_tokenize: catches (local patch); also used once, with
//    parse_special=true, on the browser's terminator text.
//  - llama_decode with embeddings on: models without a KV cache (BERT-like
//    encoders) go through llama_context::encode(). Neither throws on this
//    path: sched_reserve() only runs after a setting changed (none is
//    changed after creation), output_reserve() reports failure by return
//    value. Both GGML_ASSERT (abort) that a batch fits n_batch, and encode()
//    that it fits n_ubatch: every text is cut to min(n_ctx, n_batch,
//    n_ubatch) tokens first. UNSPECIFIED and RANK pooling would abort or
//    return other shapes: only MEAN, CLS and LAST are ever set.
//  - llama_get_embeddings_seq: synchronize() and a std::map find, no throw;
//    nullptr when the sequence has no pooled vector. (Not
//    llama_get_embeddings_ith: it GGML_ABORTs on a bad index in builds
//    without NDEBUG.)
//  - llama_pooling_type, llama_n_ubatch, llama_model_n_embd_out,
//    llama_vocab_get_add_eos, llama_vocab_get_add_sep, llama_get_memory,
//    llama_memory_clear (no-op on nullptr): plain getters / no throw.
// Re-check these when updating the vendored llama.cpp.

#include "chrome/services/molt_ai/inference/llama_embedding_engine.h"

#include <algorithm>
#include <climits>
#include <utility>

#include "chrome/services/molt_ai/inference/engine/embedding_utils.h"
#include "third_party/llama_cpp/include/llama.h"

namespace molt_ai {

namespace {

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

// Tokenizes |text|; nullopt on a tokenizer error.
std::optional<std::vector<llama_token>> Tokenize(const llama_vocab* vocab,
                                                 const std::string& text,
                                                 bool add_special,
                                                 bool parse_special) {
  if (text.size() > static_cast<size_t>(INT32_MAX) - 16) return std::nullopt;
  std::vector<llama_token> tokens(text.size() + 16);
  int32_t n = llama_tokenize(vocab, text.c_str(),
                             static_cast<int32_t>(text.size()), tokens.data(),
                             static_cast<int32_t>(tokens.size()), add_special,
                             parse_special);
  // INT32_MIN is an error, not "buffer too small".
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

std::string ReadModelArch(const llama_model* model) {
  char buf[64] = {0};
  const int32_t n = llama_model_meta_val_str(model, "general.architecture",
                                             buf, sizeof(buf));
  return n > 0 ? std::string(buf, std::min<size_t>(static_cast<size_t>(n),
                                                   sizeof(buf) - 1))
               : std::string();
}

// (`enum`: llama.h also declares a function named llama_pooling_type.)
enum llama_pooling_type ToLlama(EmbeddingPoolingType pooling) {
  switch (pooling) {
    case EmbeddingPoolingType::kMean:
      return LLAMA_POOLING_TYPE_MEAN;
    case EmbeddingPoolingType::kCls:
      return LLAMA_POOLING_TYPE_CLS;
    case EmbeddingPoolingType::kLast:
      return LLAMA_POOLING_TYPE_LAST;
  }
  return LLAMA_POOLING_TYPE_MEAN;
}

uint32_t ToU32(int64_t v) {
  if (v < 0) return 0;
  return v > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(v);
}

}  // namespace

struct LlamaEmbeddingEngine::Model {
  ~Model() {
    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    file.reset();  // only now: lazily read tensors go through this stream
  }

  ScopedStdioFile file;
  llama_model* model = nullptr;
  llama_context* ctx = nullptr;
  const llama_vocab* vocab = nullptr;
  size_t native_dimensions = 0;
  size_t dimensions = 0;
  // Tokens per text, within the context's n_ctx, n_batch and n_ubatch.
  size_t max_tokens = 0;
  // The tokenizer ends texts with a marker of its own (EOS / SEP).
  bool keep_last = false;
  llama_token terminator = -1;
};

LlamaEmbeddingEngine::LlamaEmbeddingEngine() = default;
LlamaEmbeddingEngine::~LlamaEmbeddingEngine() = default;

bool LlamaEmbeddingEngine::loaded() const {
  return model_ && model_->ctx;
}

EmbeddingLoadResult LlamaEmbeddingEngine::Load(
    ScopedStdioFile scoped_file,
    const EmbeddingLoadParams& params) {
  EmbeddingLoadResult result;
  if (!scoped_file || model_) return result;  // closes |scoped_file|
  if (params.dimensions == 0 || params.dimensions > kMaxDimensions ||
      params.max_tokens_per_text < kMinTokensPerText ||
      params.max_tokens_per_text > kMaxTokensPerText ||
      params.terminator.size() > kMaxTerminatorBytes) {
    return result;
  }
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

  if (!LlamaEngine::InitBackendOnce()) {
    result.status = EngineLoadStatus::kUnsupportedCpu;
    return result;
  }
  if (ggml_backend_dev_count() == 0) {
    result.status = EngineLoadStatus::kBackendUnavailable;
    return result;
  }

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = -1;  // all layers on the GPU (Metal on macOS)
  m->model = llama_model_load_from_file_ptr(file, model_params);
  if (!m->model) return result;  // kInvalidModelFile

  const int32_t native = llama_model_n_embd_out(m->model);
  if (native <= 0 || static_cast<uint32_t>(native) < params.dimensions) {
    return result;  // not the model the browser expects
  }

  // One text per decode, so the context holds one text's tokens.
  uint32_t n_ctx = params.max_tokens_per_text;
  const int32_t n_ctx_train = llama_model_n_ctx_train(m->model);
  if (n_ctx_train > 0) {
    n_ctx = std::min(n_ctx, static_cast<uint32_t>(n_ctx_train));
  }
  if (n_ctx < kMinTokensPerText) return result;

  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = n_ctx;
  // Encoders (non-causal attention) need the whole text in one ubatch.
  ctx_params.n_batch = n_ctx;
  ctx_params.n_ubatch = n_ctx;
  ctx_params.n_seq_max = 1;
  ctx_params.embeddings = true;
  ctx_params.pooling_type = ToLlama(params.pooling);
  ctx_params.no_perf = true;
  const int32_t threads =
      static_cast<int32_t>(std::clamp<uint32_t>(params.cpu_threads, 1, 256));
  ctx_params.n_threads = threads;
  ctx_params.n_threads_batch = threads;
  m->ctx = llama_init_from_model(m->model, ctx_params);
  if (!m->ctx) {
    result.status = EngineLoadStatus::kOutOfMemory;
    return result;
  }
  if (llama_pooling_type(m->ctx) != ToLlama(params.pooling)) return result;

  m->vocab = llama_model_get_vocab(m->model);
  if (!m->vocab) return result;
  const int32_t n_vocab = llama_vocab_n_tokens(m->vocab);
  if (n_vocab <= 0) return result;
  if (!params.terminator.empty()) {
    // Browser-authored text naming a special token: parse_special=true so
    // it becomes that token, which must exist as a single token.
    std::optional<std::vector<llama_token>> t =
        Tokenize(m->vocab, params.terminator, /*add_special=*/false,
                 /*parse_special=*/true);
    if (!t || t->size() != 1 || (*t)[0] < 0 || (*t)[0] >= n_vocab) {
      return result;
    }
    m->terminator = (*t)[0];
  }
  m->keep_last =
      llama_vocab_get_add_eos(m->vocab) || llama_vocab_get_add_sep(m->vocab);
  m->native_dimensions = static_cast<size_t>(native);
  m->dimensions = params.dimensions;
  m->max_tokens = std::min({static_cast<size_t>(llama_n_ctx(m->ctx)),
                            static_cast<size_t>(llama_n_batch(m->ctx)),
                            static_cast<size_t>(llama_n_ubatch(m->ctx)),
                            static_cast<size_t>(n_ctx)});
  if (m->max_tokens < kMinTokensPerText) return result;

  result.status = EngineLoadStatus::kOk;
  result.info.architecture = ReadModelArch(m->model);
#if defined(GGML_USE_METAL)
  result.info.backend = llama_supports_gpu_offload() ? "metal" : "cpu";
#else
  result.info.backend = "cpu";
#endif
  result.info.native_dimensions = ToU32(native);
  result.info.dimensions = params.dimensions;
  result.info.max_tokens_per_text = ToU32(static_cast<int64_t>(m->max_tokens));
  result.info.vocab_size = ToU32(n_vocab);
  model_ = std::move(m);
  return result;
}

std::optional<std::vector<float>> LlamaEmbeddingEngine::Embed(
    const std::vector<std::string>& texts,
    const std::function<bool()>& is_cancelled) {
  if (!loaded()) return std::nullopt;
  Model& m = *model_;
  llama_memory_t mem = llama_get_memory(m.ctx);  // nullptr for encoders
  std::vector<float> out(texts.size() * m.dimensions, 0.0f);
  for (size_t i = 0; i < texts.size(); ++i) {
    if (is_cancelled && is_cancelled()) return std::nullopt;
    // Page text: plain text (parse_special=false), with the tokenizer's own
    // BOS/CLS/EOS/SEP (add_special=true).
    std::optional<std::vector<llama_token>> tokens =
        Tokenize(m.vocab, texts[i], /*add_special=*/true,
                 /*parse_special=*/false);
    if (!tokens) return std::nullopt;
    std::vector<llama_token> fitted = FitEmbeddingTokens(
        std::move(*tokens), m.max_tokens, m.keep_last, m.terminator);
    if (fitted.empty()) continue;  // no tokens: an all-zero vector

    // Each text is its own sequence 0, from position 0.
    llama_memory_clear(mem, /*data=*/true);
    if (llama_decode(m.ctx, llama_batch_get_one(
                                fitted.data(),
                                static_cast<int32_t>(fitted.size()))) != 0) {
      llama_memory_clear(mem, true);
      return std::nullopt;
    }
    const float* pooled = llama_get_embeddings_seq(m.ctx, 0);
    if (!pooled ||
        !TruncateAndNormalize(pooled, m.native_dimensions, m.dimensions,
                              out.data() + i * m.dimensions)) {
      llama_memory_clear(mem, true);
      return std::nullopt;
    }
  }
  llama_memory_clear(mem, true);
  return out;
}

}  // namespace molt_ai
