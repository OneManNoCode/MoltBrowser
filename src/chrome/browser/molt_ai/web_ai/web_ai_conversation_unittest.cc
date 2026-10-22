// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"

#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::web_ai {
namespace {

// A turn of exactly |tokens| estimated tokens (ASCII: 3 bytes a token, plus
// the per-message overhead).
Turn TurnOf(Role role, uint32_t tokens, char fill = 'x') {
  Turn t = MakeTurn(role, std::string(3 * (tokens - kPerMessageTokens), fill));
  EXPECT_EQ(t.tokens, tokens);
  return t;
}

TEST(WebAIConversationTest, UsageCountsTheSystemPromptAndTurns) {
  Conversation c(100);
  EXPECT_EQ(c.input_usage(), 0u);
  EXPECT_TRUE(c.SetSystemPrompt(std::string(30, 's')));  // 10 + 4
  EXPECT_EQ(c.input_usage(), 14u);
  std::vector<Turn> turns;
  turns.push_back(TurnOf(Role::kUser, 20));
  const Conversation::Plan plan = c.PlanAdd(turns);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(plan.evict, 0u);
  EXPECT_EQ(plan.requested, 34u);
  c.Add(plan, std::move(turns));
  EXPECT_EQ(c.input_usage(), 34u);
  EXPECT_EQ(c.turns().size(), 1u);
}

TEST(WebAIConversationTest, SystemPromptAloneCanBeTooLarge) {
  Conversation c(10);
  EXPECT_FALSE(c.SetSystemPrompt(std::string(100, 's')));
  EXPECT_TRUE(c.system_prompt().empty());
  EXPECT_EQ(c.input_usage(), 0u);
}

TEST(WebAIConversationTest, TooLargeInputDoesNotFit) {
  Conversation c(50);
  ASSERT_TRUE(c.SetSystemPrompt(std::string(30, 's')));  // 14
  std::vector<Turn> turns;
  turns.push_back(TurnOf(Role::kUser, 40));
  const Conversation::Plan plan = c.PlanAdd(turns);
  EXPECT_FALSE(plan.fits);
  EXPECT_EQ(plan.requested, 54u);
}

TEST(WebAIConversationTest, OldestTurnsAreEvictedFirst) {
  Conversation c(100);
  for (char fill : {'a', 'b', 'c'}) {
    std::vector<Turn> turns;
    turns.push_back(TurnOf(Role::kUser, 30, fill));
    c.Add(c.PlanAdd(turns), std::move(turns));
  }
  EXPECT_EQ(c.input_usage(), 90u);
  std::vector<Turn> next;
  next.push_back(TurnOf(Role::kUser, 40, 'd'));
  const Conversation::Plan plan = c.PlanAdd(next);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(plan.evict, 1u);  // 'a' goes; 60 + 40 = 100
  // Planning changes nothing.
  EXPECT_EQ(c.turns().size(), 3u);
  const Conversation::Shape shape = c.ShapeAfter(plan, next);
  EXPECT_EQ(shape.user_message.substr(0, 1), "b");  // b, c, d merged
  EXPECT_TRUE(shape.history.empty());
  c.Add(plan, std::move(next));
  EXPECT_EQ(c.input_usage(), 100u);
  ASSERT_EQ(c.turns().size(), 3u);
  EXPECT_EQ(c.turns().front().text[0], 'b');
}

TEST(WebAIConversationTest, ReplyEvictsAndReports) {
  Conversation c(60);
  std::vector<Turn> turns;
  turns.push_back(TurnOf(Role::kUser, 30));
  c.Add(c.PlanAdd(turns), std::move(turns));
  EXPECT_FALSE(c.AddReply(std::string(3 * 16, 'r')));  // 20: fits (50)
  EXPECT_EQ(c.input_usage(), 50u);
  // 30: evicts the user turn, and with it its reply (the history must start
  // with a user turn).
  EXPECT_TRUE(c.AddReply(std::string(3 * 26, 'r')));
  EXPECT_EQ(c.input_usage(), 30u);
  ASSERT_EQ(c.turns().size(), 1u);
  EXPECT_EQ(c.turns().back().role, Role::kAssistant);
}

TEST(WebAIConversationTest, EvictionDropsWholeExchanges) {
  Conversation c(100);
  for (int i = 0; i < 2; ++i) {
    std::vector<Turn> turns;
    turns.push_back(TurnOf(Role::kUser, 30));
    c.Add(c.PlanAdd(turns), std::move(turns));
    EXPECT_FALSE(c.AddReply(std::string(3 * 6, 'r')));  // 10
  }
  EXPECT_EQ(c.input_usage(), 80u);
  std::vector<Turn> next;
  next.push_back(TurnOf(Role::kUser, 30));
  const Conversation::Plan plan = c.PlanAdd(next);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(plan.evict, 2u);  // the first user turn and its reply
  const Conversation::Shape shape = c.ShapeAfter(plan, next);
  ASSERT_EQ(shape.history.size(), 2u);
  EXPECT_EQ(shape.history[0].first, Role::kUser);
}

TEST(WebAIConversationTest, ALeadingAssistantTurnStaysUntilEviction) {
  // A page may open with the assistant's greeting.
  Conversation c(1000);
  std::vector<Turn> initial;
  initial.push_back(MakeTurn(Role::kAssistant, "How can I help?"));
  c.Add(c.PlanAdd(initial), std::move(initial));
  std::vector<Turn> next;
  next.push_back(MakeTurn(Role::kUser, "Hi"));
  const Conversation::Plan plan = c.PlanAdd(next);
  EXPECT_EQ(plan.evict, 0u);
  const Conversation::Shape shape = c.ShapeAfter(plan, next);
  ASSERT_EQ(shape.history.size(), 1u);
  EXPECT_EQ(shape.history[0].first, Role::kAssistant);
}

TEST(WebAIConversationTest, AReplyLargerThanTheQuotaIsKeptAlone) {
  Conversation c(20);
  std::vector<Turn> turns;
  turns.push_back(TurnOf(Role::kUser, 10));
  c.Add(c.PlanAdd(turns), std::move(turns));
  EXPECT_TRUE(c.AddReply(std::string(300, 'r')));
  ASSERT_EQ(c.turns().size(), 1u);
  EXPECT_EQ(c.turns().front().role, Role::kAssistant);
  // The next prompt evicts it.
  std::vector<Turn> next;
  next.push_back(TurnOf(Role::kUser, 10));
  const Conversation::Plan plan = c.PlanAdd(next);
  EXPECT_TRUE(plan.fits);
  EXPECT_EQ(plan.evict, 1u);
}

TEST(WebAIConversationTest, ShapeMergesRunsAndEndsWithTheUser) {
  Conversation c(1000);
  std::vector<Turn> initial;
  initial.push_back(MakeTurn(Role::kUser, "q1"));
  initial.push_back(MakeTurn(Role::kAssistant, "a1"));
  initial.push_back(MakeTurn(Role::kAssistant, "a1b"));
  c.Add(c.PlanAdd(initial), std::move(initial));
  std::vector<Turn> added;
  added.push_back(MakeTurn(Role::kUser, "q2"));
  added.push_back(MakeTurn(Role::kUser, "q2b"));
  const Conversation::Shape shape = c.ShapeAfter(c.PlanAdd(added), added);
  ASSERT_EQ(shape.history.size(), 2u);
  EXPECT_EQ(shape.history[0].first, Role::kUser);
  EXPECT_EQ(shape.history[0].second, "q1");
  EXPECT_EQ(shape.history[1].first, Role::kAssistant);
  EXPECT_EQ(shape.history[1].second, "a1\n\na1b");
  EXPECT_EQ(shape.user_message, "q2\n\nq2b");
}

TEST(WebAIConversationTest, CopiesAreIndependent) {
  Conversation c(1000);
  ASSERT_TRUE(c.SetSystemPrompt("be brief"));
  std::vector<Turn> turns;
  turns.push_back(MakeTurn(Role::kUser, "hello"));
  c.Add(c.PlanAdd(turns), std::move(turns));
  Conversation fork = c;
  fork.AddReply("hi");
  EXPECT_EQ(c.turns().size(), 1u);
  EXPECT_EQ(fork.turns().size(), 2u);
  EXPECT_EQ(fork.system_prompt(), "be brief");
  EXPECT_GT(fork.input_usage(), c.input_usage());
}

}  // namespace
}  // namespace molt_ai::web_ai
