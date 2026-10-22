// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"

#include <algorithm>
#include <cstdint>
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

// A user turn with one image per entry of |widths| (32 high, grey); the
// width tells the images apart.
Turn ImageTurn(const std::string& text, const std::vector<uint32_t>& widths) {
  Turn t = MakeTurn(Role::kUser, text, widths.size());
  for (uint32_t w : widths) {
    t.images.emplace_back(w, 32u, std::vector<uint8_t>(size_t{w} * 32 * 3, 9));
  }
  return t;
}

std::vector<uint32_t> Widths(const std::vector<ModelImage>& images) {
  std::vector<uint32_t> widths;
  for (const ModelImage& image : images) {
    widths.push_back(image.width);
  }
  return widths;
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

TEST(WebAIConversationTest, ImagesCountAFixedAmount) {
  EXPECT_EQ(MakeTurn(Role::kUser, "look", 2).tokens,
            EstimateMessageTokens("look") + 2 * kImageInputTokens);
  // Saturating, like every count.
  EXPECT_EQ(MakeTurn(Role::kUser, "", 10'000'000).tokens, UINT32_MAX);
  Conversation c(kLargeInputQuota);
  std::vector<Turn> turns;
  turns.push_back(ImageTurn("look", {40}));
  const Conversation::Plan plan = c.PlanAdd(turns);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(plan.requested, EstimateMessageTokens("look") + kImageInputTokens);
  c.Add(plan, std::move(turns));
  EXPECT_EQ(c.input_usage(), plan.requested);
  EXPECT_EQ(c.image_count(), 1u);
  // Two images don't fit the small quota with any text.
  Conversation small(kSmallInputQuota);
  std::vector<Turn> two;
  two.push_back(ImageTurn("x", {40, 50}));
  EXPECT_FALSE(small.PlanAdd(two).fits);
}

TEST(WebAIConversationTest, TheContextKeepsTheNewestImages) {
  Conversation c(kLargeInputQuota);
  std::vector<Turn> first;
  first.push_back(ImageTurn("a", {40, 41, 42}));
  c.Add(c.PlanAdd(first), std::move(first));
  c.AddReply("ok");
  const uint32_t usage = c.input_usage();
  std::vector<Turn> second;
  second.push_back(ImageTurn("b", {50, 51}));
  const uint32_t added = second[0].tokens;
  c.Add(c.PlanAdd(second), std::move(second));
  // Four kept: the newest turn's two, then the first turn's last two.
  EXPECT_EQ(c.image_count(), kMaxContextImages);
  ASSERT_EQ(c.turns().size(), 3u);
  EXPECT_EQ(Widths(c.turns()[0].images), (std::vector<uint32_t>{41, 42}));
  EXPECT_EQ(Widths(c.turns()[2].images), (std::vector<uint32_t>{50, 51}));
  // The one let go still counts: it was part of its turn.
  EXPECT_EQ(c.input_usage(), usage + added);
  // A turn with four drops every older image.
  std::vector<Turn> third;
  third.push_back(ImageTurn("c", {60, 61, 62, 63}));
  c.Add(c.PlanAdd(third), std::move(third));
  EXPECT_EQ(c.image_count(), kMaxContextImages);
  EXPECT_TRUE(c.turns()[0].images.empty());
  EXPECT_TRUE(c.turns()[2].images.empty());
}

TEST(WebAIConversationTest, EvictingATurnDropsItsImages) {
  Conversation c(2 * kImageInputTokens + 100);
  std::vector<Turn> first;
  first.push_back(ImageTurn("a", {40}));
  c.Add(c.PlanAdd(first), std::move(first));
  c.AddReply("ok");
  std::vector<Turn> second;
  second.push_back(ImageTurn("b", {50, 51}));
  const Conversation::Plan plan = c.PlanAdd(second);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(plan.evict, 2u);
  const Conversation::Shape shape = c.ShapeAfter(plan, second);
  EXPECT_TRUE(shape.history.empty());
  EXPECT_TRUE(shape.history_images.empty());
  EXPECT_EQ(Widths(shape.user_images), (std::vector<uint32_t>{50, 51}));
  c.Add(plan, std::move(second));
  EXPECT_EQ(c.image_count(), 2u);
}

TEST(WebAIConversationTest, ShapeSendsTheNewestImagesWithTheirTurns) {
  Conversation c(kLargeInputQuota);
  std::vector<Turn> initial;
  initial.push_back(ImageTurn("q1", {40}));
  initial.push_back(MakeTurn(Role::kAssistant, "a1"));
  initial.push_back(ImageTurn("q2", {50, 51}));
  initial.push_back(MakeTurn(Role::kAssistant, "a2"));
  c.Add(c.PlanAdd(initial), std::move(initial));
  // Two appended user turns with images, then the prompt's text.
  std::vector<Turn> added;
  added.push_back(ImageTurn("q3", {60}));
  added.push_back(MakeTurn(Role::kUser, "q3b"));
  const Conversation::Shape shape = c.ShapeAfter(c.PlanAdd(added), added);
  ASSERT_EQ(shape.history.size(), 4u);
  ASSERT_EQ(shape.history_images.size(), 4u);
  // Four in all, newest first: the new message's one, q2's two, q1's one.
  EXPECT_EQ(Widths(shape.user_images), (std::vector<uint32_t>{60}));
  EXPECT_EQ(shape.user_message, "q3\n\nq3b");
  EXPECT_EQ(Widths(shape.history_images[2]), (std::vector<uint32_t>{50, 51}));
  EXPECT_EQ(Widths(shape.history_images[0]), (std::vector<uint32_t>{40}));
  EXPECT_TRUE(shape.history_images[1].empty());
  EXPECT_TRUE(shape.history_images[3].empty());

  // With more than four, the oldest are left out; a turn keeps its last.
  std::vector<Turn> more;
  more.push_back(ImageTurn("q4", {70, 71}));
  const Conversation::Shape full = c.ShapeAfter(c.PlanAdd(more), more);
  EXPECT_EQ(Widths(full.user_images), (std::vector<uint32_t>{70, 71}));
  EXPECT_EQ(Widths(full.history_images[2]), (std::vector<uint32_t>{50, 51}));
  EXPECT_TRUE(full.history_images[0].empty());
  // Text only: nothing about images at all.
  Conversation text(kLargeInputQuota);
  std::vector<Turn> plain;
  plain.push_back(MakeTurn(Role::kUser, "hi"));
  const Conversation::Shape none = text.ShapeAfter(text.PlanAdd(plain), plain);
  EXPECT_TRUE(none.history_images.empty());
  EXPECT_TRUE(none.user_images.empty());
}

TEST(WebAIConversationTest, NeverMoreThanFourImagesInAShape) {
  // Every split of up to 12 images over three user turns (with replies).
  for (size_t a = 0; a <= 4; ++a) {
    for (size_t b = 0; b <= 4; ++b) {
      for (size_t n = 0; n <= 4; ++n) {
        Conversation c(kLargeInputQuota * 4);
        std::vector<Turn> initial;
        initial.push_back(ImageTurn("a", std::vector<uint32_t>(a, 40)));
        initial.push_back(MakeTurn(Role::kAssistant, "r"));
        initial.push_back(ImageTurn("b", std::vector<uint32_t>(b, 50)));
        initial.push_back(MakeTurn(Role::kAssistant, "r"));
        c.Add(c.PlanAdd(initial), std::move(initial));
        ASSERT_EQ(c.image_count(), std::min(a + b, kMaxContextImages));
        std::vector<Turn> added;
        added.push_back(ImageTurn("n", std::vector<uint32_t>(n, 60)));
        const Conversation::Shape s = c.ShapeAfter(c.PlanAdd(added), added);
        size_t total = s.user_images.size();
        for (const auto& images : s.history_images) {
          total += images.size();
        }
        EXPECT_EQ(total, std::min(a + b + n, kMaxContextImages))
            << a << " " << b << " " << n;
        EXPECT_EQ(s.user_images.size(), n);
      }
    }
  }
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
