// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/runtime/stream_filters.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace {

std::string RunThink(const std::vector<std::string>& pieces,
                     bool start_inside = false) {
  ThinkTagFilter f(start_inside);
  std::string out;
  for (const std::string& p : pieces) out += f.Feed(p);
  out += f.Flush();
  return out;
}

// Feed |text| one byte at a time — the worst case for split tags.
std::string RunThinkBytewise(const std::string& text) {
  std::vector<std::string> pieces;
  for (char c : text) pieces.emplace_back(1, c);
  return RunThink(pieces);
}

TEST(ThinkTagFilterTest, PassesThroughPlainText) {
  EXPECT_EQ(RunThink({"Hello", ", ", "world"}), "Hello, world");
}

TEST(ThinkTagFilterTest, HidesThinkBlockAndTrimsFollowingWhitespace) {
  EXPECT_EQ(RunThink({"<think>", "\nThe user wants X.\n", "</think>", "\n\n",
                      "Answer: 42"}),
            "Answer: 42");
}

TEST(ThinkTagFilterTest, HidesEmptyNoThinkBlock) {
  // Qwen3 with /no_think still emits an empty block.
  EXPECT_EQ(RunThink({"<think>\n\n</think>\n\n", "Hi there"}), "Hi there");
}

TEST(ThinkTagFilterTest, TagsSplitAcrossPieces) {
  EXPECT_EQ(RunThink({"<th", "ink>reason", "ing</th", "in", "k>", "Done"}),
            "Done");
  EXPECT_EQ(RunThinkBytewise("<think>secret plan</think>\n\nVisible."),
            "Visible.");
}

TEST(ThinkTagFilterTest, PartialTagPrefixThatIsJustTextIsReleased) {
  EXPECT_EQ(RunThink({"a <", "b"}), "a <b");
  EXPECT_EQ(RunThink({"x <thi"}), "x <thi");  // released at Flush()
  ThinkTagFilter f;
  EXPECT_EQ(f.Feed("<thi"), "");  // held back: could still become <think>
  EXPECT_EQ(f.Feed("s"), "<this");
}

TEST(ThinkTagFilterTest, LaterThinkTagInAnswerIsShownVerbatim) {
  // Once the answer has started, a literal tag is content (e.g. a model
  // explaining reasoning markup), not a reasoning delimiter.
  EXPECT_EQ(RunThink({"Use ", "<think>", " tags like </think> this."}),
            "Use <think> tags like </think> this.");
}

TEST(ThinkTagFilterTest, UnterminatedThinkHidesEverything) {
  ThinkTagFilter f;
  EXPECT_EQ(f.Feed("<think>still thinking when the budget ran out"), "");
  EXPECT_TRUE(f.in_think());
  EXPECT_EQ(f.Flush(), "");
}

TEST(ThinkTagFilterTest, ForcedCloseEndsReasoning) {
  ThinkTagFilter f;
  EXPECT_EQ(f.Feed("<think>long reasoning"), "");
  EXPECT_EQ(f.Feed(ThinkTagFilter::kForceClose), "");
  EXPECT_FALSE(f.in_think());
  EXPECT_EQ(f.Feed("Short answer."), "Short answer.");
}

TEST(ThinkTagFilterTest, StartInsideForPrefilledTemplates) {
  EXPECT_EQ(RunThink({"reasoning...", "</think>\n", "Answer"},
                     /*start_inside=*/true),
            "Answer");
}

TEST(ThinkTagFilterTest, OrphanCloseTagBeforeAnswerIsDropped) {
  EXPECT_EQ(RunThink({"  </think>\n\nAnswer"}), "Answer");
}

TEST(ThinkTagFilterTest, LeadingWhitespaceBeforeThinkIsHarmless) {
  EXPECT_EQ(RunThink({"\n", "<think>x</think>", "Answer"}), "Answer");
  EXPECT_EQ(RunThink({"\n\n", "Answer"}), "Answer");
}

TEST(ThinkTagFilterTest, SawThinkTracksBlocks) {
  ThinkTagFilter f;
  f.Feed("plain");
  EXPECT_FALSE(f.saw_think());
  ThinkTagFilter g;
  g.Feed("<think>x</think>y");
  EXPECT_TRUE(g.saw_think());
}

std::string RunHarmony(const std::vector<std::string>& pieces) {
  HarmonyChannelFilter f;
  std::string out;
  for (const std::string& p : pieces) out += f.Feed(p);
  return out;
}

TEST(HarmonyChannelFilterTest, NonHarmonyModelPassesThrough) {
  EXPECT_EQ(RunHarmony({"Hello", " <|message|>", " there"}),
            "Hello <|message|> there");
  // A bare control-looking piece from a non-Harmony model is text.
  EXPECT_EQ(RunHarmony({"<|end|>", "x"}), "<|end|>x");
}

TEST(HarmonyChannelFilterTest, OnlyFinalChannelIsSurfaced) {
  EXPECT_EQ(RunHarmony({"<|channel|>", "analysis", "<|message|>",
                        "User asks for 2+2.", "<|end|>", "<|start|>",
                        "assistant", "<|channel|>", "final", "<|message|>",
                        "It is ", "4."}),
            "It is 4.");
}

TEST(HarmonyChannelFilterTest, HidingStateTracksChannels) {
  HarmonyChannelFilter f;
  EXPECT_FALSE(f.hiding());
  f.Feed("<|channel|>");
  EXPECT_TRUE(f.hiding());
  f.Feed("analysis");
  f.Feed("<|message|>");
  EXPECT_TRUE(f.hiding());
  f.Feed("<|end|>");
  f.Feed("<|start|>");
  f.Feed("assistant");
  f.Feed("<|channel|>");
  f.Feed("final");
  f.Feed("<|message|>");
  EXPECT_FALSE(f.hiding());
}

TEST(HarmonyChannelFilterTest, ForceFinalSwitchesToAnswer) {
  HarmonyChannelFilter f;
  f.Feed("<|channel|>");
  f.Feed("analysis");
  f.Feed("<|message|>");
  EXPECT_EQ(f.Feed("thinking..."), "");
  // The runtime tokenizes kForceFinal into control tokens + "assistant"
  // + "final"; emulate that piece sequence.
  for (const char* p : {"<|end|>", "<|start|>", "assistant", "<|channel|>",
                        "final", "<|message|>"}) {
    EXPECT_EQ(f.Feed(p), "");
  }
  EXPECT_EQ(f.Feed("Answer"), "Answer");
}

TEST(Utf8Test, SafePrefixHoldsBackIncompleteCharacters) {
  const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600, 4 bytes
  EXPECT_EQ(Utf8SafePrefixLength("abc"), 3u);
  EXPECT_EQ(Utf8SafePrefixLength("ab" + emoji.substr(0, 2)), 2u);
  EXPECT_EQ(Utf8SafePrefixLength("ab" + emoji), 6u);
  EXPECT_EQ(Utf8SafePrefixLength("\xC3"), 0u);       // lead of 2-byte char
  EXPECT_EQ(Utf8SafePrefixLength("\xC3\xA9"), 2u);   // é
}

TEST(Utf8Test, AssemblerJoinsSplitEmoji) {
  Utf8Assembler a;
  EXPECT_EQ(a.Push("Hi \xF0\x9F"), "Hi ");
  EXPECT_EQ(a.Push("\x98\x80!"), "\xF0\x9F\x98\x80!");
  EXPECT_EQ(a.Flush(), "");
  EXPECT_EQ(a.Push("\xE2\x82"), "");
  EXPECT_EQ(a.Flush(), "\xE2\x82");
}

}  // namespace
}  // namespace molt_ai
