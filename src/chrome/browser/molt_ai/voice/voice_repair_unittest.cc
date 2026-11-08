// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_repair.h"

#include <string>

#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

using K = VoiceCommandKind;

// The recognizer's own mistakes, from the spoken suite's runs on a real
// build: one command word heard wrong, or a scrap of "Hey Molt" left in
// front of the command.
TEST(VoiceRepairTest, RepairsWhatTheSpokenSuiteMisheard) {
  const ParseContext context;
  struct Case {
    const char* heard;
    K kind;
    const char* repaired;
  } kCases[] = {
      {"Hey, Malt, close this tip.", K::kCloseTab, "close this tab"},
      {"He mold. Joe my downloads", K::kListDownloads, "show my downloads"},
      {"Hey molt, on mute", K::kUnmute, "unmute"},
      {"Hey mold. Flay", K::kPlay, "play"},
      {"Himalde. Flay", K::kPlay, "play"},
      {"Hey, Malt, meet", K::kMute, "mute"},
      {"He more. Next video", K::kNextTrack, "Next video"},
      {"He mold. Groll to the bottom", K::kScrollToBottom,
       "scroll to the bottom"},
      // A greeting, then a scrap of the name, each with its comma.
      {"Hey, more, skip the head", K::kSkipAd, "skip the ad"},
      {"Hey, Mol, read the tile", K::kReadTitle, "read the tile"},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.heard);
    ASSERT_EQ(ParseVoiceCommand(c.heard, context).kind, K::kUnknown);
    const std::optional<RepairedCommand> r =
        RepairMisheardCommand(c.heard, context);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->command.kind, c.kind);
    EXPECT_EQ(r->text, c.repaired);
    // What was heard stays what was heard.
    EXPECT_NE(r->command.heard.find_first_not_of(' '), std::string::npos);
  }
}

TEST(VoiceRepairTest, AScrapOfTheWakePhraseBeforeACommand) {
  const ParseContext context;
  for (const char* heard :
       {"halt. Play", "Timolt, pause", "Kalt. Scroll down"}) {
    SCOPED_TRACE(heard);
    ASSERT_EQ(ParseVoiceCommand(heard, context).kind, K::kUnknown);
    const std::optional<RepairedCommand> r =
        RepairMisheardCommand(heard, context);
    ASSERT_TRUE(r);
    EXPECT_NE(r->command.kind, K::kUnknown);
  }
  // Not a scrap: a word that sounds nothing like "Molt".
  EXPECT_FALSE(RepairMisheardCommand("Broccoli. Play", context));
}

// The twelfth build's live run: "Hey Molt" heard as a scrap, then a press
// as it was said. With only the scrap dropped, the press runs (and asks
// first, as in full, when it buys or deletes).
TEST(VoiceRepairTest, APressAsSaidAfterAScrapOfTheWakePhrase) {
  const ParseContext context;
  for (const char* heard : {"Kalt, click empty the cart.",
                            "Kalt, click write a note", "halt, click country.",
                            "halt. Click place border", "Kalt, submit the form"}) {
    SCOPED_TRACE(heard);
    ASSERT_EQ(ParseVoiceCommand(heard, context).kind, K::kUnknown);
    const std::optional<RepairedCommand> r =
        RepairMisheardCommand(heard, context);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->as_said);
    EXPECT_TRUE(r->command.kind == K::kChoose || r->command.kind == K::kSubmit)
        << static_cast<int>(r->command.kind);
    EXPECT_FALSE(RepairableKind(r->command.kind));
  }
  // A guessed word never makes a press: "klick" swapped for "click".
  EXPECT_FALSE(RepairMisheardCommand("Hey mold, klick country", context));
  // Nor does a scrap that sounds nothing like the wake phrase.
  EXPECT_FALSE(RepairMisheardCommand("Broccoli, click country", context));
}

// After a scrap, a command as said that may not run on a guess stays not
// understood, and no swap makes it another one.
TEST(VoiceRepairTest, NotEveryCommandAsSaidAfterAScrap) {
  const ParseContext context;
  struct Case {
    const char* said;
    K kind;
  } kCases[] = {
      {"voice mode off", K::kVoiceModeOff},
      {"stop listening", K::kVoiceModeOff},
      {"close this window", K::kCloseWindow},
      {"unbookmark this page", K::kRemoveBookmark},
      {"forget everything you learned", K::kForgetLearning},
  };
  for (const Case& c : kCases) {
    SCOPED_TRACE(c.said);
    ASSERT_EQ(ParseVoiceCommand(c.said, context).kind, c.kind);
    EXPECT_FALSE(RepairableAsSaidKind(c.kind));
    EXPECT_FALSE(
        RepairMisheardCommand(std::string("Kalt, ") + c.said, context));
  }
}

TEST(VoiceRepairTest, TwoDifferentCommandsAndNeitherIsTaken) {
  // "fog" sounds like "back" and like "pause": not a guess to act on.
  const ParseContext context;
  EXPECT_FALSE(RepairMisheardCommand("Hey mold, fog", context));
}

TEST(VoiceRepairTest, NeverMakesACommandOutOfEverydaySpeech) {
  const ParseContext context;
  for (const char* said : {
           "no",
           "nope",
           "no thanks",
           "that's right",
           "go ahead",
           "the cat is asleep on the sofa again",
           "who won the game last night",
           "did you watch the game last night",
           "we're out of milk",
           "let me think about it for a minute",
           "old habits die hard",
           "the head of the department is here",
           "find me a cheap flight to Lisbon next Friday",
           "delete all my emails from last week",
           "buy the cheapest one",
       }) {
    SCOPED_TRACE(said);
    ASSERT_EQ(ParseVoiceCommand(said, context).kind, K::kUnknown);
    EXPECT_FALSE(RepairMisheardCommand(said, context));
  }
}

TEST(VoiceRepairTest, ASwapNeverMakesFreeText) {
  // "take hello there" could be "type hello there", but a swap that only
  // puts a verb in front of whatever was said isn't trusted.
  const ParseContext context;
  EXPECT_FALSE(RepairMisheardCommand("Hey, Molt, take hello there.", context));
  // A lone wake phrase is never made a command.
  EXPECT_FALSE(RepairMisheardCommand("Hey mold", context));
}

TEST(VoiceRepairTest, NotWhileAnAnswerIsAwaitedNorInSpanish) {
  ParseContext answering;
  answering.expecting_yes_no = true;
  EXPECT_FALSE(RepairMisheardCommand("close this tip", answering));
  ParseContext spanish;
  spanish.language = Language::kSpanish;
  EXPECT_FALSE(RepairMisheardCommand("close this tip", spanish));
}

TEST(VoiceRepairTest, OnlyWhenTheGrammarUnderstoodNothing) {
  const ParseContext context;
  // Understood as it is: nothing to repair.
  EXPECT_FALSE(RepairMisheardCommand("close this tab", context));
  EXPECT_FALSE(RepairMisheardCommand("", context));
  // Too long to be a misheard command.
  EXPECT_FALSE(RepairMisheardCommand(
      "please close this tip right now because I really do not need it any "
      "more at all",
      context));
}

TEST(VoiceRepairTest, OnlyCommandsSafeToRunOnAGuess) {
  for (K kind : {K::kCloseTab, K::kPlay, K::kScrollDown, K::kType,
                 K::kListDownloads}) {
    EXPECT_TRUE(RepairableKind(kind));
  }
  for (K kind : {K::kUnknown, K::kRefused, K::kYes, K::kNo, K::kChoose,
                 K::kSubmit, K::kCorrection, K::kRunRoutine,
                 K::kForgetLearning, K::kVoiceModeOff, K::kVoiceModeOn,
                 K::kAlwaysListening, K::kProtectedSetting, K::kCloseWindow,
                 K::kOpenDownload, K::kClearField, K::kAskAI,
                 K::kRemoveBookmark}) {
    EXPECT_FALSE(RepairableKind(kind)) << static_cast<int>(kind);
  }
  // As said after a scrap: every kind a guess may run, and presses,
  // submissions, corrections, routines run, the AI; never an answer, what
  // voice mode learned, voice mode itself, a protected setting, a download
  // opened, a window closed or a bookmark removed.
  for (K kind : {K::kCloseTab, K::kPlay, K::kChoose, K::kSubmit,
                 K::kCorrection, K::kRunRoutine, K::kAskAI}) {
    EXPECT_TRUE(RepairableAsSaidKind(kind)) << static_cast<int>(kind);
  }
  for (K kind : {K::kUnknown, K::kRefused, K::kYes, K::kNo,
                 K::kForgetLearning, K::kRememberRoutine, K::kForgetRoutine,
                 K::kForgetWord, K::kVoiceModeOff, K::kVoiceModeOn,
                 K::kAlwaysListening, K::kProtectedSetting, K::kOpenDownload,
                 K::kCloseWindow, K::kRemoveBookmark}) {
    EXPECT_FALSE(RepairableAsSaidKind(kind)) << static_cast<int>(kind);
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
