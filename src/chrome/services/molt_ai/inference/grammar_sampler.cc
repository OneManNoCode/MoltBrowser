// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// llama.cpp calls used here, checked for exceptions and aborts at b11641
// (also listed at the top of llama_engine.cc):
//  - llama_sampler_init_grammar(): the GBNF parser catches std::exception
//    (std::runtime_error, std::stoul's out_of_range) and returns nullptr;
//    its one GGML_ASSERT (n_prev_rules >= 1) can't fail, and the stack
//    builder's GGML_ABORT needs rules the parser never produces. A rejected
//    grammar is printed to stderr in full: never reached with the
//    converter's output (json_schema_grammar.h), which is tested to parse.
//  - llama_sampler_apply() on the grammar sampler: GGML_ASSERTs on a null
//    vocab (never) and on an empty stack set, which only an accepted token
//    the grammar rejected can produce (never: see Accept()). The candidate
//    check recurses once per character of a piece (pieces are capped at
//    kMaxGrammarPieceBytes).
//  - llama_sampler_accept() on the grammar sampler: throws if the token
//    leaves no grammar stack and aborts on an end-of-generation token
//    before the value is complete. Only tokens that passed Allows() (the
//    same grammar check on exactly the same code points: strict UTF-8, no
//    NUL) are accepted, and never an end-of-generation token.
//  - llama_sampler_apply() on the chain (top-k, top-p, temp, dist): no
//    throw; softmax asserts a non-empty array, which it always is.
//  - llama_token_to_piece (catches, local patch), llama_vocab_get_attr,
//    llama_vocab_get_text, llama_vocab_is_eog, llama_vocab_n_tokens: ids in
//    [0, n_vocab) only.

#include "chrome/services/molt_ai/inference/grammar_sampler.h"

#include <cmath>
#include <utility>

namespace molt_ai {

// ---- GrammarVocab -----------------------------------------------------------

GrammarVocab::GrammarVocab(const llama_vocab* vocab) : vocab_(vocab) {}
GrammarVocab::~GrammarVocab() = default;

// static
std::unique_ptr<GrammarVocab> GrammarVocab::Build(const llama_vocab* vocab) {
  if (!vocab) return nullptr;
  const int32_t n = llama_vocab_n_tokens(vocab);
  if (n <= 0) return nullptr;
  std::unique_ptr<GrammarVocab> out(new GrammarVocab(vocab));
  out->classes_.resize(static_cast<size_t>(n));
  out->offsets_.assign(static_cast<size_t>(n) + 1, 0);
  // One byte more than any piece kept, so a longer one is reported as such
  // (a negative size) rather than cut.
  char buf[kMaxGrammarPieceBytes + 1];
  for (llama_token t = 0; t < n; ++t) {
    GrammarTokenFacts facts;
    facts.end_of_generation = llama_vocab_is_eog(vocab, t);
    const llama_token_attr attr = llama_vocab_get_attr(vocab, t);
    facts.control =
        attr == LLAMA_TOKEN_ATTR_UNDEFINED ||
        (attr & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_UNKNOWN |
                 LLAMA_TOKEN_ATTR_UNUSED));
    facts.user_defined = attr & LLAMA_TOKEN_ATTR_USER_DEFINED;
    const char* text = llama_vocab_get_text(vocab, t);
    facts.text = text ? std::string_view(text) : std::string_view();
    // special=true: the rendering llama.cpp's grammar uses (its piece
    // cache). For the tokens kept it is also the visible text.
    const int32_t len = llama_token_to_piece(vocab, t, buf,
                                             static_cast<int32_t>(sizeof(buf)),
                                             /*lstrip=*/0, /*special=*/true);
    GrammarToken cls = GrammarToken::kBanned;
    if (len >= 0 && static_cast<size_t>(len) <= kMaxGrammarPieceBytes) {
      facts.piece = std::string_view(buf, static_cast<size_t>(len));
      cls = ClassifyGrammarToken(facts);
    } else if (facts.end_of_generation) {
      cls = GrammarToken::kEndOfGeneration;
    }
    out->classes_[static_cast<size_t>(t)] = cls;
    if (cls == GrammarToken::kAllowed) out->pieces_.append(facts.piece);
    out->offsets_[static_cast<size_t>(t) + 1] =
        static_cast<uint32_t>(out->pieces_.size());
  }
  return out;
}

std::string_view GrammarVocab::piece(llama_token token) const {
  const size_t t = static_cast<size_t>(token);
  return std::string_view(pieces_).substr(offsets_[t],
                                          offsets_[t + 1] - offsets_[t]);
}

// ---- GrammarSampler ---------------------------------------------------------

GrammarSampler::GrammarSampler(const GrammarVocab* vocab,
                               llama_sampler* grammar)
    : vocab_(vocab), grammar_(grammar) {}

GrammarSampler::~GrammarSampler() {
  llama_sampler_free(grammar_);
}

// static
std::unique_ptr<GrammarSampler> GrammarSampler::Create(
    const GrammarVocab* vocab,
    const std::string& gbnf) {
  // An empty grammar would make llama.cpp return a sampler that allows
  // everything.
  if (!vocab || gbnf.empty()) return nullptr;
  llama_sampler* grammar =
      llama_sampler_init_grammar(vocab->vocab(), gbnf.c_str(), "root");
  if (!grammar) return nullptr;
  return std::unique_ptr<GrammarSampler>(new GrammarSampler(vocab, grammar));
}

bool GrammarSampler::Admissible(llama_token token) const {
  if (token < 0 || token >= vocab_->size()) return false;
  switch (vocab_->token_class(token)) {
    case GrammarToken::kBanned:
      return false;
    case GrammarToken::kEndOfGeneration:
      // Never in the middle of a character.
      return !utf8_.incomplete();
    case GrammarToken::kAllowed:
      return utf8_.CanAppend(vocab_->piece(token));
  }
  return false;
}

bool GrammarSampler::Allows(llama_token token) {
  if (!Admissible(token)) return false;
  llama_token_data data = {token, 0.0f, 0.0f};
  llama_token_data_array one = {&data, 1, -1, false};
  llama_sampler_apply(grammar_, &one);
  // The grammar marks what it rejects with -INFINITY.
  return std::isfinite(data.logit);
}

std::optional<llama_token> GrammarSampler::Sample(llama_sampler* chain,
                                                  const float* logits) {
  last_allowed_ = -1;
  const int32_t n = vocab_->size();
  auto chosen = [&](llama_token token) -> std::optional<llama_token> {
    if (vocab_->token_class(token) == GrammarToken::kAllowed) {
      last_allowed_ = token;
    }
    return token;
  };

  // The unconstrained pick, if the grammar takes it.
  candidates_.resize(static_cast<size_t>(n));
  for (llama_token t = 0; t < n; ++t) {
    candidates_[static_cast<size_t>(t)] = {t, logits[t], 0.0f};
  }
  llama_token_data_array all = {candidates_.data(), candidates_.size(), -1,
                                false};
  llama_sampler_apply(chain, &all);
  if (all.selected >= 0 && static_cast<size_t>(all.selected) < all.size) {
    const llama_token t = all.data[all.selected].id;
    if (Allows(t)) return chosen(t);
  }

  // Otherwise: every token the grammar takes, then the chain again.
  ++full_passes_;
  candidates_.clear();
  for (llama_token t = 0; t < n; ++t) {
    if (std::isfinite(logits[t]) && Admissible(t)) {
      candidates_.push_back({t, logits[t], 0.0f});
    }
  }
  if (candidates_.empty()) return std::nullopt;
  llama_token_data_array allowed = {candidates_.data(), candidates_.size(), -1,
                                    false};
  llama_sampler_apply(grammar_, &allowed);
  size_t kept = 0;
  llama_token best = -1;
  float best_logit = -INFINITY;
  for (const llama_token_data& c : candidates_) {
    if (!std::isfinite(c.logit)) continue;
    if (best < 0 || c.logit > best_logit) {
      best = c.id;
      best_logit = c.logit;
    }
    candidates_[kept++] = c;
  }
  if (kept == 0) return std::nullopt;
  candidates_.resize(kept);
  allowed = {candidates_.data(), candidates_.size(), -1, false};
  llama_sampler_apply(chain, &allowed);
  if (allowed.selected >= 0 &&
      static_cast<size_t>(allowed.selected) < allowed.size) {
    const llama_token t = allowed.data[allowed.selected].id;
    if (Allows(t)) return chosen(t);
  }
  // The chain picked something it shouldn't have (a zero-probability
  // candidate): take the most likely allowed token instead.
  if (Allows(best)) return chosen(best);
  return std::nullopt;
}

bool GrammarSampler::Accept(llama_token token) {
  // Only the token Sample() just checked: accepting anything else could
  // empty the grammar's stacks, which throws inside llama.cpp.
  if (token < 0 || token != last_allowed_) return false;
  last_allowed_ = -1;
  utf8_.Append(vocab_->piece(token));
  llama_sampler_accept(grammar_, token);
  return true;
}

}  // namespace molt_ai
