// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Context-window arithmetic (engine/context_budget.h) on what a model file
// says about itself (training window, layers, heads: GGUF metadata, so a
// crafted file picks them) and on request sizes (token counts of up to
// kMaxRequestTextBytes of text, up to kMaxHistoryTurns turns).
//
// Checks: no UB (signed overflow, division by zero); the context size is a
// multiple of 256 or the model's own smaller window and its batch fits it;
// a plan keeps history from a turn start, never grows the context, the user
// turn or the reply, and when it fits, the kept prompt plus the reply fit
// the window.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/services/molt_ai/inference/engine/context_budget.h"

namespace {

// molt_inference.mojom: kMaxRequestTextBytes (32 MiB; a token is at least a
// byte) and kMaxHistoryTurns.
constexpr int kMaxTokens = 32 * 1024 * 1024;
constexpr size_t kMaxHistoryTurns = 512;

void CheckSizing(FuzzedDataProvider* provider) {
  using namespace molt_ai;
  ContextSizingInput in;
  in.total_ram_bytes = provider->ConsumeIntegral<uint64_t>();
  in.model_file_bytes = provider->ConsumeIntegral<uint64_t>();
  in.n_ctx_train = provider->ConsumeIntegral<int>();
  const int n_layer = provider->ConsumeIntegral<int>();
  const int n_embd = provider->ConsumeIntegral<int>();
  const int n_head = provider->ConsumeIntegral<int>();
  const int n_head_kv = provider->ConsumeIntegral<int>();
  in.kv_bytes_per_token =
      provider->ConsumeBool()
          ? EstimateKvBytesPerToken(n_layer, n_embd, n_head, n_head_kv)
          : provider->ConsumeIntegral<uint64_t>();
  const ContextSizing sizing = ComputeContextSizing(in);
  CHECK(sizing.n_ctx > 0 || in.n_ctx_train > 0);
  CHECK(sizing.n_ctx <= 32768);
  if (in.n_ctx_train > 0) {
    CHECK(sizing.n_ctx <= std::max(in.n_ctx_train, kMinContextTokens));
    CHECK(sizing.n_ctx >= std::min(in.n_ctx_train, kMinContextTokens));
  } else {
    CHECK(sizing.n_ctx >= kMinContextTokens && sizing.n_ctx % 256 == 0);
  }
  CHECK(sizing.n_batch >= 1 || sizing.n_ctx <= 0);
  CHECK(sizing.n_batch <= sizing.n_ctx && sizing.n_batch <= kMaxBatchTokens);
  const int reply = AdaptiveReplyTokens(sizing.n_ctx);
  const int reasoning = AdaptiveReasoningTokens(sizing.n_ctx);
  CHECK(reply >= 512 && reply <= 2048);
  CHECK(reasoning >= 256 && reasoning <= 1024);
}

void CheckBudget(FuzzedDataProvider* provider) {
  using namespace molt_ai;
  PromptBudgetInput in;
  in.n_ctx = provider->ConsumeIntegralInRange<int>(0, 1 << 20);
  in.reply_reserve = provider->ConsumeIntegralInRange<int>(0, 1 << 16);
  in.system_tokens = provider->ConsumeIntegralInRange<int>(0, kMaxTokens);
  in.user_tokens = provider->ConsumeIntegralInRange<int>(0, kMaxTokens);
  in.context_tokens = provider->ConsumeIntegralInRange<int>(0, kMaxTokens);
  in.per_message_overhead = provider->ConsumeIntegralInRange<int>(0, 64);
  in.safety_margin = provider->ConsumeIntegralInRange<int>(0, 64);
  const size_t turns = provider->ConsumeIntegralInRange<size_t>(0, kMaxHistoryTurns);
  int left = kMaxTokens;
  for (size_t i = 0; i < turns; ++i) {
    const int t = provider->ConsumeIntegralInRange<int>(0, left);
    left -= t;
    in.history_tokens.push_back(t);
  }
  const size_t flags = provider->ConsumeIntegralInRange<size_t>(0, turns);
  for (size_t i = 0; i < flags; ++i) {
    in.history_starts_turn.push_back(provider->ConsumeBool());
  }

  const PromptBudgetPlan plan = PlanPromptBudget(in);
  const size_t n = in.history_tokens.size();
  CHECK(plan.history_keep_from <= n);
  const size_t k = plan.history_keep_from;
  // Kept history starts a turn (a missing flag means "starts a turn").
  CHECK(k == 0 || k == n || k >= in.history_starts_turn.size() ||
        in.history_starts_turn[k]);
  CHECK(plan.context_token_limit == -1 ||
        (plan.context_token_limit >= 0 &&
         plan.context_token_limit <= in.context_tokens));
  CHECK(plan.user_token_limit == -1 ||
        (plan.user_token_limit >= std::min(in.user_tokens, 32) &&
         plan.user_token_limit <= in.user_tokens));
  CHECK(plan.reply_tokens >= 0 && plan.reply_tokens <= in.reply_reserve);
  CHECK(plan.fits == (plan.reply_tokens >= 16));
  if (plan.fits) {
    const int64_t ov = in.per_message_overhead;
    int64_t cost = in.system_tokens + ov;
    cost += plan.context_token_limit == -1 ? in.context_tokens
                                           : plan.context_token_limit;
    for (size_t i = k; i < n; ++i) cost += in.history_tokens[i] + ov;
    cost += (plan.user_token_limit == -1 ? in.user_tokens
                                         : plan.user_token_limit) +
            ov;
    CHECK(cost + plan.reply_tokens <=
          std::max<int64_t>(0, int64_t{in.n_ctx} - in.safety_margin))
        << "the plan doesn't fit the window";
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider provider(data, size);
  if (provider.ConsumeBool()) {
    CheckSizing(&provider);
  } else {
    CheckBudget(&provider);
  }
  return 0;
}
