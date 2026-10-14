// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/context_budget.h"

#include <cstdint>
#include <numeric>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;

// Qwen3-8B geometry: 36 layers, n_embd 4096, 32 heads, 8 KV heads.
uint64_t Qwen3KvBytes() {
  return EstimateKvBytesPerToken(36, 4096, 32, 8);
}

TEST(ContextSizingTest, KvEstimateMatchesGeometry) {
  // 2 * 36 * (128 * 8) * 2 bytes = 147456, +10%.
  EXPECT_EQ(Qwen3KvBytes(), 147456u + 14745u);
  EXPECT_EQ(EstimateKvBytesPerToken(0, 4096, 32, 8), 0u);
  EXPECT_EQ(EstimateKvBytesPerToken(36, 4096, 0, 8), 0u);
}

TEST(ContextSizingTest, RamTiers) {
  ContextSizingInput in;
  in.n_ctx_train = 131072;
  in.total_ram_bytes = 16 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 8192);
  in.total_ram_bytes = 32 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 16384);
  in.total_ram_bytes = 64 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 32768);
}

TEST(ContextSizingTest, CappedByTrainingWindow) {
  ContextSizingInput in;
  in.total_ram_bytes = 64 * kGiB;
  in.n_ctx_train = 16384;  // Phi-4
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 16384);
  in.n_ctx_train = 2048;  // TinyLlama
  ContextSizing s = ComputeContextSizing(in);
  EXPECT_EQ(s.n_ctx, 2048);
  EXPECT_EQ(s.n_batch, 2048);
}

TEST(ContextSizingTest, CappedByKvBudgetAndNeverBelowFloor) {
  ContextSizingInput in;
  in.n_ctx_train = 40960;
  in.kv_bytes_per_token = Qwen3KvBytes();
  in.model_file_bytes = 5 * kGiB;

  // 16 GB: (16 - 5 - 4) / 2 = 3.5 GB of KV -> ~22K tokens; tier caps at 8K.
  in.total_ram_bytes = 16 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 8192);

  // 12 GB: (12 - 5 - 4) / 2 = 1.5 GB -> ~9.7K tokens; tier caps at 8K.
  in.total_ram_bytes = 12 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, 8192);

  // 10 GB: 0.5 GB of KV -> ~3.2K tokens, rounded down to a multiple of 256.
  in.total_ram_bytes = 10 * kGiB;
  ContextSizing s = ComputeContextSizing(in);
  EXPECT_EQ(s.n_ctx % 256, 0);
  EXPECT_GE(s.n_ctx, kMinContextTokens);
  EXPECT_LT(s.n_ctx, 4096);

  // 8 GB: no spare RAM at all -> the old fixed floor, not zero.
  in.total_ram_bytes = 8 * kGiB;
  EXPECT_EQ(ComputeContextSizing(in).n_ctx, kMinContextTokens);
}

TEST(ContextSizingTest, BatchIsBounded) {
  ContextSizingInput in;
  in.total_ram_bytes = 64 * kGiB;
  ContextSizing s = ComputeContextSizing(in);
  EXPECT_EQ(s.n_ctx, 32768);
  EXPECT_EQ(s.n_batch, kMaxBatchTokens);
}

TEST(AdaptiveBudgetTest, ReplyAndReasoningScaleWithContext) {
  EXPECT_EQ(AdaptiveReplyTokens(2048), 512);
  EXPECT_EQ(AdaptiveReplyTokens(8192), 1024);
  EXPECT_EQ(AdaptiveReplyTokens(32768), 2048);
  EXPECT_EQ(AdaptiveReasoningTokens(2048), 256);
  EXPECT_EQ(AdaptiveReasoningTokens(32768), 1024);
}

int PlanCost(const PromptBudgetInput& in, const PromptBudgetPlan& p) {
  int cost = in.system_tokens + in.per_message_overhead;
  cost += (p.user_token_limit < 0 ? in.user_tokens : p.user_token_limit) +
          in.per_message_overhead;
  cost += p.context_token_limit < 0 ? in.context_tokens : p.context_token_limit;
  for (size_t i = p.history_keep_from; i < in.history_tokens.size(); ++i) {
    cost += in.history_tokens[i] + in.per_message_overhead;
  }
  return cost;
}

TEST(PromptBudgetTest, EverythingFits) {
  PromptBudgetInput in;
  in.n_ctx = 8192;
  in.reply_reserve = 1024;
  in.system_tokens = 300;
  in.user_tokens = 50;
  in.context_tokens = 1500;
  in.history_tokens = {100, 200, 100, 200};
  in.per_message_overhead = 5;
  PromptBudgetPlan p = PlanPromptBudget(in);
  EXPECT_TRUE(p.fits);
  EXPECT_EQ(p.history_keep_from, 0u);
  EXPECT_EQ(p.context_token_limit, -1);
  EXPECT_EQ(p.user_token_limit, -1);
  EXPECT_EQ(p.reply_tokens, 1024);
}

TEST(PromptBudgetTest, DropsOldestWholeTurnsBeforeTouchingContext) {
  PromptBudgetInput in;
  in.n_ctx = 4096;
  in.reply_reserve = 512;
  in.system_tokens = 400;
  in.user_tokens = 100;
  in.context_tokens = 1000;
  // 6 messages (3 turns) of 600 tokens: 3600 total — too much.
  in.history_tokens = {600, 600, 600, 600, 600, 600};
  in.history_starts_turn = {true, false, true, false, true, false};
  PromptBudgetPlan p = PlanPromptBudget(in);
  EXPECT_TRUE(p.fits);
  EXPECT_EQ(p.context_token_limit, -1) << "context trimmed before history";
  // Dropped in whole turns: keep_from lands on a user message.
  EXPECT_EQ(p.history_keep_from % 2, 0u);
  EXPECT_GT(p.history_keep_from, 0u);
  EXPECT_LE(PlanCost(in, p) + p.reply_tokens, in.n_ctx - in.safety_margin);
}

TEST(PromptBudgetTest, NeverStartsHistoryWithOrphanAssistantReply) {
  PromptBudgetInput in;
  in.n_ctx = 2048;
  in.reply_reserve = 256;
  in.system_tokens = 200;
  in.user_tokens = 50;
  in.history_tokens = {500, 50, 500, 50, 400};
  in.history_starts_turn = {true, false, true, false, true};
  PromptBudgetPlan p = PlanPromptBudget(in);
  ASSERT_LT(p.history_keep_from, in.history_tokens.size());
  EXPECT_TRUE(in.history_starts_turn[p.history_keep_from]);
}

TEST(PromptBudgetTest, TrimsContextOnceHistoryIsAtItsFloor) {
  PromptBudgetInput in;
  in.n_ctx = 4096;
  in.reply_reserve = 512;
  in.system_tokens = 500;
  in.user_tokens = 100;
  in.context_tokens = 6000;  // a huge PDF excerpt
  in.history_tokens = {200, 200};
  PromptBudgetPlan p = PlanPromptBudget(in);
  EXPECT_TRUE(p.fits);
  EXPECT_GE(p.context_token_limit, 0);
  EXPECT_LT(p.context_token_limit, 6000);
  // The newest exchange survives (it is under the history floor).
  EXPECT_EQ(p.history_keep_from, 0u);
  EXPECT_EQ(p.user_token_limit, -1);
  EXPECT_LE(PlanCost(in, p) + p.reply_tokens, in.n_ctx - in.safety_margin);
}

TEST(PromptBudgetTest, SystemPromptIsNeverTrimmed) {
  PromptBudgetInput in;
  in.n_ctx = 2048;
  in.reply_reserve = 512;
  in.system_tokens = 1200;
  in.user_tokens = 900;
  in.context_tokens = 800;
  in.history_tokens = {300, 300};
  PromptBudgetPlan p = PlanPromptBudget(in);
  // Context and history go first, then the user tail / reply; the plan
  // has no knob for the system prompt at all, and the result fits.
  EXPECT_EQ(p.context_token_limit, 0);
  EXPECT_EQ(p.history_keep_from, 2u);
  EXPECT_TRUE(p.fits);
  EXPECT_LE(PlanCost(in, p) + p.reply_tokens, in.n_ctx - in.safety_margin);
  EXPECT_GE(p.reply_tokens, 16);
}

TEST(PromptBudgetTest, ReportsDoesNotFitWhenSystemAloneOverflows) {
  PromptBudgetInput in;
  in.n_ctx = 2048;
  in.reply_reserve = 512;
  in.system_tokens = 2100;
  in.user_tokens = 10;
  PromptBudgetPlan p = PlanPromptBudget(in);
  EXPECT_FALSE(p.fits);
}

TEST(PromptBudgetTest, ReplyReserveCappedAtHalfTheWindow) {
  PromptBudgetInput in;
  in.n_ctx = 2048;
  in.reply_reserve = 4096;
  in.system_tokens = 100;
  in.user_tokens = 10;
  PromptBudgetPlan p = PlanPromptBudget(in);
  EXPECT_TRUE(p.fits);
  EXPECT_LE(p.reply_tokens, (2048 - in.safety_margin) / 2);
}

}  // namespace
}  // namespace molt_ai
