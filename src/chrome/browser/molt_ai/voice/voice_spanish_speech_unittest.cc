// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Everything else voice mode says, in Spanish (docs/design/
// TOUCHLESS_BROWSING.md §4.1): sentences made in English where the
// language isn't known and said in the window's (Relocalized()), the wake
// phrases per language, routines and learning, the activation guard's
// questions and notices, and a list's field words. English is unchanged.

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/security/activation_policy.h"
#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/page_reading.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"
#include "chrome/browser/molt_ai/voice/voice_language.h"
#include "chrome/browser/molt_ai/voice/voice_listener_protocol.h"
#include "chrome/browser/molt_ai/voice/voice_routines_logic.h"
#include "chrome/browser/molt_ai/voice/voice_speech_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr Language kEs = Language::kSpanish;
constexpr Language kEn = Language::kEnglish;

// --- Relocalized() ----------------------------------------------------------

TEST(RelocalizedTest, EnglishIsAlwaysTheTextItself) {
  for (std::string_view text :
       {"The page tried to open a new window to evil.example.",
        "I stopped my news at step 3.", "anything at all", ""}) {
    EXPECT_EQ(Relocalized(kEn, text), text);
  }
}

TEST(RelocalizedTest, AWholeSentenceOrTheKeyItWasMadeFrom) {
  EXPECT_EQ(Relocalized(kEs, "The page tried to open another app."),
            "La página intentó abrir otra aplicación.");
  EXPECT_EQ(Relocalized(kEs,
                        Said(kEn, "The page tried to open a new window to {0}.",
                             {"evil.example"})),
            "La página intentó abrir una ventana nueva de evil.example.");
  EXPECT_EQ(
      Relocalized(kEs, Said(kEn, "You have {0} routines called {1}. Choose "
                                 "another name, or rename them in Agent mode.",
                            {"2", "my news"})),
      "Tienes 2 rutinas llamadas my news. Elige otro nombre, o cámbiales el "
      "nombre en el modo agente.");
  // The words in a key's place are inserted as they are, once.
  EXPECT_EQ(Relocalized(kEs, "Step 3 of this routine isn't one I can do by "
                             "voice."),
            "El paso 3 de esta rutina no es uno que pueda hacer por voz.");
  EXPECT_EQ(Relocalized(kEs, "Searching the web for {1}{0}."),
            "Buscando {1}{0} en la web.");
}

TEST(RelocalizedTest, OnlyWhatAKeyMade) {
  // No key made it: as it is.
  EXPECT_EQ(Relocalized(kEs, "Bohemian Rhapsody"), "Bohemian Rhapsody");
  // A key with too little text of its own ("{0}." ) never matches.
  EXPECT_EQ(Relocalized(kEs, "Queen."), "Queen.");
  // The key with the most text of its own wins ("I stopped: {0}. Say
  // ..." over "{0}. Say ..."-like shorter ones).
  EXPECT_EQ(Relocalized(kEs, "I stopped: no network. Say what can I say to "
                             "hear the commands I know."),
            "Me he detenido: no network. Di qué puedo decir para oír las "
            "órdenes que conozco.");
  // Too long to match: as it is.
  const std::string long_text(kMaxRelocalizedBytes + 1, 'a');
  EXPECT_EQ(Relocalized(kEs, long_text), long_text);
}

// --- The wake phrases --------------------------------------------------------

TEST(SpanishWakePhraseTest, GatedOnASpanishRecognizer) {
  EXPECT_EQ(WakePhrasesFor(kEn), std::vector<std::string>({"hey molt"}));
  // No Spanish recognizer yet: nothing changes.
  ASSERT_FALSE(ListeningAvailable(kEs));
  EXPECT_EQ(WakePhrasesFor(kEs), WakePhrasesFor(kEn));
  // Once there is one: "oye molt" first, within CONFIG's cap.
  const std::vector<std::string> spanish = SpanishWakePhrases();
  ASSERT_FALSE(spanish.empty());
  EXPECT_EQ(spanish.front(), "oye molt");
  EXPECT_LE(spanish.size(), kMaxWakePhrases);
  // Each is the Spanish grammar's wake phrase.
  for (const std::string& phrase : spanish) {
    ParseContext context;
    context.language = kEs;
    context.wake_phrase = phrase;
    const VoiceCommand command =
        ParseVoiceCommand(phrase + ", abre YouTube", context);
    EXPECT_EQ(command.kind, VoiceCommandKind::kOpenSite) << phrase;
    EXPECT_EQ(command.site, "youtube") << phrase;
  }
}

// --- Routines and learning ----------------------------------------------------

RoutineStep StepOf(VoiceCommand command) {
  RoutineStep step;
  step.command = std::move(command);
  return step;
}

TEST(SpanishRoutinesTest, StepsAreSaidAsSpanishsCommands) {
  VoiceCommand open;
  open.kind = VoiceCommandKind::kOpenSite;
  open.site = "YouTube";
  EXPECT_EQ(DescribeRoutineStep(StepOf(open), kEs), "abre YouTube");
  EXPECT_EQ(DescribeRoutineStep(StepOf(open), kEn), "open YouTube");
  EXPECT_EQ(DescribeRoutineStep(StepOf(open)), "open YouTube");
  VoiceCommand search;
  search.kind = VoiceCommandKind::kSearch;
  search.scope = SearchScope::kDefaultEngine;
  search.text = "noticias";
  EXPECT_EQ(DescribeRoutineStep(StepOf(search), kEs),
            "busca noticias en la web");
  VoiceCommand play;
  play.kind = VoiceCommandKind::kChoose;
  play.verb = ChoiceVerb::kPlay;
  play.choice.kind = ChoiceRef::Kind::kPosition;
  play.choice.ordinal = 1;
  EXPECT_EQ(DescribeRoutineStep(StepOf(play), kEs), "reproduce el primero");
  EXPECT_EQ(DescribeRoutineStep(StepOf(play), kEn), "play the first one");
  play.choice.from_end = true;
  EXPECT_EQ(DescribeRoutineStep(StepOf(play), kEs), "reproduce el último");
  play.choice.ordinal = 2;
  EXPECT_EQ(DescribeRoutineStep(StepOf(play), kEs),
            "reproduce el segundo desde el final");
  VoiceCommand skip;
  skip.kind = VoiceCommandKind::kSkipForward;
  skip.seconds = 90;
  EXPECT_EQ(DescribeRoutineStep(StepOf(skip), kEs),
            "adelanta 1 minuto 30 segundos");
  VoiceCommand scroll;
  scroll.kind = VoiceCommandKind::kScrollDown;
  scroll.scroll = ScrollAmount::kHalfPage;
  EXPECT_EQ(DescribeRoutineStep(StepOf(scroll), kEs), "baja un poco");
  EXPECT_EQ(DescribeRoutineStep(StepOf(scroll), kEn), "scroll down a bit");
  // A value not kept.
  VoiceCommand type;
  type.kind = VoiceCommandKind::kType;
  type.text = "1234";
  RoutineStep withheld = StepOf(type);
  withheld.variable = "pin";
  EXPECT_EQ(DescribeRoutineStep(withheld, kEs), "escribe lo que me digas");
}

TEST(SpanishRoutinesTest, ListsAreJoinedWithY) {
  EXPECT_EQ(JoinSpokenList({"abre YouTube", "busca noticias"}, kEs),
            "abre YouTube y busca noticias");
  EXPECT_EQ(JoinSpokenList({"a", "b", "c"}, kEs), "a, b y c");
  // "e" before an "i" sound, never before "hie".
  EXPECT_EQ(JoinSpokenList({"tomate", "higos"}, kEs), "tomate e higos");
  EXPECT_EQ(JoinSpokenList({"agua", "hielo"}, kEs), "agua y hielo");
  EXPECT_EQ(JoinSpokenList({"a", "b", "c"}, kEn), "a, b and c");
  EXPECT_EQ(JoinSpokenList({"a", "b", "c"}), "a, b and c");
}

VoiceEnvironment SpanishEnv() {
  VoiceEnvironment env;
  env.language = kEs;
  env.page_id = 1;
  env.host = "www.youtube.com";
  env.web_page = true;
  env.now_ms = 1000000;
  env.learning.routines_available = true;
  env.learning.enabled = true;
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

TEST(SpanishRoutinesTest, TheLoopsRoutineSentencesInSpanish) {
  VoiceEnvironment env = SpanishEnv();
  env.learning.routines = {"mis noticias", "trenes"};
  EXPECT_EQ(SaidIn(PlanUtterance("cuáles son mis rutinas", PageMap(), env,
                                 ConversationState())),
            "Tienes 2 rutinas: mis noticias y trenes. Di ejecuta y luego su "
            "nombre.");
  EXPECT_EQ(SaidIn(PlanUtterance("ejecuta la rutina del tiempo", PageMap(),
                                 env, ConversationState())),
            "No tengo ninguna rutina llamada del tiempo. Di cuáles son mis "
            "rutinas.");
  env.learning.this_steps = {"abre YouTube", "busca noticias"};
  env.learning.this_version = 3;
  const VoiceTurn remember = PlanUtterance("recuerda esto como el tiempo",
                                           PageMap(), env, ConversationState());
  const std::string said = SaidIn(remember);
  EXPECT_NE(said.find("Recordaré: abre YouTube y busca noticias."),
            std::string::npos)
      << said;
  EXPECT_NE(said.find("Di sí para guardarla como el tiempo"),
            std::string::npos)
      << said;
  // English says what it always did.
  VoiceEnvironment english = env;
  english.language = kEn;
  english.learning.this_steps = {"open YouTube", "search for news"};
  const std::string english_said = SaidIn(PlanUtterance(
      "remember this as the weather", PageMap(), english, ConversationState()));
  EXPECT_NE(english_said.find("I'll remember: open YouTube and search for "
                              "news."),
            std::string::npos)
      << english_said;
}

// --- The activation guard ----------------------------------------------------

TEST(SpanishGuardTest, QuestionsAndNoticesAreSaidInSpanish) {
  using activation::Request;
  const activation::Question window =
      activation::QuestionFor(Request::kNewWindow, "www.evil.example", "");
  // English as it always was.
  EXPECT_EQ(window.said, "The page tried to open a new window to "
                         "evil.example.");
  EXPECT_EQ(window.verb, "open it");
  EXPECT_EQ(Relocalized(kEs, window.said),
            "La página intentó abrir una ventana nueva de evil.example.");
  EXPECT_EQ(Relocalized(kEs, window.verb), "abrirla");
  const activation::Question app =
      activation::QuestionFor(Request::kExternalApp, "x.example", "mailto");
  EXPECT_EQ(Relocalized(kEs, app.said),
            "La página intentó abrir otra aplicación con un enlace mailto.");
  const activation::Question file =
      activation::QuestionFor(Request::kDownload, "", "");
  EXPECT_EQ(Relocalized(kEs, file.said),
            "La página intentó descargar un archivo.");
  EXPECT_EQ(Relocalized(kEs, file.verb), "descargarlo");
  EXPECT_EQ(Relocalized(kEs, activation::RefusalNotice(
                                 Request::kPermissionPrompt, "camera")),
            "La página pidió el permiso de camera. Ahora no puedo aceptar una "
            "respuesta a eso por voz, así que lo he rechazado.");
  for (Request request :
       {Request::kFullScreen, Request::kClipboardWrite,
        Request::kPictureInPicture, Request::kNewWindow, Request::kDownload,
        Request::kExternalApp}) {
    const std::string english = activation::RefusalNotice(request, "");
    EXPECT_NE(Relocalized(kEs, english), english) << english;
  }
}

// --- A list's field words ----------------------------------------------------

TEST(SpanishFieldWordsTest, CountsAndLengthsInSpanish) {
  EXPECT_EQ(SpokenCount("2.1B views", kEs), "2,1 mil millones de views");
  EXPECT_EQ(SpokenCount("1.2M views", kEs), "1,2 millones de views");
  EXPECT_EQ(SpokenCount("1M subscribers", kEs), "1 millón de subscribers");
  EXPECT_EQ(SpokenCount("15K likes", kEs), "15 mil likes");
  EXPECT_EQ(SpokenCount("312 comments", kEs), "312 comments");
  EXPECT_EQ(SpokenCount("2.1B views", kEn), "2.1 billion views");
  EXPECT_EQ(SpokenCount("2.1B views"), "2.1 billion views");
  ChoiceCandidate video;
  video.title = "Bohemian Rhapsody";
  video.fields.push_back({ChoiceFieldKind::kDuration, "6:07"});
  video.fields.push_back({ChoiceFieldKind::kCount, "1.2M views"});
  EXPECT_EQ(SpokenCandidate(video, Verbosity::kFull, kEs),
            "Bohemian Rhapsody, 6 minutos, 1,2 millones de views");
  EXPECT_EQ(SpokenCandidate(video, Verbosity::kFull, kEn),
            SpokenCandidate(video, Verbosity::kFull));
  EXPECT_EQ(SpokenCandidate(ChoiceCandidate(), Verbosity::kNormal, kEs),
            "sin título");
}

TEST(SpanishFieldWordsTest, HeadingsAreSaidInSpanish) {
  ReadingDocument document;
  ReadingBlock heading;
  heading.kind = ReadingBlockKind::kHeading;
  heading.level = 2;
  heading.text = "Historia";
  document.blocks.push_back(heading);
  EXPECT_EQ(MoveSpeech(document, 0, kEs), "Título de nivel 2: Historia.");
  EXPECT_EQ(MoveSpeech(document, 0), "Heading level 2: Historia.");
  EXPECT_EQ(MoveSpeech(document, 0, kEn), MoveSpeech(document, 0));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
