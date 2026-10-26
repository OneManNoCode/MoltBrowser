// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Constrained decoding in the inference service: llama.cpp's grammar
// sampler, wrapped so that its accept path, which throws or aborts on a
// token the grammar can't take, is unreachable. Every token is checked
// (grammar_tokens.h: banned tokens, strict UTF-8 across pieces, then the
// grammar itself on that one token) before it is accepted. The checked
// llama.cpp calls are listed at the top of llama_engine.cc.
//
// Sampling is rejection-first, as llama.cpp's own common sampler does it:
// the usual chain (top-k, top-p, temperature, dist) picks from all logits;
// if the grammar takes that token it is used. Otherwise the vocabulary is
// filtered (bans, UTF-8, then the grammar over every remaining candidate)
// and the chain picks again from what is left. Most tokens of a model that
// follows the format take the cheap path.
//
// Std and llama.h only, like LlamaEngine; built and tested standalone.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_GRAMMAR_SAMPLER_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_GRAMMAR_SAMPLER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/services/molt_ai/inference/engine/grammar_tokens.h"
#include "chrome/services/molt_ai/inference/engine/raw_ptr_exclusion.h"
#include "third_party/llama_cpp/include/llama.h"

namespace molt_ai {

// Per-token facts for one model's vocabulary (ClassifyGrammarToken() and
// the piece as the grammar sees it), built once and shared by every
// constrained generation of that model.
class GrammarVocab {
 public:
  static std::unique_ptr<GrammarVocab> Build(const llama_vocab* vocab);
  ~GrammarVocab();
  GrammarVocab(const GrammarVocab&) = delete;
  GrammarVocab& operator=(const GrammarVocab&) = delete;

  const llama_vocab* vocab() const { return vocab_; }
  int32_t size() const { return static_cast<int32_t>(classes_.size()); }
  // |token| in [0, size()).
  GrammarToken token_class(llama_token token) const {
    return classes_[static_cast<size_t>(token)];
  }
  std::string_view piece(llama_token token) const;

 private:
  explicit GrammarVocab(const llama_vocab* vocab);

  MOLT_RAW_PTR_EXCLUSION const llama_vocab* const vocab_;
  std::vector<GrammarToken> classes_;
  // Pieces of kAllowed tokens, back to back; offsets_[t]..offsets_[t + 1].
  std::vector<uint32_t> offsets_;
  std::string pieces_;
};

class GrammarSampler {
 public:
  // nullptr if llama.cpp refuses |gbnf| (start rule "root"). |vocab| must
  // outlive the sampler.
  static std::unique_ptr<GrammarSampler> Create(const GrammarVocab* vocab,
                                                const std::string& gbnf);
  ~GrammarSampler();
  GrammarSampler(const GrammarSampler&) = delete;
  GrammarSampler& operator=(const GrammarSampler&) = delete;

  // The next token, picked by |chain| from |logits| (vocab size floats)
  // among the tokens the grammar allows now; nullopt if none is (the
  // grammar can't continue with this vocabulary). An end-of-generation
  // token is returned only once the grammar is complete.
  std::optional<llama_token> Sample(llama_sampler* chain, const float* logits);

  // Records |token| in the grammar. Only the token the last Sample()
  // returned, and not an end-of-generation token; anything else is refused
  // (returns false) and leaves the grammar as it was.
  bool Accept(llama_token token);

  // How many times Sample() had to filter the whole vocabulary.
  uint32_t full_passes() const { return full_passes_; }

 private:
  GrammarSampler(const GrammarVocab* vocab, llama_sampler* grammar);

  // The bans, the UTF-8 check and the grammar, for one token.
  bool Allows(llama_token token);
  // The bans and the UTF-8 check only.
  bool Admissible(llama_token token) const;

  MOLT_RAW_PTR_EXCLUSION const GrammarVocab* const vocab_;
  MOLT_RAW_PTR_EXCLUSION llama_sampler* const grammar_;
  Utf8Prefix utf8_;
  // The token the last Sample() checked and returned, if Accept() may take
  // it; -1 otherwise.
  llama_token last_allowed_ = -1;
  std::vector<llama_token_data> candidates_;
  uint32_t full_passes_ = 0;
};

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_GRAMMAR_SAMPLER_H_
