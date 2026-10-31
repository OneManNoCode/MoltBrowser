// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's language: the setting and the browser's language, the
// platform voice it speaks with, what the browser says in it
// (spoken_answers.h), and the voice loop planning in Spanish on the hand-
// built pages the English suites use. English's own suites are unchanged.

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_map_test_trees.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_language.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(VoiceLanguageTest, TheSettingThenTheBrowsersLanguage) {
  EXPECT_EQ(StoredVoiceLanguage("es"), Language::kSpanish);
  EXPECT_EQ(StoredVoiceLanguage("en"), Language::kEnglish);
  // Only the two values: anything else means the browser's language.
  for (std::string_view other : {"", "ES", "es-MX", "fr", " es", "english"}) {
    EXPECT_FALSE(StoredVoiceLanguage(other)) << other;
  }
  EXPECT_EQ(VoiceLanguageFor("es", "en-US"), Language::kSpanish);
  EXPECT_EQ(VoiceLanguageFor("en", "es-419"), Language::kEnglish);
  EXPECT_EQ(VoiceLanguageFor("", "es-419"), Language::kSpanish);
  EXPECT_EQ(VoiceLanguageFor("", "es"), Language::kSpanish);
  EXPECT_EQ(VoiceLanguageFor("", "en-GB"), Language::kEnglish);
  // A browser language voice mode doesn't have: English.
  EXPECT_EQ(VoiceLanguageFor("", "fr"), Language::kEnglish);
  EXPECT_EQ(VoiceLanguageFor("de", "pt-BR"), Language::kEnglish);
  EXPECT_EQ(VoiceLanguageFor("", ""), Language::kEnglish);
}

TEST(VoiceLanguageTest, ListeningOnlyWhereThereIsARecognizer) {
  EXPECT_TRUE(ListeningAvailable(Language::kEnglish));
  EXPECT_EQ(ListeningUnavailableNotice(Language::kEnglish), "");
  EXPECT_FALSE(ListeningAvailable(Language::kSpanish));
  EXPECT_NE(ListeningUnavailableNotice(Language::kSpanish).find("español"),
            std::string_view::npos);
  EXPECT_EQ(LanguageDisplayName(Language::kSpanish), "Español");
}

TEST(VoiceLanguageTest, PlatformTagAndVoice) {
  EXPECT_EQ(PlatformLanguageTag(Language::kEnglish, "es-MX"), "en-US");
  EXPECT_EQ(PlatformLanguageTag(Language::kSpanish, "es-MX"), "es-MX");
  EXPECT_EQ(PlatformLanguageTag(Language::kSpanish, "es_ES"), "es-ES");
  EXPECT_EQ(PlatformLanguageTag(Language::kSpanish, "en-US"), "es");
  const std::vector<PlatformVoice> voices = {
      {"Samantha", "en-US", false},
      {"Cloud Lucia", "es-ES", true},  // remote: never
      {"Monica", "es_ES", false},
      {"Paulina", "es-MX", false},
      {"Unknown", "", false},
  };
  // The region's voice first, then any of the language; never a remote one.
  EXPECT_EQ(PickPlatformVoice(voices, Language::kSpanish, "es-MX"), 3u);
  EXPECT_EQ(PickPlatformVoice(voices, Language::kSpanish, "es-ES"), 2u);
  EXPECT_EQ(PickPlatformVoice(voices, Language::kSpanish, "es"), 2u);
  EXPECT_EQ(PickPlatformVoice(voices, Language::kEnglish, "en-US"), 0u);
  EXPECT_FALSE(
      PickPlatformVoice({{"Cloud", "es-ES", true}}, Language::kSpanish, "es"));
  EXPECT_FALSE(PickPlatformVoice({}, Language::kSpanish, "es"));
}

// --- What the browser says -------------------------------------------------

TEST(SpokenAnswersTest, EnglishIsTheSentenceItself) {
  EXPECT_EQ(Localized(Language::kEnglish, "Zoomed in."), "Zoomed in.");
  EXPECT_EQ(Said(Language::kEnglish, "Playing {0}.", {"Queen"}),
            "Playing Queen.");
  EXPECT_EQ(
      Said(Language::kEnglish, "Searching {0} for {1}", {"YouTube", "cats"}),
      "Searching YouTube for cats");
  EXPECT_TRUE(SpokenAnswers(Language::kEnglish).empty());
}

TEST(SpokenAnswersTest, SpanishFromTheCatalog) {
  EXPECT_EQ(Localized(Language::kSpanish, "Zoomed in."), "Zoom ampliado.");
  EXPECT_EQ(Said(Language::kSpanish, "Playing {0}.", {"Queen"}),
            "Reproduciendo Queen.");
  // The words may go elsewhere in the sentence.
  EXPECT_EQ(
      Said(Language::kSpanish, "Searching {0} for {1}", {"YouTube", "gatos"}),
      "Buscando gatos en YouTube");
  // A sentence the catalog lacks is said as it is.
  EXPECT_EQ(Localized(Language::kSpanish, "Not in the catalog."),
            "Not in the catalog.");
  EXPECT_EQ(Said(Language::kSpanish, "Unknown {0}.", {"x"}), "Unknown x.");
}

TEST(SpokenAnswersTest, WordsAreInsertedOnceAsTheyAre) {
  // Page text with placeholders or braces in it stays text.
  EXPECT_EQ(Said(Language::kSpanish, "Playing {0}.", {"{0}{1}{{"}),
            "Reproduciendo {0}{1}{{.");
  EXPECT_EQ(Said(Language::kEnglish, "{1} and {0}, {{x}", {"a", "b"}),
            "b and a, {x}");
  // A placeholder without its word is left out; a lone brace stays.
  EXPECT_EQ(Said(Language::kEnglish, "a{2}b{", {"x"}), "ab{");
  EXPECT_EQ(Said(Language::kEnglish, "", {}), "");
}

TEST(SpokenAnswersTest, TheCatalogIsSortedUniqueAndComplete) {
  const std::vector<SpokenAnswer>& answers = SpokenAnswers(Language::kSpanish);
  ASSERT_GT(answers.size(), 150u);
  std::set<std::string_view> seen;
  for (size_t i = 0; i < answers.size(); ++i) {
    EXPECT_TRUE(seen.insert(answers[i].english).second) << answers[i].english;
    EXPECT_FALSE(answers[i].text.empty()) << answers[i].english;
    if (i > 0) {
      EXPECT_LT(answers[i - 1].english, answers[i].english);
    }
    // The same placeholders on both sides.
    for (char digit = '0'; digit <= '9'; ++digit) {
      const std::string placeholder = std::string("{") + digit + "}";
      EXPECT_EQ(answers[i].english.find(placeholder) == std::string_view::npos,
                answers[i].text.find(placeholder) == std::string_view::npos)
          << answers[i].english;
    }
  }
}

TEST(SpokenAnswersTest, LengthsAndListsInSpanish) {
  EXPECT_EQ(SpokenLength(90, Language::kSpanish), "1 minuto 30 segundos");
  EXPECT_EQ(SpokenLength(1, Language::kSpanish), "1 segundo");
  EXPECT_EQ(SpokenDuration(367, Language::kSpanish), "6 minutos");
  EXPECT_EQ(SpokenDuration(3700, Language::kSpanish), "1 hora 2 minutos");
  // English as before.
  EXPECT_EQ(SpokenLength(90), "1 minute 30 seconds");
  EXPECT_EQ(SpokenLength(90, Language::kEnglish), SpokenLength(90));
  EXPECT_EQ(ChooseHint(Language::kSpanish), "Di un número o un nombre.");
  EXPECT_EQ(ChooseHint(Language::kEnglish), ChooseHint());
}

TEST(SpokenAnswersTest, FailuresInSpanish) {
  for (StepFailure failure :
       {StepFailure::kNotActive, StepFailure::kStale, StepFailure::kNotAllowed,
        StepFailure::kDisabled, StepFailure::kNotEditable,
        StepFailure::kCredentialField, StepFailure::kTooLong,
        StepFailure::kNotMedia, StepFailure::kNoMediaSession,
        StepFailure::kFocusNotConfirmed, StepFailure::kBlockedURL,
        StepFailure::kNoTab, StepFailure::kUnavailable}) {
    const std::string_view english = FailureText(failure);
    EXPECT_EQ(FailureText(failure, Language::kEnglish), english);
    const std::string_view spanish = FailureText(failure, Language::kSpanish);
    EXPECT_FALSE(spanish.empty());
    EXPECT_NE(spanish, english) << english;
  }
  EXPECT_EQ(FailureText(StepFailure::kCancelled, Language::kSpanish), "");
}

// --- The voice loop in Spanish ---------------------------------------------

VoiceEnvironment SpanishEnv() {
  VoiceEnvironment env;
  env.language = Language::kSpanish;
  env.page_id = 1;
  env.host = "www.youtube.com";
  env.web_page = true;
  env.can_go_back = true;
  env.now_ms = 1000000;
  env.tabs = {{"bohemian rhapsody - YouTube", "www.youtube.com"},
              {"Correo", "mail.example"}};
  return env;
}

std::string SaidIn(const VoiceTurn& turn) {
  std::string out;
  for (const VoiceStep& step : turn.steps) {
    if (step.kind == VoiceStepKind::kSpeak) {
      if (!out.empty()) {
        out.push_back('|');
      }
      out.append(step.text);
    }
  }
  return out;
}

TEST(VoiceLoopSpanishTest, OpenAndAnnounce) {
  VoiceEnvironment env = SpanishEnv();
  env.host.clear();
  VoiceTurn turn = PlanUtterance("Oye Molt, abre YouTube", PageMap(), env,
                                 ConversationState());
  EXPECT_EQ(turn.kind, VoiceCommandKind::kOpenSite);
  ASSERT_EQ(turn.steps.size(), 2u);
  EXPECT_EQ(turn.steps[0].url, "https://www.youtube.com/");
  EXPECT_EQ(turn.status, "Abriendo YouTube");
  turn = PlanFollowUp(turn.steps[1], PageMap(), env, turn.state);
  EXPECT_EQ(SaidIn(turn), "YouTube está abierto.");
  // The English loop says what it always did.
  VoiceEnvironment english = env;
  english.language = Language::kEnglish;
  turn = PlanUtterance("open YouTube", PageMap(), english, ConversationState());
  EXPECT_EQ(turn.status, "Opening YouTube");
}

TEST(VoiceLoopSpanishTest, CommandsAnswerInSpanish) {
  const VoiceEnvironment env = SpanishEnv();
  const ConversationState state;
  EXPECT_EQ(SaidIn(PlanUtterance("baja", PageMap(), env, state)), "Abajo.");
  EXPECT_EQ(SaidIn(PlanUtterance("acerca", PageMap(), env, state)),
            "Zoom ampliado.");
  EXPECT_EQ(SaidIn(PlanUtterance("pausa", PageMap(), env, state)),
            "No se está reproduciendo nada.");
  EXPECT_EQ(PlanUtterance("atrás", PageMap(), env, state).status,
            "Volviendo atrás");
  VoiceEnvironment playing = env;
  playing.media_session = true;
  playing.media_playing = true;
  EXPECT_EQ(SaidIn(PlanUtterance("adelanta un minuto y medio", PageMap(),
                                 playing, state)),
            "He adelantado 1 minuto 30 segundos.");
  // "Help" reads the Spanish reference's examples.
  const std::string help =
      SaidIn(PlanUtterance("¿qué puedo decir?", PageMap(), env, state));
  EXPECT_EQ(help.rfind("Puedes decir: ", 0), 0u) << help;
  EXPECT_NE(help.find("abre YouTube"), std::string::npos) << help;
  EXPECT_NE(help.find("Órdenes de voz"), std::string::npos) << help;
  // What it can't do yet, quoted as heard.
  const VoiceTurn unknown =
      PlanUtterance("prepara un café", PageMap(), env, state);
  ASSERT_FALSE(unknown.steps.empty());
  EXPECT_EQ(unknown.steps.back().preface,
            "Todavía no puedo hacer eso: \"prepara un café\". Di qué puedo "
            "decir para saber lo que puedo hacer.");
}

TEST(VoiceLoopSpanishTest, ResultsReadAndChosenInSpanish) {
  const PageMap map = BuildPageMap(test::YouTubeLikeResults());
  VoiceEnvironment env = SpanishEnv();
  ConversationState state;
  VoiceTurn turn = PlanUtterance("lee los resultados", map, env, state);
  const std::string said = SaidIn(turn);
  EXPECT_EQ(said.rfind("20 resultados.", 0), 0u) << said;
  EXPECT_NE(said.find("Di un número o un nombre."), std::string::npos);
  state = turn.state;
  turn = PlanUtterance("reproduce el cuarto", map, env, state);
  EXPECT_EQ(turn.kind, VoiceCommandKind::kChoose);
  EXPECT_FALSE(turn.steps.empty());
  turn = PlanUtterance("el cuatro", map, env, state);
  EXPECT_EQ(turn.kind, VoiceCommandKind::kChoose);
}

TEST(VoiceLoopSpanishTest, SpanishButtonsAskFirst) {
  PageMap map;
  EXPECT_TRUE(NeedsConfirmation("Comprar ahora", "", map, Language::kSpanish));
  EXPECT_TRUE(NeedsConfirmation("Cerrar sesión", "", map, Language::kSpanish));
  EXPECT_TRUE(NeedsConfirmation("Eliminar", "", map, Language::kSpanish));
  EXPECT_FALSE(
      NeedsConfirmation("Añadir al carrito", "", map, Language::kSpanish));
  // English's checks are the same in every language, and English's
  // answers are unchanged.
  EXPECT_TRUE(NeedsConfirmation("Buy now", "", map, Language::kSpanish));
  EXPECT_EQ(NeedsConfirmation("Comprar ahora", "", map, Language::kEnglish),
            NeedsConfirmation("Comprar ahora", "", map));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
