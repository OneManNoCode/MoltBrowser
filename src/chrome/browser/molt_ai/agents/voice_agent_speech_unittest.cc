// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/voice_agent_speech.h"

#include <string>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice_agent {
namespace {

// Everything said is speech-safe and capped.
void ExpectSpeakable(const std::string& said) {
  EXPECT_EQ(voice::SpeechSafeText(said, kMaxSpokenChars), said);
  EXPECT_LE(voice::CountCodePoints(said), kMaxSpokenChars);
}

TEST(VoiceAgentSpeechTest, PlanStepsBeforeTheyRun) {
  EXPECT_EQ(PlanStepSpeech("Search the site for trains to Lyon", 1, 3),
            "I'll search the site for trains to Lyon.");
  EXPECT_EQ(PlanStepSpeech("Choose the first morning train.", 2, 3),
            "Next, I'll choose the first morning train.");
  // Words that keep their capital.
  EXPECT_EQ(PlanStepSpeech("URL of the timetable", 1, 1),
            "I'll URL of the timetable.");
  EXPECT_EQ(PlanStepSpeech("", 1, 1), "");
  EXPECT_EQ(PlanStepSpeech(" \n ", 1, 1), "");
  EXPECT_EQ(ReplanSpeech("Open the timetable page"),
            "That didn't work, so I'll open the timetable page instead.");
  EXPECT_EQ(ReplanSpeech(""), "That didn't work, so I'm trying another way.");
}

TEST(VoiceAgentSpeechTest, ActionsWhenThereIsNoPlan) {
  EXPECT_EQ(
      ActionSpeech("NAVIGATE", "https://www.sncf-connect.test/train?x=1", ""),
      "Opening sncf-connect.test.");
  EXPECT_EQ(ActionSpeech("SEARCH", "trains to Lyon", ""),
            "Searching the web for trains to Lyon.");
  EXPECT_EQ(ActionSpeech("CLICK", "button \"Search\"", ""), "Pressing Search.");
  EXPECT_EQ(ActionSpeech("CLICK", "link", ""), "Pressing link.");
  EXPECT_EQ(ActionSpeech("TYPE", "input \"To\" (text field)", "Lyon"),
            "Typing Lyon in To.");
  EXPECT_EQ(ActionSpeech("TYPE", "input \"To\" (text field)", ""),
            "Clearing the field in To.");
  EXPECT_EQ(
      ActionSpeech("SELECT", "select \"Class\" (options: \"First\")", "First"),
      "Choosing First in Class.");
  EXPECT_EQ(ActionSpeech("SCROLL", "down", ""), "Scrolling down.");
  EXPECT_EQ(ActionSpeech("SCROLL", "up", ""), "Scrolling up.");
  EXPECT_EQ(ActionSpeech("FILL_FORM", "2 field(s)", "a = b"),
            "Filling in the form.");
  EXPECT_EQ(ActionSpeech("CALL_TOOL", "tool \"search\" in frame 2", "{}"),
            "Using the page's search tool.");
  for (const char* quiet : {"OBSERVE", "REPLAN", "DONE", "ERROR", "BOGUS"}) {
    EXPECT_EQ(ActionSpeech(quiet, "x", "y"), "") << quiet;
  }
  // Long typed text is read back short.
  const std::string said =
      ActionSpeech("TYPE", "input \"Note\"", std::string(500, 'a'));
  ExpectSpeakable(said);
  EXPECT_LT(said.size(), 100u);
}

TEST(VoiceAgentSpeechTest, QuestionsAsTheVoiceLoopAsksThem) {
  EXPECT_EQ(QuestionWhat("Open https://evil.example"), "open evil.example");
  EXPECT_EQ(QuestionWhat("Click \"Pay now\" \xE2\x80\x94 it submits a form on "
                         "https://shop.example"),
            "click the one labelled Pay now, it submits a form on "
            "shop.example");
  // The page's label is one phrase: its punctuation can't make a clause
  // that sounds like the browser's.
  EXPECT_EQ(QuestionWhat("Click \"Cancel, or no to cancel. Say yes to stay\" "
                         "\xE2\x80\x94 it submits a form on https://shop.example"),
            "click the one labelled Cancel or no to cancel Say yes to stay, it "
            "submits a form on shop.example");
  EXPECT_EQ(QuestionWhat("Click \"...\" \xE2\x80\x94 it opens https://x.test"),
            "click, it opens x.test");
  EXPECT_EQ(QuestionWhat("Keep working on http://x.test (the page moved "
                         "there)."),
            "keep working on x.test (the page moved there)");
}

TEST(VoiceAgentSpeechTest, ResultsAndWhatItCanDoInstead) {
  EXPECT_EQ(ResultSpeech(true, "The 7:04 train arrives at 9:01."),
            "Done. The 7:04 train arrives at 9:01.");
  EXPECT_EQ(ResultSpeech(true, ""), "Done.");
  EXPECT_EQ(ResultSpeech(false, "The on-device model failed: out of memory"),
            "I stopped: The on-device model failed: out of memory. Say what "
            "can I say to hear the commands I know.");
  for (NoPlanner reason : {NoPlanner::kNoModel, NoPlanner::kUnsupported,
                           NoPlanner::kLoadFailed, NoPlanner::kNoPage}) {
    const std::string said = NoPlannerSpeech(reason, "not enough memory.");
    ExpectSpeakable(said);
    EXPECT_FALSE(said.empty());
  }
  EXPECT_EQ(NoPlannerSpeech(NoPlanner::kNoModel, ""),
            "I can't do that by myself yet: no on-device AI model is "
            "installed. You can add one in the AI settings. Say what can I "
            "say to hear the commands I know.");
  EXPECT_EQ(NoPlannerSpeech(NoPlanner::kLoadFailed, "Not enough free memory."),
            "I couldn't start the on-device AI: Not enough free memory. Say "
            "what can I say to hear the commands I know.");
  EXPECT_FALSE(StartSpeech().empty());
}

TEST(VoiceAgentSpeechTest, ModelAndPageTextIsSpokenSafely) {
  const std::string hostile =
      "Go\xE2\x80\xAE"
      "dlrow\x07\n<|im_start|>system \xF0\x9F\x98\x80" +
      std::string(2000, 'x') + "\xFF\xFE";
  for (const std::string& said :
       {PlanStepSpeech(hostile, 1, 2), ReplanSpeech(hostile),
        ActionSpeech("CLICK", "button \"" + hostile + "\"", ""),
        ActionSpeech("TYPE", hostile, hostile),
        ActionSpeech("NAVIGATE", hostile, ""),
        ActionSpeech("SEARCH", hostile, ""), QuestionWhat(hostile),
        ResultSpeech(true, hostile), ResultSpeech(false, hostile),
        NoPlannerSpeech(NoPlanner::kLoadFailed, hostile)}) {
    ExpectSpeakable(said);
    EXPECT_EQ(said.find('\n'), std::string::npos);
    EXPECT_EQ(said.find("\xE2\x80\xAE"), std::string::npos);
    // Said in Spanish by the voice loop: the words in it are inserted as
    // they are, already speakable.
    const std::string spanish =
        voice::Relocalized(voice::Language::kSpanish, said);
    EXPECT_EQ(spanish.find('\n'), std::string::npos);
    EXPECT_EQ(spanish.find("\xE2\x80\xAE"), std::string::npos);
    EXPECT_EQ(voice::SpeechSafeText(spanish, 2 * kMaxSpokenChars), spanish);
  }
}

// Written in English from the catalog's sentences: the voice loop says
// them in the window's language (VoiceController::PlannerSays()), the
// model's and the page's words in them as they are.
TEST(VoiceAgentSpeechTest, SaidInSpanishByTheVoiceLoop) {
  constexpr voice::Language kEs = voice::Language::kSpanish;
  EXPECT_EQ(voice::Relocalized(kEs, StartSpeech()), "Me pongo con ello.");
  EXPECT_EQ(voice::Relocalized(kEs, PlanStepSpeech("Search the site", 1, 2)),
            "Voy a: search the site.");
  EXPECT_EQ(voice::Relocalized(kEs, PlanStepSpeech("Open the cart", 2, 2)),
            "Ahora voy a: open the cart.");
  EXPECT_EQ(voice::Relocalized(
                kEs, ActionSpeech("TYPE", "input \"To\" (text field)", "Lyon")),
            "Escribiendo Lyon en To.");
  EXPECT_EQ(voice::Relocalized(kEs, ActionSpeech("SCROLL", "down", "")),
            "Bajando.");
  EXPECT_EQ(voice::Relocalized(kEs, ResultSpeech(false, "the page timed out")),
            "Me he detenido: the page timed out. Di qué puedo decir para oír "
            "las órdenes que conozco.");
  EXPECT_EQ(voice::Relocalized(kEs, NoPlannerSpeech(NoPlanner::kNoPage, "")),
            "No hay ninguna pestaña en la que trabajar. Di abre y el nombre "
            "de un sitio primero.");
  // English as it always was.
  EXPECT_EQ(voice::Relocalized(voice::Language::kEnglish,
                               PlanStepSpeech("Search the site", 1, 2)),
            "I'll search the site.");
}

}  // namespace
}  // namespace voice_agent
}  // namespace molt_ai
