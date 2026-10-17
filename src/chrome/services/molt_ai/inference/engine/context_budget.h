// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Pure (std-only) context-window arithmetic for the on-device inference
// engine (LlamaEngine, in the sandboxed inference service):
//   - how large a llama.cpp context to allocate for a model on this machine
//   - how to fit system prompt + retrieved context + history + the new user
//     turn + a reply reserve into that window WITHOUT ever truncating the
//     system prompt (the old code cut tokens off the FRONT of the templated
//     prompt, dropping BOS, the chat-template header and the instructions).
//
// Kept free of llama.cpp / base types so it is unit-tested standalone
// (context_budget_unittest.cc).

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CONTEXT_BUDGET_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CONTEXT_BUDGET_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace molt_ai {

// Never allocate less than this (the previous fixed size), unless the model
// itself was trained on a shorter window.
inline constexpr int kMinContextTokens = 2048;
// Upper bound on llama_context_params.n_batch (prompt tokens per decode
// call). Prompts longer than this are decoded in n_batch-sized chunks.
inline constexpr int kMaxBatchTokens = 2048;

struct ContextSizingInput {
  uint64_t total_ram_bytes = 0;
  uint64_t model_file_bytes = 0;
  int n_ctx_train = 0;              // llama_model_n_ctx_train; 0 = unknown
  uint64_t kv_bytes_per_token = 0;  // EstimateKvBytesPerToken; 0 = unknown
};

struct ContextSizing {
  int n_ctx = kMinContextTokens;
  int n_batch = kMinContextTokens;
};

// RAM tier -> target window: <=16 GB: 8K, <=32 GB: 16K, above: 32K. Then
// capped by the model's training window and by a KV-cache budget of half
// the RAM left after the weights and a 4 GB OS/browser headroom. Rounded
// down to a multiple of 256; never below kMinContextTokens (or n_ctx_train
// if that is smaller).
ContextSizing ComputeContextSizing(const ContextSizingInput& in);

// f16 K+V cache bytes per token: 2 (K,V) * n_layer * head_dim * n_head_kv *
// 2 bytes, +10% for padding / per-layer head-dim differences (e.g. Gemma 3).
// Returns 0 if any input is non-positive.
uint64_t EstimateKvBytesPerToken(int n_layer,
                                 int n_embd,
                                 int n_head,
                                 int n_head_kv);

// Visible-answer token budget used when the user hasn't configured
// max_tokens: n_ctx / 8 clamped to [512, 2048].
int AdaptiveReplyTokens(int n_ctx);

// Hidden reasoning (<think> / Harmony analysis) budget used when the caller
// doesn't specify one: n_ctx / 8 clamped to [256, 1024]. Reasoning tokens
// do NOT count against the visible-answer budget.
int AdaptiveReasoningTokens(int n_ctx);

struct PromptBudgetInput {
  int n_ctx = 0;
  // Tokens to keep free for generation (visible answer + reasoning).
  int reply_reserve = 0;
  // Never trimmed (system prompt incl. instructions and template header).
  int system_tokens = 0;
  // The new user turn — trimmed only as a last resort.
  int user_tokens = 0;
  // Retrieved page / PDF / attachment / memory context — trimmed after
  // history has been cut down to its floor.
  int context_tokens = 0;
  // History messages, oldest first.
  std::vector<int> history_tokens;
  // history_starts_turn[i] is true when history message i begins a turn
  // (a user message). Messages are only dropped in whole turns so the kept
  // history never starts with an orphaned assistant reply. Missing / short
  // vector => every message starts a turn.
  std::vector<bool> history_starts_turn;
  // Chat-template tokens added per message (role markers etc.).
  int per_message_overhead = 0;
  // Slack for template / tokenizer drift.
  int safety_margin = 8;
};

struct PromptBudgetPlan {
  // History messages [history_keep_from, end) are kept.
  size_t history_keep_from = 0;
  // Max context tokens to keep; -1 = keep all, 0 = drop the context.
  int context_token_limit = -1;
  // Max user-turn tokens to keep; -1 = keep all.
  int user_token_limit = -1;
  // Generation budget that fits after the prompt (<= reply_reserve).
  int reply_tokens = 0;
  // False when even system + a minimal user turn + 16 reply tokens can't
  // fit; the caller should refuse with a clear error instead of decoding.
  bool fits = true;
};

// Order of sacrifice: (1) oldest whole history turns, down to a floor of a
// quarter of the prompt budget; (2) the retrieved context; (3) the rest of
// the history; (4) the tail of the user turn and the reply reserve. The
// system prompt is never touched.
PromptBudgetPlan PlanPromptBudget(const PromptBudgetInput& in);

}  // namespace molt_ai

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_CONTEXT_BUDGET_H_
