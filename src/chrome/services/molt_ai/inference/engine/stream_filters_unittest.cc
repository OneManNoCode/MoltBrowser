// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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

// ---- GemmaThoughtFilter ---------------------------------------------------

std::string RunGemma(const std::vector<std::string>& pieces,
                     bool start_inside = false) {
  GemmaThoughtFilter f(start_inside);
  std::string out;
  for (const std::string& p : pieces) out += f.Feed(p);
  out += f.Flush();
  return out;
}

// Calls |check| with |text| cut into pieces every way that matters for a
// marker split across tokens: every possible split when |text| is short,
// otherwise every split into up to four pieces; and one byte per piece.
void ForEachSplit(
    const std::string& text,
    const std::function<void(const std::vector<std::string>&)>& check) {
  const size_t n = text.size();
  auto cut = [&](const std::vector<size_t>& at) {
    std::vector<std::string> pieces;
    size_t from = 0;
    for (size_t c : at) {
      pieces.push_back(text.substr(from, c - from));
      from = c;
    }
    pieces.push_back(text.substr(from));
    check(pieces);
  };
  if (n <= 16) {
    for (uint32_t mask = 0; mask < (1u << (n > 0 ? n - 1 : 0)); ++mask) {
      std::vector<size_t> at;
      for (size_t i = 1; i < n; ++i) {
        if (mask & (1u << (i - 1))) at.push_back(i);
      }
      cut(at);
    }
    return;
  }
  cut({});
  for (size_t a = 1; a < n; ++a) {
    cut({a});
    for (size_t b = a + 1; b < n; ++b) {
      cut({a, b});
      for (size_t c = b + 1; c < n; ++c) cut({a, b, c});
    }
  }
  std::vector<size_t> bytewise;
  for (size_t i = 1; i < n; ++i) bytewise.push_back(i);
  cut(bytewise);
}

// The visible output of |text| doesn't depend on how it was chunked.
void ExpectGemmaForEverySplit(const std::string& text,
                              const std::string& expected) {
  size_t splits = 0;
  ForEachSplit(text, [&](const std::vector<std::string>& pieces) {
    ++splits;
    std::string out = RunGemma(pieces);
    if (out != expected) {
      std::string shown;
      for (const std::string& p : pieces) shown += "[" + p + "]";
      ADD_FAILURE() << "pieces " << shown << " gave \"" << out
                    << "\", want \"" << expected << "\"";
    }
  });
  EXPECT_GT(splits, 0u);
}

TEST(GemmaThoughtFilterTest, PassesThroughPlainText) {
  EXPECT_EQ(RunGemma({"Hello", ", ", "world"}), "Hello, world");
  // What Gemma 4 writes with thinking off: the prompt ends with an empty
  // "<|channel>thought\n<channel|>" block and the answer follows.
  ExpectGemmaForEverySplit("Hi there", "Hi there");
}

TEST(GemmaThoughtFilterTest, HidesThoughtBlockAsGemmaWritesIt) {
  // Token by token, as llama_token_to_piece() renders a Gemma 4 stream:
  // the markers are single USER_DEFINED pieces.
  EXPECT_EQ(RunGemma({"<|channel>", "thought", "\n", "The", " user", " wants",
                      " X", ".", "\n", "<channel|>", "\n\n", "Answer", ": 42"}),
            "Answer: 42");
  ExpectGemmaForEverySplit(
      "<|channel>thought\nThe user wants X.\n<channel|>\n\nAnswer: 42",
      "Answer: 42");
}

TEST(GemmaThoughtFilterTest, MarkersSplitAcrossPieces) {
  EXPECT_EQ(RunGemma({"<|chan", "nel>secret", " plan<chan", "nel|", ">Done"}),
            "Done");
  // A marker cut at every possible point, alone and around text.
  ExpectGemmaForEverySplit("<|channel>abc", "");
  ExpectGemmaForEverySplit("x<channel|>yz", "xyz");
  ExpectGemmaForEverySplit("<|channel>a<channel|>b", "b");
  ExpectGemmaForEverySplit("Hi <|channel>t\nsecret<channel|> there",
                           "Hi  there");
}

TEST(GemmaThoughtFilterTest, ThoughtBetweenAnswerParts) {
  // Interleaved thinking: whitespace around the block is kept, like the
  // template's strip_thinking().
  ExpectGemmaForEverySplit(
      "Step one.<|channel>thought\nnow step two<channel|> Step two.",
      "Step one. Step two.");
}

TEST(GemmaThoughtFilterTest, UnterminatedBlockHidesTheRest) {
  GemmaThoughtFilter f;
  EXPECT_EQ(f.Feed("<|channel>thought\nstill thinking when the budget"), "");
  EXPECT_TRUE(f.hiding());
  EXPECT_EQ(f.Feed(" ran out <chan"), "");
  EXPECT_EQ(f.Flush(), "");
  ExpectGemmaForEverySplit("Answer.<|channel>thought\nmore", "Answer.");
}

TEST(GemmaThoughtFilterTest, NestedBlocksStayHidden) {
  // Reasoning that quotes the markers in pairs doesn't end the block.
  ExpectGemmaForEverySplit(
      "<|channel>thought\nGemma uses <|channel>x<channel|> blocks."
      "<channel|>Answer",
      "Answer");
  ExpectGemmaForEverySplit("<|channel><|channel><channel|>b<channel|>c", "c");
}

TEST(GemmaThoughtFilterTest, ExtraOpenMarkerHidesUntilForceClose) {
  GemmaThoughtFilter f;
  EXPECT_EQ(f.Feed("<|channel>thought\nI'll write <|channel> here."
                   "<channel|>The answer"),
            "");
  EXPECT_TRUE(f.hiding());  // fail closed: still one level deep
  // The runtime's reasoning budget runs out: it feeds kForceClose to the
  // model and closes every level.
  EXPECT_EQ(f.Feed(GemmaThoughtFilter::kForceClose), "");
  f.ForceClose();
  EXPECT_FALSE(f.hiding());
  EXPECT_EQ(f.Feed("\n\nShort answer."), "Short answer.");
  EXPECT_EQ(f.Flush(), "");
}

TEST(GemmaThoughtFilterTest, ForceCloseMarkerEndsASingleBlock) {
  GemmaThoughtFilter f;
  EXPECT_EQ(f.Feed("<|channel>thought\nlong reasoning <|chan"), "");
  // The held-back "<|chan" doesn't swallow the forced close.
  EXPECT_EQ(f.Feed(GemmaThoughtFilter::kForceClose), "");
  EXPECT_FALSE(f.hiding());
  EXPECT_EQ(f.Feed("Answer"), "Answer");
  f.ForceClose();  // no-op outside a block
  EXPECT_EQ(f.Feed(" more"), " more");
}

TEST(GemmaThoughtFilterTest, OrphanCloseMarkerIsDropped) {
  ExpectGemmaForEverySplit("<channel|>\nAnswer", "Answer");
  ExpectGemmaForEverySplit("Answer <channel|>more", "Answer more");
}

TEST(GemmaThoughtFilterTest, LookAlikesAreText) {
  // Harmony's channel token (gpt-oss), a bare tag, a marker prefix at the
  // end of the stream, a near miss.
  ExpectGemmaForEverySplit("<|channel|>x", "<|channel|>x");
  ExpectGemmaForEverySplit("a <channel> b", "a <channel> b");
  ExpectGemmaForEverySplit("tail <|chan", "tail <|chan");
  ExpectGemmaForEverySplit("<|channelX>y", "<|channelX>y");
  ExpectGemmaForEverySplit("<channel||>z", "<channel||>z");
  GemmaThoughtFilter f;
  EXPECT_EQ(f.Feed("a <|chan"), "a ");  // held: could still be a marker
  EXPECT_EQ(f.Feed("ge"), "<|change");
}

TEST(GemmaThoughtFilterTest, StartInsideWhenThePromptOpenedTheBlock) {
  EXPECT_EQ(RunGemma({"reasoning...", "\n", "<channel|>", "\n\n", "Answer"},
                     /*start_inside=*/true),
            "Answer");
  EXPECT_EQ(RunGemma({"never closed"}, /*start_inside=*/true), "");
}

TEST(GemmaThoughtFilterTest, EndsInsideThought) {
  // llama.cpp's built-in gemma4 generation prompt (thinking off).
  EXPECT_FALSE(GemmaThoughtFilter::EndsInsideThought(
      "<|turn>model\n<|channel>thought\n<channel|>"));
  EXPECT_TRUE(GemmaThoughtFilter::EndsInsideThought(
      "<|turn>model\n<|channel>thought\n"));
  EXPECT_FALSE(GemmaThoughtFilter::EndsInsideThought(""));
  EXPECT_FALSE(GemmaThoughtFilter::EndsInsideThought("<|channel|>analysis"));
}

TEST(GemmaThoughtFilterTest, LongHiddenBlockStreamsInConstantMemory) {
  // 1 MB of reasoning, one byte at a time, must not be buffered.
  GemmaThoughtFilter f;
  EXPECT_EQ(f.Feed("<|channel>thought\n"), "");
  std::string out;
  const std::string chunk = "reasoning <chan ";
  for (int i = 0; i < 65536; ++i) {
    for (char c : chunk) out += f.Feed(std::string_view(&c, 1));
  }
  EXPECT_EQ(out, "");
  EXPECT_EQ(f.Feed("<channel|>ok"), "ok");
}

TEST(StreamPipelineTest, GemmaStreamThroughAllFilters) {
  // token piece -> Harmony -> Gemma -> Think -> UTF-8, as in the runtime.
  HarmonyChannelFilter harmony;
  GemmaThoughtFilter gemma;
  ThinkTagFilter think;
  Utf8Assembler utf8;
  std::string out;
  for (const char* p :
       {"<|channel>", "thought", "\n", "pens\xC3", "\xA9" "e", "\n",
        "<channel|>", "\n\n", "R\xC3\xA9", "ponse ", "\xF0\x9F", "\x98\x80"}) {
    out += utf8.Push(think.Feed(gemma.Feed(harmony.Feed(p))));
  }
  std::string rest = think.Feed(gemma.Flush());
  rest += think.Flush();
  out += utf8.Push(rest);
  out += utf8.Flush();
  EXPECT_EQ(out, "R\xC3\xA9ponse \xF0\x9F\x98\x80");
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

// ---- StopSequenceFilter ---------------------------------------------------

std::string RunStop(const std::vector<std::string>& stops,
                    const std::vector<std::string>& pieces,
                    bool* stopped = nullptr) {
  StopSequenceFilter f(stops);
  std::string out;
  for (const std::string& p : pieces) out += f.Feed(p);
  out += f.Flush();
  if (stopped) *stopped = f.stopped();
  return out;
}

// Every way of splitting |text| into two or three pieces gives the same
// output as feeding it whole (and as feeding it byte by byte).
void ExpectSplitInvariant(const std::vector<std::string>& stops,
                          const std::string& text) {
  bool whole_stopped = false;
  const std::string whole = RunStop(stops, {text}, &whole_stopped);
  std::vector<std::string> bytes;
  for (char c : text) bytes.emplace_back(1, c);
  bool stopped = false;
  EXPECT_EQ(RunStop(stops, bytes, &stopped), whole) << text;
  EXPECT_EQ(stopped, whole_stopped) << text;
  for (size_t i = 0; i <= text.size(); ++i) {
    for (size_t j = i; j <= text.size(); ++j) {
      EXPECT_EQ(RunStop(stops, {text.substr(0, i), text.substr(i, j - i),
                                text.substr(j)},
                        &stopped),
                whole)
          << text << " split at " << i << "," << j;
      EXPECT_EQ(stopped, whole_stopped);
    }
  }
}

TEST(StopSequenceFilterTest, NoStopsPassesEverythingThrough) {
  StopSequenceFilter f;
  EXPECT_TRUE(f.empty());
  EXPECT_EQ(f.Feed("Hello"), "Hello");
  EXPECT_EQ(f.Feed(""), "");
  EXPECT_EQ(f.Flush(), "");
  EXPECT_FALSE(f.stopped());
  // Empty stop strings are ignored.
  StopSequenceFilter g({"", ""});
  EXPECT_TRUE(g.empty());
  EXPECT_EQ(g.Feed("abc"), "abc");
}

TEST(StopSequenceFilterTest, StopTextAndEverythingAfterAreDropped) {
  bool stopped = false;
  EXPECT_EQ(RunStop({"\nUser:"}, {"Answer.\nUser: more"}, &stopped),
            "Answer.");
  EXPECT_TRUE(stopped);
  EXPECT_EQ(RunStop({"STOP"}, {"STOP right away"}, &stopped), "");
  EXPECT_TRUE(stopped);
  EXPECT_EQ(RunStop({"STOP"}, {"no stop here"}, &stopped), "no stop here");
  EXPECT_FALSE(stopped);
}

TEST(StopSequenceFilterTest, NothingIsReleasedAfterAStop) {
  StopSequenceFilter f({"END"});
  EXPECT_EQ(f.Feed("abc E"), "abc ");
  EXPECT_EQ(f.Feed("ND tail"), "");
  EXPECT_TRUE(f.stopped());
  EXPECT_EQ(f.Feed("more"), "");
  EXPECT_EQ(f.Flush(), "");
}

TEST(StopSequenceFilterTest, HoldsBackOnlyAPossibleStopPrefix) {
  StopSequenceFilter f({"</answer>"});
  EXPECT_EQ(f.Feed("x </ans"), "x ");
  EXPECT_EQ(f.held_back(), 5u);
  EXPECT_EQ(f.Feed("wer is"), "</answer is");  // not a stop after all
  EXPECT_EQ(f.held_back(), 0u);
  EXPECT_EQ(f.Feed(" <"), " ");
  EXPECT_EQ(f.Flush(), "<");  // end of stream releases the prefix
  EXPECT_FALSE(f.stopped());
}

TEST(StopSequenceFilterTest, SplitAcrossPiecesAnyWay) {
  ExpectSplitInvariant({"\n\nUser:"}, "Hi.\n\nUse it.\n\nUser: x");
  ExpectSplitInvariant({"abab"}, "abaabababab");
  ExpectSplitInvariant({"aab"}, "aaaaaab!");
  ExpectSplitInvariant({"xyz", "yq"}, "xxyyqz");
  ExpectSplitInvariant({"<|im_end|>"}, "<|im_<|im_end|>");
  ExpectSplitInvariant({"stop"}, "no match at all");
}

TEST(StopSequenceFilterTest, OverlappingPrefixesFoundByKmp) {
  // A naive matcher that restarts after a mismatch at the next byte would
  // miss these.
  EXPECT_EQ(RunStop({"aab"}, {"aaab"}), "a");
  EXPECT_EQ(RunStop({"abab"}, {"ababab"}), "");
  EXPECT_EQ(RunStop({"abac"}, {"xabababac!"}), "xabab");
}

TEST(StopSequenceFilterTest, FirstStopToCompleteWins) {
  EXPECT_EQ(RunStop({"abc", "b"}, {"xabc"}), "xa");
  EXPECT_EQ(RunStop({"bc", "abcd"}, {"abcd"}), "a");
  EXPECT_EQ(RunStop({"END", "STOP"}, {"1 STOP 2 END"}), "1 ");
}

TEST(StopSequenceFilterTest, Utf8StopsNeverSplitACharacter) {
  // "é" is C3 A9; the stop starts with the same lead byte as a different
  // character in the text.
  bool stopped = false;
  EXPECT_EQ(RunStop({"\xC3\xA8nd"}, {"caf\xC3\xA9", " \xC3\xA8", "nd"},
                    &stopped),
            "caf\xC3\xA9 ");
  EXPECT_TRUE(stopped);
  StopSequenceFilter f({"\xE2\x80\x94STOP"});  // em dash + STOP
  const std::string a = f.Feed("x\xE2\x80\x94");
  EXPECT_EQ(a, "x");
  EXPECT_TRUE(IsValidUtf8(a));
  const std::string b = f.Feed("\xE2\x80\x94y");
  EXPECT_EQ(b, "\xE2\x80\x94\xE2\x80\x94y");
  EXPECT_TRUE(IsValidUtf8(b));
}

TEST(StopSequenceFilterTest, LinearTimeOnAdversarialInput) {
  // 2 MiB of 'a' against stops that almost match everywhere: KMP keeps this
  // at O(n * stops); a naive rescan would be O(n * stops * 64).
  std::vector<std::string> stops;
  for (int i = 0; i < 8; ++i) stops.push_back(std::string(63, 'a') + "b");
  StopSequenceFilter f(stops);
  const std::string chunk(4096, 'a');
  size_t out = 0;
  for (int i = 0; i < 512; ++i) {
    out += f.Feed(chunk).size();
    EXPECT_LE(f.held_back(), 63u);
  }
  out += f.Flush().size();
  EXPECT_EQ(out, 512u * 4096u);
  EXPECT_FALSE(f.stopped());
}

TEST(StreamPipelineTest, StopSequenceAppliesAfterReasoningFilters) {
  // A stop string inside the hidden reasoning doesn't end the answer.
  ThinkTagFilter think;
  StopSequenceFilter stop({"DONE"});
  std::string out;
  for (const char* p : {"<think>", "I am DONE thinking", "</think>",
                        "\n\nThe answer", " is 4. DO", "NE and more"}) {
    out += stop.Feed(think.Feed(p));
  }
  out += stop.Feed(think.Flush());
  out += stop.Flush();
  EXPECT_EQ(out, "The answer is 4. ");
  EXPECT_TRUE(stop.stopped());
}

// ---- UTF-8 validation -----------------------------------------------------

TEST(Utf8Test, IsValidUtf8AcceptsWellFormedText) {
  EXPECT_TRUE(IsValidUtf8(""));
  EXPECT_TRUE(IsValidUtf8("plain ascii"));
  EXPECT_TRUE(IsValidUtf8(std::string("nul\0inside", 10)));
  EXPECT_TRUE(IsValidUtf8("caf\xC3\xA9"));                // U+00E9
  EXPECT_TRUE(IsValidUtf8("\xE2\x82\xAC"));               // U+20AC
  EXPECT_TRUE(IsValidUtf8("\xED\x9F\xBF"));               // U+D7FF
  EXPECT_TRUE(IsValidUtf8("\xEE\x80\x80"));               // U+E000
  EXPECT_TRUE(IsValidUtf8("\xEF\xBF\xBD"));               // U+FFFD
  EXPECT_TRUE(IsValidUtf8("\xF0\x9F\x98\x80"));           // U+1F600
  EXPECT_TRUE(IsValidUtf8("\xF4\x8F\xBF\xBF"));           // U+10FFFF
}

TEST(Utf8Test, IsValidUtf8RejectsIllFormedText) {
  EXPECT_FALSE(IsValidUtf8("\x80"));                  // lone continuation
  EXPECT_FALSE(IsValidUtf8("\xC3"));                  // truncated
  EXPECT_FALSE(IsValidUtf8("ab\xE2\x82"));            // truncated at end
  EXPECT_FALSE(IsValidUtf8("\xC0\xAF"));              // overlong '/'
  EXPECT_FALSE(IsValidUtf8("\xC1\xBF"));              // overlong
  EXPECT_FALSE(IsValidUtf8("\xE0\x80\xAF"));          // overlong 3-byte
  EXPECT_FALSE(IsValidUtf8("\xF0\x80\x80\xAF"));      // overlong 4-byte
  EXPECT_FALSE(IsValidUtf8("\xED\xA0\x80"));          // U+D800 surrogate
  EXPECT_FALSE(IsValidUtf8("\xED\xBF\xBF"));          // U+DFFF surrogate
  EXPECT_FALSE(IsValidUtf8("\xF4\x90\x80\x80"));      // U+110000
  EXPECT_FALSE(IsValidUtf8("\xF5\x80\x80\x80"));      // invalid lead
  EXPECT_FALSE(IsValidUtf8("\xFF"));
  EXPECT_FALSE(IsValidUtf8("\xC3\x28"));              // bad continuation
  EXPECT_FALSE(IsValidUtf8("\xE2\x28\xA1"));
}

TEST(Utf8Test, ReplaceInvalidUtf8UsesOneReplacementPerMaximalSubpart) {
  const std::string fffd = "\xEF\xBF\xBD";
  EXPECT_EQ(ReplaceInvalidUtf8("valid \xC3\xA9"), "valid \xC3\xA9");
  EXPECT_EQ(ReplaceInvalidUtf8("a\x80" "b"), "a" + fffd + "b");
  EXPECT_EQ(ReplaceInvalidUtf8("\xE2\x82"), fffd);          // one subpart
  EXPECT_EQ(ReplaceInvalidUtf8("\xE2\x82" "A"), fffd + "A");
  EXPECT_EQ(ReplaceInvalidUtf8("\xC0\xAF"), fffd + fffd);   // C0 never leads
  EXPECT_EQ(ReplaceInvalidUtf8("\xED\xA0\x80"), fffd + fffd + fffd);
  EXPECT_EQ(ReplaceInvalidUtf8("\xF0\x9F\x98"), fffd);
  EXPECT_EQ(ReplaceInvalidUtf8("\xF0\x9F\x98\xF0\x9F\x98\x80"),
            fffd + "\xF0\x9F\x98\x80");
  // The Unicode Standard's example (section 3.9, U+FFFD substitution).
  EXPECT_EQ(ReplaceInvalidUtf8("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80"
                               "\xBF\x64"),
            "a" + fffd + fffd + fffd + "b" + fffd + "c" + fffd + fffd + "d");
}

TEST(Utf8Test, ReplaceInvalidUtf8OutputIsAlwaysValid) {
  // Every 1- and 2-byte string, and a sample of 3- and 4-byte ones.
  for (int a = 0; a < 256; ++a) {
    const std::string one(1, static_cast<char>(a));
    EXPECT_TRUE(IsValidUtf8(ReplaceInvalidUtf8(one)));
    for (int b = 0; b < 256; ++b) {
      const std::string two = one + static_cast<char>(b);
      const std::string fixed = ReplaceInvalidUtf8(two);
      ASSERT_TRUE(IsValidUtf8(fixed)) << a << " " << b;
      if (IsValidUtf8(two)) {
        EXPECT_EQ(fixed, two);
      }
    }
  }
  uint32_t x = 12345;
  for (int i = 0; i < 200000; ++i) {
    std::string s;
    for (int j = 0; j < 4; ++j) {
      x = x * 1103515245u + 12345u;
      s.push_back(static_cast<char>(0x80 | ((x >> 16) & 0x7F) |
                                    ((x >> 24) & 0x80 ? 0x40 : 0)));
    }
    const std::string fixed = ReplaceInvalidUtf8(s);
    ASSERT_TRUE(IsValidUtf8(fixed));
    if (IsValidUtf8(s)) {
      EXPECT_EQ(fixed, s);
    }
  }
}

TEST(Utf8Test, DisplayTextHasNoNulAndIsValid) {
  const std::string fffd = "\xEF\xBF\xBD";
  EXPECT_EQ(ReplaceNul("plain"), "plain");
  EXPECT_EQ(ReplaceNul(std::string("a\0b\0", 4)), "a" + fffd + "b" + fffd);
  EXPECT_EQ(ToDisplayText(std::string("\0\xC3\xA9\xFF", 4)),
            fffd + "\xC3\xA9" + fffd);
  EXPECT_EQ(ToDisplayText("caf\xC3\xA9"), "caf\xC3\xA9");
}

TEST(StreamPipelineTest, ServiceAndBrowserStopFiltersAgree) {
  // The service runs the stop filter on display text; the browser runs its
  // copy on the chunks it receives (already display text). Same text, same
  // cut, however the service chunked it.
  const std::vector<std::string> stops = {"\xEF\xBF\xBD\xEF\xBF\xBD",
                                          "END"};
  const std::vector<std::string> pieces = {"ok ", "\xFF", "\xFE", " tail END"};
  Utf8Assembler utf8;
  StopSequenceFilter service(stops);
  std::vector<std::string> sent;
  for (const std::string& p : pieces) {
    std::string chunk = service.Feed(ToDisplayText(utf8.Push(p)));
    if (!chunk.empty()) sent.push_back(chunk);
  }
  std::string last = service.Feed(ToDisplayText(utf8.Flush()));
  last += service.Flush();
  if (!last.empty()) sent.push_back(last);
  EXPECT_TRUE(service.stopped());
  StopSequenceFilter browser(stops);
  std::string shown;
  for (const std::string& c : sent) {
    ASSERT_TRUE(IsValidUtf8(c));
    shown += browser.Feed(ReplaceNul(c));
  }
  shown += browser.Flush();
  EXPECT_EQ(shown, "ok ");
  EXPECT_FALSE(browser.stopped());  // the service already cut it
}

TEST(StreamPipelineTest, ByteFallbackGarbageLeavesTheServiceAsValidUtf8) {
  // Pieces as a byte-fallback vocab can produce them; what the service sends
  // (Utf8Assembler output, then ReplaceInvalidUtf8) is always valid.
  Utf8Assembler utf8;
  std::string sent;
  for (const char* p : {"ok ", "\xED", "\xA0\x80", " \xC3", "(", "\xF0\x9F",
                        "\x98\x80", "\xFF", "end \xE2\x82"}) {
    const std::string chunk = ReplaceInvalidUtf8(utf8.Push(p));
    EXPECT_TRUE(IsValidUtf8(chunk));
    sent += chunk;
  }
  sent += ReplaceInvalidUtf8(utf8.Flush());
  EXPECT_TRUE(IsValidUtf8(sent));
  EXPECT_NE(sent.find("\xF0\x9F\x98\x80"), std::string::npos);
}

}  // namespace
}  // namespace molt_ai
