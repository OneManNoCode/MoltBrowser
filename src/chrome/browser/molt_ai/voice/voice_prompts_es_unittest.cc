// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice gesture and the browser's prompts in Spanish
// (docs/design/TOUCHLESS_BROWSING.md §4.1, §7.3): Spanish's confirmation
// words heard with Spanish's phonetic key, every prompt kind said and
// answered in both languages, and English exactly as it was.

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/confirmation_words_es.h"
#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/voice_gesture.h"
#include "chrome/browser/molt_ai/voice/voice_gesture_channel.h"
#include "chrome/browser/molt_ai/voice/voice_prompts.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

constexpr Language kEs = Language::kSpanish;
constexpr Language kEn = Language::kEnglish;

constexpr HeardEvidence kClean{/*from_microphone=*/true, /*clean=*/true};
constexpr HeardEvidence kNotClean{/*from_microphone=*/true, /*clean=*/false};

GestureBinding Camera(std::string site = "https://example.com camera") {
  return GestureBinding{GestureAction::kAllowPermission, std::move(site)};
}

// splitmix64, as voice_gesture_unittest.cc's.
class Counter {
 public:
  explicit Counter(uint64_t start) : next_(start) {}
  uint64_t operator()() {
    uint64_t z = (next_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

 private:
  uint64_t next_;
};

size_t IndexOf(std::string_view word, Language language) {
  const auto words = ConfirmationWords(language);
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i] == word) {
      return i;
    }
  }
  return words.size();
}

// --- The words -------------------------------------------------------------

TEST(SpanishGestureWordsTest, TheListIsSpanishsInSpanish) {
  ASSERT_EQ(ConfirmationWords(kEs).size(), SpanishConfirmationWords().size());
  EXPECT_EQ(ConfirmationWords(kEs).data(), SpanishConfirmationWords().data());
  EXPECT_EQ(ConfirmationWords(kEn).data(), ConfirmationWords().data());
  EXPECT_EQ(AnswerWords(kEs).data(), SpanishAnswerWords().data());
  EXPECT_EQ(AnswerWords(kEn).data(), AnswerWords().data());
}

// Each word, its plural, split in two and with what is said around it, is
// that word for sure: under Spanish's key no other word of the list is
// within the bound plus a close sound (the list's own rule), so the margin
// never makes a word of the list unsure.
TEST(SpanishGestureWordsTest, EveryWordOfTheListIsSure) {
  const auto words = ConfirmationWords(kEs);
  for (size_t i = 0; i < words.size(); ++i) {
    const std::string word(words[i]);
    const char last = word.back();
    const bool vowel =
        last == 'a' || last == 'e' || last == 'i' || last == 'o' || last == 'u';
    const std::string plural =
        last == 's' ? word : word + (vowel ? "s" : "es");
    for (const std::string& said :
         {word, plural, "di " + word, "sí, " + word, "la " + word,
          "di la " + word, word + " por favor", "vale " + word + " gracias",
          word + ", porfa"}) {
      const HeardWord heard = HearConfirmationWord(said, kEs);
      EXPECT_EQ(heard.kind, HeardWord::Kind::kWord) << said;
      EXPECT_EQ(heard.index, i) << said;
      EXPECT_EQ(MatchConfirmationWord(said, kEs), i) << said;
    }
    // Split in two by a recognizer.
    const std::string split = word.substr(0, 3) + " " + word.substr(3);
    EXPECT_EQ(MatchConfirmationWord(split, kEs), i) << split;
  }
}

TEST(SpanishGestureWordsTest, AccentsAndCaseAreFolded) {
  EXPECT_EQ(MatchConfirmationWord("Plátano", kEs), IndexOf("platano", kEs));
  EXPECT_EQ(MatchConfirmationWord("¡Camión!", kEs), IndexOf("camion", kEs));
  EXPECT_EQ(MatchConfirmationWord("camiones", kEs), IndexOf("camion", kEs));
  EXPECT_EQ(MatchConfirmationWord("Di el melocotón, por favor.", kEs),
            IndexOf("melocoton", kEs));
}

// A Spanish recognizer's spelling of the word: Spanish's key hears "h" as
// silent and "v" as "b".
TEST(SpanishGestureWordsTest, SoundAlikesBySpanishsKey) {
  EXPECT_EQ(MatchConfirmationWord("ormiga", kEs), IndexOf("hormiga", kEs));
  EXPECT_EQ(MatchConfirmationWord("cavallo", kEs), IndexOf("caballo", kEs));
  EXPECT_EQ(MatchConfirmationWord("bizcocho", kEs), IndexOf("bizcocho", kEs));
  EXPECT_EQ(MatchConfirmationWord("biscocho", kEs), IndexOf("bizcocho", kEs));
}

TEST(SpanishGestureWordsTest, AnswersAreNeverWords) {
  for (std::string_view answer : AnswerWords(kEs)) {
    EXPECT_EQ(HearConfirmationWord(answer, kEs).kind, HeardWord::Kind::kNone)
        << answer;
  }
  // Every answer of every prompt, in both languages: an answer that is
  // checked after the word ("sí", "permite", "consérvalo") never reads as a
  // word, and neither does one checked before it.
  for (Language language : {kEn, kEs}) {
    for (std::string_view answer : PromptAnswerPhrases(language)) {
      EXPECT_EQ(HearConfirmationWord(answer, language).kind,
                HeardWord::Kind::kNone)
          << answer;
    }
  }
}

TEST(SpanishGestureWordsTest, OtherLanguagesWordsAreNotWords) {
  // English's words in a Spanish prompt are no word of its list, and
  // Spanish's in an English prompt none of English's: a prompt is heard in
  // its own language.
  EXPECT_EQ(HearConfirmationWord("harbor", kEs).kind, HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("umbrella", kEs).kind,
            HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("manzana", kEn).kind, HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("abre youtube", kEs).kind,
            HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("", kEs).kind, HeardWord::Kind::kNone);
  EXPECT_EQ(HearConfirmationWord("la manzana roja grande de mi casa", kEs).kind,
            HeardWord::Kind::kNone);
}

// What is near two words of the list is unsure, as in English. Found, not
// chosen: every word whose Spanish key is within the bound of one word and
// within one more of another.
TEST(SpanishGestureWordsTest, ASoundAlikeOfTwoWordsIsUnsure) {
  const auto words = ConfirmationWords(kEs);
  // Each word with one sound changed: never another word for sure unless
  // only that word is near.
  int unsure = 0;
  for (std::string_view word : words) {
    for (char c : std::string_view("aeiosnlrtkpm")) {
      for (size_t at = 1; at + 1 < word.size(); ++at) {
        std::string changed(word);
        changed[at] = c;
        const HeardWord heard = HearConfirmationWord(changed, kEs);
        if (heard.kind == HeardWord::Kind::kUnsure) {
          ++unsure;
          EXPECT_FALSE(MatchConfirmationWord(changed, kEs)) << changed;
        }
        if (heard.kind == HeardWord::Kind::kWord) {
          // Sure: no other word is within the bound plus the margin.
          const std::string key = SpanishPhoneticKey(changed);
          for (size_t j = 0; j < words.size(); ++j) {
            if (j == heard.index) {
              continue;
            }
            const std::string other = SpanishPhoneticKey(words[j]);
            const int bound = PhoneticBound(std::max(key.size(), other.size()));
            EXPECT_GT(PhoneticDistance(key, other, bound + 2),
                      bound + kSureSoundAlikeMargin)
                << changed << " / " << words[j];
          }
        }
      }
    }
  }
  EXPECT_GT(unsure, 0);
}

// --- The capability in Spanish ---------------------------------------------

TEST(SpanishGestureBookTest, ASpanishPromptDrawsAndHearsSpanish) {
  GestureBook book{Counter(3)};
  std::optional<GesturePrompt> prompt = book.Begin(Camera(), kEs, 0);
  ASSERT_TRUE(prompt);
  EXPECT_EQ(prompt->language, kEs);
  EXPECT_LT(IndexOf(prompt->word, kEs), ConfirmationWords(kEs).size());
  VoiceGesture gesture;
  // Not cleanly: nothing.
  EXPECT_EQ(book.Hear(prompt->id, prompt->word, kNotClean, 50, &gesture),
            HearResult::kNotClean);
  EXPECT_EQ(book.Hear(prompt->id, "di la " + prompt->word + " por favor",
                      kClean, 100, &gesture),
            HearResult::kMinted);
  EXPECT_TRUE(book.Consume(gesture, Camera(), 200));
}

TEST(SpanishGestureBookTest, AnotherSpanishWordIsOneTry) {
  for (uint64_t seed = 0; seed < 10; ++seed) {
    GestureBook first{Counter(seed)};
    std::optional<GesturePrompt> prompt = first.Begin(Camera(), kEs, 0);
    ASSERT_TRUE(prompt);
    for (std::string_view other : ConfirmationWords(kEs)) {
      if (other == prompt->word) {
        continue;
      }
      GestureBook book{Counter(seed)};
      std::optional<GesturePrompt> same = book.Begin(Camera(), kEs, 0);
      ASSERT_TRUE(same);
      ASSERT_EQ(same->word, prompt->word);
      VoiceGesture gesture;
      EXPECT_EQ(book.Hear(same->id, std::string(other), kClean, 100, &gesture),
                HearResult::kWrongWord)
          << other << " for " << prompt->word;
    }
    // English's words are no word in a Spanish prompt: it keeps waiting.
    VoiceGesture gesture;
    EXPECT_EQ(first.Hear(prompt->id, "harbor", kClean, 100, &gesture),
              HearResult::kNotAWord);
    EXPECT_NE(first.Find(prompt->id, 100), nullptr);
  }
}

// The guesser's odds in Spanish: 40 words, never one of the last 8, so a
// guess is right 1 time in 32, as in English.
TEST(SpanishGestureBookTest, FreshWordsNeverTheRecentOnes) {
  EXPECT_EQ(ConfirmationWords(kEs).size(), 40u);
  GestureBook book{Counter(11)};
  std::vector<std::string> words;
  for (int i = 0; i < 300; ++i) {
    std::optional<GesturePrompt> prompt =
        book.Begin(Camera("s" + std::to_string(i)), kEs, i);
    ASSERT_TRUE(prompt);
    for (size_t back = 1; back <= kRecentWordsAvoided && back <= words.size();
         ++back) {
      EXPECT_NE(prompt->word, words[words.size() - back]);
    }
    words.push_back(prompt->word);
    book.Cancel(prompt->id);
  }
  const std::set<std::string> seen(words.begin(), words.end());
  EXPECT_EQ(seen.size(), ConfirmationWords(kEs).size());
  for (const std::string& word : seen) {
    EXPECT_LT(IndexOf(word, kEs), ConfirmationWords(kEs).size()) << word;
  }
}

TEST(SpanishGestureBookTest, ARetiredSpanishWordIsNeitherTheWordNorAMiss) {
  GestureBook book{Counter(5)};
  std::optional<GesturePrompt> first = book.Begin(Camera(), kEs, 0);
  ASSERT_TRUE(first);
  std::optional<GesturePrompt> again = book.Begin(Camera(), kEs, 10);
  ASSERT_TRUE(again);
  ASSERT_NE(again->word, first->word);
  VoiceGesture gesture;
  EXPECT_EQ(book.Hear(again->id, first->word, kClean, 20, &gesture),
            HearResult::kRetired);
  EXPECT_EQ(book.Hear(again->id, again->word, kClean, 30, &gesture),
            HearResult::kMinted);
}

// --- Every prompt kind, in both languages -----------------------------------

BrowserPrompt Prompt(PromptKind kind,
                     Language language,
                     std::string site,
                     std::string text = std::string()) {
  BrowserPrompt prompt;
  prompt.kind = kind;
  prompt.language = language;
  prompt.site = std::move(site);
  prompt.text = std::move(text);
  return prompt;
}

PendingPrompt Waiting(PromptKind kind,
                      Language language,
                      std::string word = std::string()) {
  PendingPrompt prompt;
  prompt.id = 7;
  prompt.kind = kind;
  prompt.language = language;
  prompt.word = std::move(word);
  prompt.expires_ms = 1000;
  return prompt;
}

PromptAnswer Answer(const PendingPrompt& prompt, std::string_view heard) {
  return ParsePromptReply(prompt, heard).answer;
}

TEST(SpanishPromptsTest, Permission) {
  BrowserPrompt prompt =
      Prompt(PromptKind::kPermission, kEs, "www.ejemplo.com");
  prompt.what = "usar tu cámara y tu micrófono";
  EXPECT_EQ(PromptSpeech(prompt, "manzana"),
            "ejemplo.com quiere usar tu cámara y tu micrófono. Para "
            "permitirlo, di manzana. Para bloquearlo, di no.");
  EXPECT_EQ(PromptSpeech(prompt, WordOnScreen(kEs)),
            "ejemplo.com quiere usar tu cámara y tu micrófono. Para "
            "permitirlo, di la palabra que ves en la pantalla. Para "
            "bloquearlo, di no.");
  EXPECT_EQ(PromptSpeech(prompt, ""),
            "ejemplo.com quiere usar tu cámara y tu micrófono. Para "
            "bloquearlo, di no. Para permitirlo, usa el ratón o el teclado.");
  prompt.screen_shared = true;
  EXPECT_EQ(PromptSpeech(prompt, ""),
            "ejemplo.com quiere usar tu cámara y tu micrófono. No puedo "
            "aceptar la palabra mientras compartes la pantalla, así que para "
            "permitirlo usa el ratón o el teclado. Para bloquearlo, di no.");
  prompt.screen_shared = false;
  prompt.what.clear();
  prompt.site.clear();
  prompt.explain_shown_only = true;
  EXPECT_EQ(PromptSpeech(prompt, "manzana"),
            "Esta página pide un permiso. Para permitirlo, di manzana. Para "
            "bloquearlo, di no. No digo la palabra en voz alta mientras otro "
            "programa pueda oírla.");
  // Page text in |what| is dropped as in English; accented letters stay.
  prompt.what = "usar tu <cámara>\n\xE2\x80\xAE";
  EXPECT_EQ(PromptSpeech(prompt, "manzana")
                .rfind("Esta página quiere usar tu cámara.", 0),
            0u);

  const PendingPrompt waiting =
      Waiting(PromptKind::kPermission, kEs, "manzana");
  EXPECT_EQ(Answer(waiting, "manzana"), PromptAnswer::kAllow);
  EXPECT_EQ(Answer(waiting, "sí, manzana, por favor"), PromptAnswer::kAllow);
  EXPECT_EQ(Answer(waiting, "tomate"), PromptAnswer::kWrongWord);
  EXPECT_EQ(Answer(waiting, "No."), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(waiting, "Bloquéalo"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(waiting, "no lo permitas"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(waiting, "deniega"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(waiting, "cancela"), PromptAnswer::kDecline);
  EXPECT_EQ(Answer(waiting, "ahora no"), PromptAnswer::kDecline);
  EXPECT_EQ(Answer(waiting, "sí"), PromptAnswer::kNeedWord);
  EXPECT_EQ(Answer(waiting, "permítelo"), PromptAnswer::kNeedWord);
  // English's answers are a command as usual in Spanish ("no" is no in
  // both).
  EXPECT_EQ(Answer(waiting, "block it"), PromptAnswer::kNone);
  EXPECT_EQ(Answer(waiting, "harbor"), PromptAnswer::kNone);
  EXPECT_EQ(ReplySpeech(waiting, ParsePromptReply(waiting, "sí")),
            "Para permitirlo, di manzana.");
  EXPECT_EQ(ReplySpeechWithoutWord(waiting, ParsePromptReply(waiting, "sí")),
            "Para permitirlo, di la palabra que ves en la pantalla.");
  EXPECT_EQ(ReplySpeech(waiting, ParsePromptReply(waiting, "no")),
            "Bloqueado.");
  EXPECT_EQ(ReplySpeech(waiting, ParsePromptReply(waiting, "cancela")),
            "Descartado.");
  EXPECT_EQ(ReplySpeech(waiting, ParsePromptReply(waiting, "tomate")),
            "Esa no era la palabra, así que lo he rechazado.");
  EXPECT_EQ(PromptTimeoutSpeech(PromptKind::kPermission, kEs),
            "No has respondido, así que he descartado la solicitud de "
            "permiso.");
}

TEST(SpanishPromptsTest, Dialogs) {
  BrowserPrompt alert = Prompt(PromptKind::kAlert, kEs, "ejemplo.com",
                               "Tu sesión va a caducar");
  EXPECT_EQ(PromptSpeech(alert, ""),
            "ejemplo.com dice: Tu sesión va a caducar. Di vale para "
            "cerrarlo.");
  alert.flood = true;
  EXPECT_EQ(PromptSpeech(alert, ""),
            "ejemplo.com muestra otro mensaje. Di vale para cerrarlo.");
  alert.flood = false;
  alert.text.clear();
  EXPECT_EQ(PromptSpeech(alert, ""),
            "ejemplo.com muestra un mensaje. Di vale para cerrarlo.");
  const PendingPrompt closing = Waiting(PromptKind::kAlert, kEs);
  EXPECT_EQ(Answer(closing, "vale"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(closing, "ciérralo"), PromptAnswer::kNone);
  EXPECT_EQ(Answer(closing, "cierra"), PromptAnswer::kAccept);
  EXPECT_EQ(ReplySpeech(closing, ParsePromptReply(closing, "entendido")),
            "Cerrado.");

  BrowserPrompt confirm = Prompt(PromptKind::kConfirm, kEs, "ejemplo.com",
                                 "¿Vaciar el carrito?");
  EXPECT_EQ(PromptSpeech(confirm, ""),
            "ejemplo.com pregunta: ¿Vaciar el carrito? Di sí o no.");
  confirm.text.clear();
  EXPECT_EQ(PromptSpeech(confirm, ""),
            "ejemplo.com te pide que confirmes. Di sí o no.");
  confirm.flood = true;
  EXPECT_EQ(PromptSpeech(confirm, ""),
            "ejemplo.com hace otra pregunta. Di sí o no.");
  const PendingPrompt asking = Waiting(PromptKind::kConfirm, kEs);
  EXPECT_EQ(Answer(asking, "sí"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(asking, "de acuerdo"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(asking, "no, gracias"), PromptAnswer::kDecline);
  EXPECT_EQ(Answer(asking, "yes"), PromptAnswer::kNone);
  EXPECT_EQ(ReplySpeech(asking, ParsePromptReply(asking, "sí")), "Sí.");
  EXPECT_EQ(ReplySpeech(asking, ParsePromptReply(asking, "no")), "No.");

  BrowserPrompt text = Prompt(PromptKind::kTextPrompt, kEs, "ejemplo.com",
                              "¿Tu nombre?");
  EXPECT_EQ(PromptSpeech(text, ""),
            "ejemplo.com pregunta: ¿Tu nombre? Di responde y luego tu "
            "respuesta, o di no para cancelar.");
  text.text.clear();
  EXPECT_EQ(PromptSpeech(text, ""),
            "ejemplo.com pide una respuesta. Di responde y luego tu "
            "respuesta, o di no para cancelar.");
  PendingPrompt answering = Waiting(PromptKind::kTextPrompt, kEs);
  const PromptReply dictated =
      ParsePromptReply(answering, "Responde Ada Lovelace");
  EXPECT_EQ(dictated.answer, PromptAnswer::kDictate);
  EXPECT_EQ(dictated.text, "Ada Lovelace");
  EXPECT_EQ(ReplySpeech(answering, dictated),
            "Tu respuesta: Ada Lovelace. Di sí para enviarla, o no para "
            "cancelar.");
  EXPECT_EQ(ParsePromptReply(answering, "mi respuesta es Bob").text, "Bob");
  EXPECT_EQ(Answer(answering, "sí"), PromptAnswer::kAccept);
  EXPECT_EQ(ReplySpeech(answering, ParsePromptReply(answering, "sí")),
            "Enviado.");
  EXPECT_EQ(ReplySpeech(answering, ParsePromptReply(answering, "cancela")),
            "Cancelado.");

  BrowserPrompt leave = Prompt(PromptKind::kBeforeUnload, kEs, "ejemplo.com");
  EXPECT_EQ(PromptSpeech(leave, ""),
            "¿Salir de ejemplo.com? Puede que no se guarden los cambios que "
            "hiciste. Di sal o quédate.");
  leave.reload = true;
  leave.site.clear();
  EXPECT_EQ(PromptSpeech(leave, ""),
            "¿Recargar esta página? Puede que no se guarden los cambios que "
            "hiciste. Di recarga o quédate.");
  const PendingPrompt leaving = Waiting(PromptKind::kBeforeUnload, kEs);
  EXPECT_EQ(Answer(leaving, "sal"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(leaving, "recárgala"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(leaving, "quédate"), PromptAnswer::kDecline);
  EXPECT_EQ(Answer(leaving, "me quedo"), PromptAnswer::kDecline);
  EXPECT_EQ(ReplySpeech(leaving, ParsePromptReply(leaving, "sal")),
            "Saliendo.");
  EXPECT_EQ(ReplySpeech(leaving, ParsePromptReply(leaving, "quédate")),
            "Me quedo en la página.");
}

TEST(SpanishPromptsTest, DownloadsPasswordsAndSites) {
  BrowserPrompt file = Prompt(PromptKind::kDangerousDownload, kEs,
                              "ejemplo.com", "instalar.exe");
  EXPECT_EQ(PromptSpeech(file, "tomate"),
            "El archivo instalar.exe de ejemplo.com puede ser dañino. Para "
            "conservarlo, di tomate. Para borrarlo, di descarta.");
  EXPECT_EQ(PromptSpeech(file, ""),
            "El archivo instalar.exe de ejemplo.com puede ser dañino. Para "
            "borrarlo, di descarta. Para conservarlo, usa la página de "
            "descargas.");
  file.malicious = true;
  file.text.clear();
  EXPECT_EQ(PromptSpeech(file, ""),
            "Un archivo de ejemplo.com es peligroso, así que se ha "
            "bloqueado. Para borrarlo, di descarta. Para conservarlo de todos "
            "modos, usa la página de descargas.");
  PendingPrompt keeping =
      Waiting(PromptKind::kDangerousDownload, kEs, "tomate");
  EXPECT_EQ(Answer(keeping, "tomate"), PromptAnswer::kAllow);
  EXPECT_EQ(Answer(keeping, "descártalo"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(keeping, "bórralo"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(keeping, "consérvalo"), PromptAnswer::kNeedWord);
  EXPECT_EQ(ReplySpeech(keeping, ParsePromptReply(keeping, "tomate")),
            "He conservado el archivo.");
  EXPECT_EQ(ReplySpeech(keeping, ParsePromptReply(keeping, "manzana")),
            "Esa no era la palabra, así que no he conservado el archivo.");
  EXPECT_EQ(ReplySpeech(keeping, ParsePromptReply(keeping, "descarta")),
            "He borrado el archivo.");
  keeping.malicious = true;
  keeping.word.clear();
  EXPECT_EQ(ReplySpeech(keeping, ParsePromptReply(keeping, "consérvalo")),
            "No puedo conservar un archivo peligroso por voz. Usa la página "
            "de descargas, o di descarta.");
  EXPECT_EQ(PromptTimeoutSpeech(PromptKind::kDangerousDownload, kEs),
            "No has respondido, así que el archivo sigue bloqueado. Puedes "
            "decidir en la página de descargas.");

  const BrowserPrompt password =
      Prompt(PromptKind::kSavePassword, kEs, "ejemplo.com");
  EXPECT_EQ(PromptSpeech(password, ""),
            "¿Guardar tu contraseña de ejemplo.com? Di guarda, nunca o ahora "
            "no.");
  const PendingPrompt saving = Waiting(PromptKind::kSavePassword, kEs);
  EXPECT_EQ(Answer(saving, "guarda"), PromptAnswer::kAccept);
  EXPECT_EQ(Answer(saving, "nunca"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(saving, "ahora no"), PromptAnswer::kDecline);
  EXPECT_EQ(ReplySpeech(saving, ParsePromptReply(saving, "nunca")),
            "No volveré a ofrecerte guardar contraseñas de este sitio.");
  EXPECT_EQ(ReplySpeech(saving, ParsePromptReply(saving, "guarda")),
            "Guardado.");
  EXPECT_EQ(ReplySpeech(saving, ParsePromptReply(saving, "luego")),
            "Ahora no.");

  BrowserPrompt sites = Prompt(PromptKind::kConfirmSites, kEs, "",
                               "a.example y b.example");
  EXPECT_EQ(PromptSpeech(sites, "caracol"),
            "¿Confirmas los sitios que usa esta rutina: a.example y "
            "b.example? Para confirmarlos, di caracol. Para dejarlos sin "
            "confirmar, di no.");
  sites.screen_shared = true;
  EXPECT_EQ(PromptSpeech(sites, ""),
            "¿Confirmas los sitios que usa esta rutina: a.example y "
            "b.example? No puedo aceptar la palabra mientras compartes la "
            "pantalla. Confírmalos en el estudio del modo agente, o di no.");
  const PendingPrompt confirming =
      Waiting(PromptKind::kConfirmSites, kEs, "caracol");
  EXPECT_EQ(Answer(confirming, "caracol"), PromptAnswer::kAllow);
  EXPECT_EQ(Answer(confirming, "no"), PromptAnswer::kDecline);
  EXPECT_EQ(Answer(confirming, "sí"), PromptAnswer::kNeedWord);
  EXPECT_EQ(ReplySpeech(confirming, ParsePromptReply(confirming, "caracol")),
            "Confirmado.");
  EXPECT_EQ(ReplySpeech(confirming, ParsePromptReply(confirming, "no")),
            "Sin confirmar.");
}

TEST(SpanishPromptsTest, TheWordsChannelTexts) {
  EXPECT_EQ(WordOnScreen(kEn), kWordOnScreen);
  EXPECT_EQ(WordOnScreen(kEs), "la palabra que ves en la pantalla");
  EXPECT_EQ(NotCleanSpeech("manzana", kEs),
            "No he podido asegurarme de que fueras tú. Cuando haya silencio y "
            "yo haya terminado de hablar, di manzana.");
  EXPECT_EQ(WordRetiredPreface(WordRetired::kAudioCaptured, kEs),
            "Algo ha empezado a usar un micrófono, así que he cambiado la "
            "palabra.");
  EXPECT_EQ(WordRetiredPreface(WordRetired::kScreenFreed, kEs),
            "Ya no compartes la pantalla.");
  EXPECT_EQ(WordRetiredPreface(WordRetired::kScreenShared, kEs), "");
  EXPECT_EQ(RetiredWordSpeech(PromptKind::kPermission, "tomate", kEs),
            "Esa palabra ha cambiado. Para permitirlo, di tomate.");
  EXPECT_EQ(RetiredWordSpeech(PromptKind::kDangerousDownload,
                              WordOnScreen(kEs), kEs),
            "Esa palabra ha cambiado. Para conservarlo, di la palabra que "
            "ves en la pantalla.");
  // Each text with the word has its twin without it, in Spanish too.
  BrowserPrompt prompt = Prompt(PromptKind::kPermission, kEs, "ejemplo.com");
  const std::string said = PromptSpeech(prompt, "manzana");
  const std::string twin = PromptSpeech(prompt, WordOnScreen(kEs));
  EXPECT_EQ(said.find("la palabra que ves"), std::string::npos);
  EXPECT_NE(twin.find("la palabra que ves"), std::string::npos);
  EXPECT_EQ(twin.find("manzana"), std::string::npos);
}

// The Spanish word said in passing: inside a sentence, or unsure.
TEST(SpanishPromptsTest, TheWordSaidInPassing) {
  const PendingPrompt prompt =
      Waiting(PromptKind::kPermission, kEs, "manzana");
  for (std::string_view heard :
       {"la palabra es manzana", "he dicho manzana", "creo que era manzanas",
        "dije man zana, ¿no?"}) {
    EXPECT_TRUE(WordSaidInPassing(prompt, heard)) << heard;
  }
  for (std::string_view heard :
       {"manzana", "di la manzana por favor", "tomate", "no", "abre youtube",
        "la palabra es tomate"}) {
    EXPECT_FALSE(WordSaidInPassing(prompt, heard)) << heard;
  }
  // Heard in the prompt's language: English's "harbor" holds nothing.
  EXPECT_FALSE(HoldsConfirmationWord("the word is harbor", "manzana", kEs));
  EXPECT_EQ(WordRetiredPreface(WordRetired::kSaidAloud, kEs),
            "Puede que otra cosa te haya oído decir la palabra, así que la he "
            "cambiado.");
}

// English is exactly as it was, whatever Spanish has: every kind and
// answer, said in English, is the catalog's key itself.
TEST(SpanishPromptsTest, EnglishIsUnchanged) {
  BrowserPrompt prompt = Prompt(PromptKind::kPermission, kEn, "example.com");
  prompt.what = "use your camera";
  EXPECT_EQ(PromptSpeech(prompt, "harbor"),
            "example.com wants to use your camera. To allow it, say harbor. "
            "To block it, say no.");
  prompt.what = "usar tu cámara";
  // English keeps ASCII letters only, as before.
  EXPECT_EQ(PromptSpeech(prompt, "").rfind("example.com wants to usar tu c "
                                           "mara.",
                                           0),
            0u);
  EXPECT_EQ(PromptTimeoutSpeech(PromptKind::kPermission),
            PromptTimeoutSpeech(PromptKind::kPermission, kEn));
  EXPECT_EQ(NotCleanSpeech("harbor"), NotCleanSpeech("harbor", kEn));
  EXPECT_EQ(RetiredWordSpeech(PromptKind::kConfirmSites, "harbor"),
            "That word changed. To confirm them, say harbor.");
  const PendingPrompt waiting = Waiting(PromptKind::kPermission, kEn, "harbor");
  EXPECT_EQ(Answer(waiting, "bloquea"), PromptAnswer::kNone);
  EXPECT_EQ(Answer(waiting, "block"), PromptAnswer::kRefuse);
  EXPECT_EQ(Answer(waiting, "manzana"), PromptAnswer::kNone);
}

// Every sentence each kind says has its Spanish: in Spanish no prompt says
// a catalog key (an English sentence) as it is.
TEST(SpanishPromptsTest, EveryKindAndAnswerIsSaidInSpanish) {
  const std::vector<PromptKind> kinds = {
      PromptKind::kPermission,        PromptKind::kAlert,
      PromptKind::kConfirm,           PromptKind::kTextPrompt,
      PromptKind::kBeforeUnload,      PromptKind::kDangerousDownload,
      PromptKind::kSavePassword,      PromptKind::kConfirmSites};
  const std::vector<PromptAnswer> answers = {
      PromptAnswer::kAllow,     PromptAnswer::kWrongWord,
      PromptAnswer::kRefuse,    PromptAnswer::kAccept,
      PromptAnswer::kDecline,   PromptAnswer::kDictate,
      PromptAnswer::kNeedWord,  PromptAnswer::kSayAgain,
      PromptAnswer::kCantAllow};
  for (PromptKind kind : kinds) {
    for (bool flood : {false, true}) {
      for (bool shared : {false, true}) {
        for (std::string_view word : {"", "manzana"}) {
          BrowserPrompt es =
              Prompt(kind, kEs, "ejemplo.com", flood ? "" : "texto");
          es.flood = flood;
          es.screen_shared = shared;
          es.malicious = flood;
          BrowserPrompt en = es;
          en.language = kEn;
          const std::string spanish = PromptSpeech(es, word);
          const std::string english = PromptSpeech(en, word);
          EXPECT_FALSE(spanish.empty());
          EXPECT_NE(spanish, english) << PromptKindName(kind);
        }
      }
    }
    for (PromptAnswer answer : answers) {
      for (bool shared : {false, true}) {
        PendingPrompt es = Waiting(kind, kEs, shared ? "" : "manzana");
        es.screen_shared = shared;
        PendingPrompt en = es;
        en.language = kEn;
        PromptReply reply;
        reply.answer = answer;
        reply.text = "Ada";
        const std::string spanish = ReplySpeech(es, reply);
        const std::string english = ReplySpeech(en, reply);
        // "No." is "No." in both.
        if (english != "No.") {
          EXPECT_NE(spanish, english)
              << PromptKindName(kind) << " " << PromptAnswerName(answer);
        }
      }
    }
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
