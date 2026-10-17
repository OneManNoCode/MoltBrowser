// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/context_budget.h"

#include <algorithm>

namespace molt_ai {

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr uint64_t kOsBrowserHeadroom = 4 * kGiB;
// A context excerpt shorter than this is noise; drop it entirely instead.
constexpr int kMinUsefulContextTokens = 64;
// Smallest user turn worth sending after a last-resort truncation.
constexpr int kMinUserTokens = 32;
// Smallest reply budget that still makes sense.
constexpr int kMinReplyTokens = 16;
// The reply reserve is only borrowed from down to this many tokens.
constexpr int kReplyFloorTokens = 256;

int RoundDownTo(int v, int multiple) {
  return v - (v % multiple);
}

}  // namespace

uint64_t EstimateKvBytesPerToken(int n_layer,
                                 int n_embd,
                                 int n_head,
                                 int n_head_kv) {
  if (n_layer <= 0 || n_embd <= 0 || n_head <= 0 || n_head_kv <= 0) return 0;
  const uint64_t head_dim = static_cast<uint64_t>(n_embd / n_head);
  const uint64_t kv_dim = head_dim * static_cast<uint64_t>(n_head_kv);
  const uint64_t bytes = 2ULL /*K,V*/ * static_cast<uint64_t>(n_layer) *
                         kv_dim * 2ULL /*f16*/;
  return bytes + bytes / 10;
}

ContextSizing ComputeContextSizing(const ContextSizingInput& in) {
  int n_ctx;
  if (in.total_ram_bytes <= 16 * kGiB) {
    n_ctx = 8192;
  } else if (in.total_ram_bytes <= 32 * kGiB) {
    n_ctx = 16384;
  } else {
    n_ctx = 32768;
  }

  if (in.n_ctx_train > 0) n_ctx = std::min(n_ctx, in.n_ctx_train);

  if (in.kv_bytes_per_token > 0) {
    const uint64_t reserved = in.model_file_bytes + kOsBrowserHeadroom;
    const uint64_t spare =
        in.total_ram_bytes > reserved ? in.total_ram_bytes - reserved : 0;
    const uint64_t kv_budget = spare / 2;
    const uint64_t by_kv = kv_budget / in.kv_bytes_per_token;
    if (by_kv < static_cast<uint64_t>(n_ctx)) n_ctx = static_cast<int>(by_kv);
  }

  n_ctx = RoundDownTo(n_ctx, 256);
  int floor = kMinContextTokens;
  if (in.n_ctx_train > 0) floor = std::min(floor, in.n_ctx_train);
  n_ctx = std::max(n_ctx, floor);

  ContextSizing out;
  out.n_ctx = n_ctx;
  out.n_batch = std::min(n_ctx, kMaxBatchTokens);
  return out;
}

int AdaptiveReplyTokens(int n_ctx) {
  return std::clamp(n_ctx / 8, 512, 2048);
}

int AdaptiveReasoningTokens(int n_ctx) {
  return std::clamp(n_ctx / 8, 256, 1024);
}

PromptBudgetPlan PlanPromptBudget(const PromptBudgetInput& in) {
  PromptBudgetPlan plan;
  const int avail = std::max(0, in.n_ctx - in.safety_margin);
  // The reply reserve may claim at most half the window.
  int reply = std::max(0, std::min(in.reply_reserve, avail / 2));

  const size_t n_hist = in.history_tokens.size();
  const int ov = in.per_message_overhead;
  auto starts_turn = [&](size_t i) {
    return i >= in.history_starts_turn.size() || in.history_starts_turn[i];
  };
  // suffix[i] = cost of keeping history messages [i, n_hist).
  std::vector<int> suffix(n_hist + 1, 0);
  for (size_t i = n_hist; i-- > 0;) {
    suffix[i] = suffix[i + 1] + std::max(0, in.history_tokens[i]) + ov;
  }
  auto next_turn = [&](size_t from) {
    size_t i = from + 1;
    while (i < n_hist && !starts_turn(i)) ++i;
    return i;
  };

  int user = std::max(0, in.user_tokens);
  int context = std::max(0, in.context_tokens);
  size_t keep_from = 0;
  const int system_cost = std::max(0, in.system_tokens) + ov;
  auto prompt_cost = [&]() {
    return system_cost + context + suffix[keep_from] + user + ov;
  };
  auto prompt_avail = [&]() { return avail - reply; };

  // (1) Oldest whole turns, while the history is above its floor.
  const int history_floor =
      std::max(0, (prompt_avail() - system_cost - user - ov) / 4);
  while (prompt_cost() > prompt_avail() && keep_from < n_hist &&
         suffix[keep_from] > history_floor) {
    keep_from = next_turn(keep_from);
  }

  // (2) The retrieved context.
  if (prompt_cost() > prompt_avail() && context > 0) {
    const int room = prompt_avail() - (prompt_cost() - context);
    context = room >= kMinUsefulContextTokens ? room : 0;
    plan.context_token_limit = context;
  }

  // (3) The rest of the history.
  while (prompt_cost() > prompt_avail() && keep_from < n_hist) {
    keep_from = next_turn(keep_from);
  }

  // (4) Borrow from the reply reserve (down to a floor), then cut the tail
  // of the user turn, then give up reply tokens as a last resort.
  int over = prompt_cost() - prompt_avail();
  if (over > 0) {
    const int borrow =
        std::min(over, reply - std::min(reply, kReplyFloorTokens));
    reply -= borrow;
    over -= borrow;
  }
  if (over > 0 && user > 0) {
    const int min_user = std::min(user, kMinUserTokens);
    const int cut = std::min(over, user - min_user);
    user -= cut;
    over -= cut;
    plan.user_token_limit = user;
  }
  if (over > 0) {
    reply -= over;
  }

  plan.history_keep_from = keep_from;
  plan.reply_tokens = std::max(0, reply);
  plan.fits = reply >= kMinReplyTokens;
  return plan;
}

}  // namespace molt_ai
