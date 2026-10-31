// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode in Spanish: the numbers and positions, the Spanish phonetic
// key, the grammar and its reference, and the confirmation words. English
// keeps its own suites (spoken_numbers_unittest.cc, voice_command_unittest.cc
// and the others), unchanged.

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/confirmation_words_es.h"
#include "chrome/browser/molt_ai/voice/phonetic_key.h"
#include "chrome/browser/molt_ai/voice/phonetic_key_es.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_numbers.h"
#include "chrome/browser/molt_ai/voice/spoken_phrases.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_command.h"
#include "chrome/browser/molt_ai/voice/voice_command_phrases.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

const LanguagePhrases& Spanish() {
  return PhrasesFor(Language::kSpanish);
}

WordList Words(const NormalizedText& text) {
  WordList words;
  for (size_t i = 0; i < text.words.size(); ++i) {
    words.push_back(text.Word(i));
  }
  return words;
}

struct Read {
  int64_t value = -1;
  size_t words = 0;
  bool ordinal = false;
};

Read Number(std::string_view said, bool homophones = false) {
  const NormalizedText text = NormalizeSpokenText(said, {});
  NumberOptions options;
  options.homophones = homophones;
  const std::optional<SpokenNumber> number =
      ReadNumber(Words(text), 0, Spanish(), options);
  if (!number) {
    return Read();
  }
  return Read{number->value, number->words, number->ordinal};
}

TEST(SpanishLanguageTest, Tags) {
  EXPECT_EQ(LanguageFromTag("es"), Language::kSpanish);
  EXPECT_EQ(LanguageFromTag("es-419"), Language::kSpanish);
  EXPECT_EQ(LanguageFromTag("ES_mx"), Language::kSpanish);
  EXPECT_EQ(LanguageFromTag("en-GB"), Language::kEnglish);
  EXPECT_FALSE(LanguageFromTag("eu"));
  EXPECT_FALSE(LanguageFromTag("est"));
  EXPECT_EQ(LanguageCode(Language::kSpanish), "es");
  EXPECT_EQ(LanguageCode(Language::kEnglish), "en");
  EXPECT_EQ(PhrasesFor(Language::kSpanish).language, Language::kSpanish);
  EXPECT_EQ(PhrasesFor(Language::kEnglish).language, Language::kEnglish);
}

TEST(SpanishNumbersTest, Cardinals) {
  const struct {
    std::string_view said;
    int64_t value;
    size_t words;
  } kCases[] = {
      {"cero", 0, 1},
      {"uno", 1, 1},
      {"cuatro", 4, 1},
      {"quince", 15, 1},
      {"dieciséis", 16, 1},
      {"veinte", 20, 1},
      {"veintiuno", 21, 1},
      {"veintiún", 21, 1},
      {"veintitrés", 23, 1},
      {"treinta", 30, 1},
      {"treinta y uno", 31, 3},
      {"noventa y nueve", 99, 3},
      {"cien", 100, 1},
      {"ciento uno", 101, 2},
      {"ciento treinta y dos", 132, 4},
      {"doscientos", 200, 1},
      {"doscientas veinte", 220, 2},
      {"quinientos cincuenta y cinco", 555, 4},
      {"mil", 1000, 1},
      {"mil uno", 1001, 2},
      {"dos mil", 2000, 2},
      {"mil novecientos ochenta y cinco", 1985, 5},
      {"dos mil veinticuatro", 2024, 3},
      {"trescientos mil", 300000, 2},
      {"novecientos noventa y nueve mil novecientos noventa y nueve", 999999,
       9},
      {"12", 12, 1},
  };
  for (const auto& c : kCases) {
    const Read read = Number(c.said);
    EXPECT_EQ(read.value, c.value) << c.said;
    EXPECT_EQ(read.words, c.words) << c.said;
    EXPECT_FALSE(read.ordinal) << c.said;
  }
}

TEST(SpanishNumbersTest, WhereANumberStops) {
  // "y" joins tens to a unit only.
  EXPECT_EQ(Number("treinta y").value, 30);
  EXPECT_EQ(Number("treinta y").words, 1u);
  EXPECT_EQ(Number("treinta y veinte").words, 1u);
  EXPECT_EQ(Number("veinte y uno").value, 21);
  // The twenties are one word: no unit follows them.
  EXPECT_EQ(Number("veintiuno dos").value, 21);
  EXPECT_EQ(Number("veintiuno dos").words, 1u);
  // Whole hundreds come first, or after the thousands.
  EXPECT_EQ(Number("cuatro doscientos").value, 4);
  EXPECT_EQ(Number("doscientos trescientos").words, 1u);
  EXPECT_EQ(Number("cien").value, 100);
  // Not numbers.
  EXPECT_EQ(Number("y").value, -1);
  EXPECT_EQ(Number("una canción").value, -1);
}

TEST(SpanishNumbersTest, Ordinals) {
  const struct {
    std::string_view said;
    int64_t value;
    size_t words;
  } kCases[] = {
      {"primero", 1, 1},
      {"primera", 1, 1},
      {"primer", 1, 1},
      {"segundo", 2, 1},
      {"segunda", 2, 1},
      {"tercer", 3, 1},
      {"tercera", 3, 1},
      {"cuarto", 4, 1},
      {"cuarta", 4, 1},
      {"séptimo", 7, 1},
      {"novena", 9, 1},
      {"décimo", 10, 1},
      {"undécima", 11, 1},
      {"decimotercero", 13, 1},
      {"décimo tercero", 13, 2},
      {"vigésimo", 20, 1},
      {"vigésimo primero", 21, 2},
      {"vigésima primera", 21, 2},
      {"centésimo", 100, 1},
      {"4º", 4, 1},
      {"4ª", 4, 1},
      {"1er", 1, 1},
      {"3ra", 3, 1},
      {"2do", 2, 1},
      {"7mo", 7, 1},
  };
  for (const auto& c : kCases) {
    const Read read = Number(c.said);
    EXPECT_EQ(read.value, c.value) << c.said;
    EXPECT_EQ(read.words, c.words) << c.said;
    EXPECT_TRUE(read.ordinal) << c.said;
  }
}

TEST(SpanishNumbersTest, Positions) {
  const struct {
    std::string_view said;
    int index;
    bool from_end;
    size_t words;
  } kCases[] = {
      {"cuarto", 4, false, 1},
      {"última", 1, true, 1},
      {"último", 1, true, 1},
      {"penúltimo", 2, true, 1},
      {"penúltima", 2, true, 1},
      {"antepenúltimo", 3, true, 1},
      {"segundo desde el final", 2, true, 4},
      {"tercero empezando por el final", 3, true, 5},
      {"mismísimo último", 1, true, 2},
  };
  for (const auto& c : kCases) {
    const NormalizedText text = NormalizeSpokenText(c.said, {});
    const std::optional<SpokenPosition> position =
        ReadPosition(Words(text), 0, Spanish(), false);
    ASSERT_TRUE(position) << c.said;
    EXPECT_EQ(position->index, c.index) << c.said;
    EXPECT_EQ(position->from_end, c.from_end) << c.said;
    EXPECT_EQ(position->words, c.words) << c.said;
  }
}

TEST(SpanishNumbersTest, Durations) {
  const struct {
    std::string_view said;
    int seconds;
  } kCases[] = {
      {"30 segundos", 30},
      {"un minuto", 60},
      {"un segundo", 1},
      {"una hora", 3600},
      {"medio minuto", 30},
      {"media hora", 1800},
      {"un minuto y medio", 90},
      {"una hora y media", 5400},
      {"dos minutos y treinta segundos", 150},
      {"dos minutos treinta segundos", 150},
      {"treinta y cinco segundos", 35},
      {"90 segs", 90},
  };
  for (const auto& c : kCases) {
    const NormalizedText text = NormalizeSpokenText(c.said, {});
    const WordList words = Words(text);
    const std::optional<SpokenDuration> duration =
        ReadDuration(words, 0, Spanish());
    ASSERT_TRUE(duration) << c.said;
    EXPECT_EQ(duration->seconds, c.seconds) << c.said;
    EXPECT_EQ(duration->words, words.size()) << c.said;
  }
  // Years are cardinals in Spanish: no two-part years.
  const NormalizedText year = NormalizeSpokenText("diecinueve ochenta", {});
  EXPECT_FALSE(ReadYear(Words(year), 0, Spanish()));
}

// The fields Spanish adds are empty or off in English: its readings are
// the ones its own suites check.
TEST(SpanishNumbersTest, EnglishTablesHaveNoneOfIt) {
  const LanguagePhrases& english = PhrasesFor(Language::kEnglish);
  EXPECT_TRUE(english.tens_unit_and.empty());
  EXPECT_TRUE(english.third_to_last_phrases.empty());
  EXPECT_TRUE(english.and_half_phrases.empty());
  EXPECT_FALSE(english.half_before_unit);
  EXPECT_FALSE(english.compound_ordinals);
  EXPECT_TRUE(english.years_in_two_parts);
  EXPECT_FALSE(english.bare_number_after_article);
  EXPECT_EQ(english.wake_sound_margin, 0);
  for (const NumberWord& word : english.numbers) {
    EXPECT_NE(word.kind, NumberWordKind::kHundreds) << word.word;
  }
}

TEST(SpanishPhraseTablesTest, SortedAndFolded) {
  const LanguagePhrases& p = Spanish();
  EXPECT_TRUE(std::is_sorted(p.numbers.begin(), p.numbers.end(),
                             [](const NumberWord& a, const NumberWord& b) {
                               return a.word < b.word;
                             }));
  for (size_t i = 1; i < p.numbers.size(); ++i) {
    EXPECT_NE(p.numbers[i - 1].word, p.numbers[i].word);
  }
  EXPECT_TRUE(
      std::is_sorted(p.choice_stopwords.begin(), p.choice_stopwords.end()));
  EXPECT_TRUE(
      std::is_sorted(p.choice_particles.begin(), p.choice_particles.end()));
  EXPECT_TRUE(std::is_sorted(
      p.role_words.begin(), p.role_words.end(),
      [](const RoleWord& a, const RoleWord& b) { return a.word < b.word; }));
  const auto folded = [](std::string_view phrase) {
    return FoldForComparison(phrase) == phrase;
  };
  for (const NumberWord& word : p.numbers) {
    EXPECT_TRUE(folded(word.word)) << word.word;
  }
  for (const auto* list :
       {&p.fillers, &p.leading_phrases, &p.trailing_phrases, &p.wake_greetings,
        &p.articles, &p.current_phrases, &p.next_phrases, &p.previous_phrases,
        &p.by_cues, &p.from_cues, &p.end_phrases, &p.site_prefixes,
        &p.site_suffixes, &p.scroll_half, &p.scroll_page}) {
    for (std::string_view phrase : *list) {
      EXPECT_TRUE(folded(phrase)) << phrase;
    }
  }
}

// --- The Spanish phonetic key ----------------------------------------------

TEST(SpanishPhoneticKeyTest, SpanishSounds) {
  // A final e is said.
  EXPECT_EQ(SpanishPhoneticKey("molde"), "MaLTa");
  EXPECT_EQ(SpanishPhoneticKey("molt"), "MaLT");
  EXPECT_EQ(PhoneticKey("molde"), PhoneticKey("molt"));  // English's
  // h is silent; ch is one sound.
  EXPECT_EQ(SpanishPhoneticKey("hola"), SpanishPhoneticKey("ola"));
  EXPECT_EQ(SpanishPhoneticKey("chocolate"), "XaKaLaTa");
  // qu and gu before e or i.
  EXPECT_EQ(SpanishPhoneticKey("queso"), "KaSa");
  EXPECT_EQ(SpanishPhoneticKey("guerra"), "KaRa");
  // j, and g before e or i, are one sound; so are ll and y; b and v.
  EXPECT_EQ(SpanishPhoneticKey("gente"), SpanishPhoneticKey("jente"));
  EXPECT_EQ(SpanishPhoneticKey("llave"), SpanishPhoneticKey("yave"));
  EXPECT_EQ(SpanishPhoneticKey("vaca"), SpanishPhoneticKey("baca"));
  // c and z before e or i are s (as in Latin America).
  EXPECT_EQ(SpanishPhoneticKey("cereza"), SpanishPhoneticKey("sereza"));
  EXPECT_EQ(SpanishPhoneticKey("huevo"), "WaPa");
  EXPECT_EQ(SpanishPhoneticKey("hoy"), "a");
  EXPECT_EQ(SpanishPhoneticKey("xilofono"), "SaLaPaNa");
  EXPECT_EQ(SpanishPhoneticKey("taxi"), "TaKSa");
  // Like the English key: lower-case ASCII only, bounded.
  EXPECT_EQ(SpanishPhoneticKey(""), "");
  EXPECT_EQ(SpanishPhoneticKey("Hola"), "");
  EXPECT_EQ(SpanishPhoneticKey("4k"), "");
  EXPECT_EQ(SpanishPhoneticKey(std::string(kMaxPhoneticWordBytes + 1, 'a')),
            "");
  EXPECT_EQ(PhoneticKeyFor(Language::kSpanish, "molde"), "MaLTa");
  EXPECT_EQ(PhoneticKeyFor(Language::kEnglish, "molde"), PhoneticKey("molde"));
}

// The wake phrase's name: a Spanish recognizer may write "Molt", which
// isn't a Spanish word, as "mol", "molde" or "molts"; within the margin
// Spanish gives the wake phrase's name (spoken_phrases_es.cc) all of them
// are taken for it, and words of other sounds aren't.
TEST(SpanishPhoneticKeyTest, TheWakePhrasesName) {
  const std::string molt = SpanishPhoneticKey("molt");
  const int margin = Spanish().wake_sound_margin;
  EXPECT_EQ(margin, 2);
  for (std::string_view near : {"mol", "molde", "molth", "molts", "malt"}) {
    EXPECT_LE(PhoneticDistance(molt, SpanishPhoneticKey(near), margin), margin)
        << near;
  }
  for (std::string_view far :
       {"mole", "molino", "mota", "volt", "hola", "abre", "sol"}) {
    EXPECT_GT(PhoneticDistance(molt, SpanishPhoneticKey(far), margin), margin)
        << far;
  }
}

// --- The grammar -----------------------------------------------------------

enum Ctx { kNone, kYesNo, kList, kField };

ParseContext Context(Ctx ctx) {
  ParseContext context;
  context.language = Language::kSpanish;
  context.expecting_yes_no = ctx == kYesNo;
  context.expecting_choice = ctx == kList;
  context.text_field_focused = ctx == kField;
  return context;
}

VoiceCommand Parse(std::string_view said, Ctx ctx = kNone) {
  return ParseVoiceCommand(said, Context(ctx));
}

std::string_view Kind(std::string_view said, Ctx ctx = kNone) {
  return VoiceCommandKindName(Parse(said, ctx).kind);
}

TEST(SpanishGrammarTest, GoingPlaces) {
  VoiceCommand c = Parse("Oye Molt, abre YouTube");
  EXPECT_EQ(c.kind, VoiceCommandKind::kOpenSite);
  EXPECT_EQ(c.site, "youtube");
  EXPECT_EQ(c.heard, "abre YouTube");
  c = Parse("hola molt ¿puedes abrir la web de Wikipedia, por favor?");
  EXPECT_EQ(c.kind, VoiceCommandKind::kOpenSite);
  EXPECT_EQ(c.site, "wikipedia");
  c = Parse("ve a ejemplo punto com");
  EXPECT_EQ(c.kind, VoiceCommandKind::kOpenSite);
  EXPECT_EQ(c.site, "ejemplo.com");
  EXPECT_TRUE(c.site_is_address);
  c = Parse("abre YouTube en una pestaña nueva");
  EXPECT_EQ(c.kind, VoiceCommandKind::kOpenSite);
  EXPECT_TRUE(c.new_tab);
  c = Parse("busca Bohemian Rhapsody");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSearch);
  EXPECT_EQ(c.text, "Bohemian Rhapsody");
  EXPECT_EQ(c.scope, SearchScope::kAuto);
  c = Parse("busca Bohemian Rhapsody en YouTube");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSearch);
  EXPECT_EQ(c.scope, SearchScope::kNamedSite);
  EXPECT_EQ(c.site, "youtube");
  EXPECT_EQ(c.text, "Bohemian Rhapsody");
  c = Parse("en YouTube busca gatos");
  EXPECT_EQ(c.scope, SearchScope::kNamedSite);
  EXPECT_EQ(c.text, "gatos");
  c = Parse("busca en internet el tiempo en Madrid");
  EXPECT_EQ(c.scope, SearchScope::kDefaultEngine);
  EXPECT_EQ(c.text, "el tiempo en Madrid");
  c = Parse("busca recetas en esta web");
  EXPECT_EQ(c.scope, SearchScope::kCurrentSite);
  c = Parse("busca horario en esta página");
  EXPECT_EQ(c.kind, VoiceCommandKind::kFindInPage);
  EXPECT_EQ(c.text, "horario");
  EXPECT_EQ(Kind("atrás"), "back");
  EXPECT_EQ(Kind("vuelve atrás"), "back");
  EXPECT_EQ(Kind("adelante"), "forward");
  EXPECT_EQ(Kind("recarga la página"), "reload");
  EXPECT_EQ(Kind("ve a inicio"), "home");
  EXPECT_EQ(Kind("página siguiente"), "next_page");
  EXPECT_EQ(Kind("abre una pestaña nueva"), "new_tab");
  EXPECT_EQ(Kind("cierra esta pestaña"), "close_tab");
  c = Parse("cierra la segunda pestaña");
  EXPECT_EQ(c.kind, VoiceCommandKind::kCloseTab);
  EXPECT_EQ(c.choice.ordinal, 2);
  c = Parse("cambia a la pestaña 3");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSwitchTab);
  EXPECT_EQ(c.choice.number, 3);
  EXPECT_EQ(Kind("siguiente pestaña"), "switch_tab");
}

TEST(SpanishGrammarTest, ViewingAndMedia) {
  EXPECT_EQ(Kind("baja"), "scroll_down");
  VoiceCommand c = Parse("baja dos páginas");
  EXPECT_EQ(c.kind, VoiceCommandKind::kScrollDown);
  EXPECT_EQ(c.scroll, ScrollAmount::kPage);
  EXPECT_EQ(c.count, 2);
  c = Parse("sube un poco");
  EXPECT_EQ(c.kind, VoiceCommandKind::kScrollUp);
  EXPECT_EQ(c.scroll, ScrollAmount::kHalfPage);
  EXPECT_EQ(Kind("ve al principio"), "scroll_to_top");
  EXPECT_EQ(Kind("ve al final de la página"), "scroll_to_bottom");
  EXPECT_EQ(Kind("acerca"), "zoom_in");
  EXPECT_EQ(Kind("aleja"), "zoom_out");
  EXPECT_EQ(Kind("tamaño normal"), "zoom_reset");
  EXPECT_EQ(Kind("reproduce"), "play");
  EXPECT_EQ(Kind("pausa"), "pause");
  EXPECT_EQ(Kind("para el vídeo"), "pause");
  EXPECT_EQ(Kind("reanuda"), "resume");
  EXPECT_EQ(Kind("sube el volumen"), "volume_up");
  EXPECT_EQ(Kind("más alto"), "volume_up");
  EXPECT_EQ(Kind("baja el volumen"), "volume_down");
  c = Parse("pon el volumen al máximo");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSetVolume);
  EXPECT_EQ(c.level, 100);
  c = Parse("pon el volumen a 5");
  EXPECT_EQ(c.level, 50);
  c = Parse("pon el volumen al treinta por ciento");
  EXPECT_EQ(c.level, 30);
  c = Parse("volumen al 40%");
  EXPECT_EQ(c.level, 40);
  EXPECT_EQ(Kind("silencia"), "mute");
  EXPECT_EQ(Kind("quita el silencio"), "unmute");
  c = Parse("adelanta un minuto y medio");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSkipForward);
  EXPECT_EQ(c.seconds, 90);
  c = Parse("retrocede 10 segundos");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSkipBack);
  EXPECT_EQ(c.seconds, 10);
  c = Parse("adelanta");
  EXPECT_EQ(c.kind, VoiceCommandKind::kSkipForward);
  EXPECT_EQ(c.seconds, 0);
  EXPECT_EQ(Kind("empieza de nuevo"), "restart");
  EXPECT_EQ(Kind("siguiente canción"), "next_track");
  EXPECT_EQ(Kind("vídeo anterior"), "previous_track");
  EXPECT_EQ(Kind("pantalla completa"), "full_screen");
  EXPECT_EQ(Kind("sal de la pantalla completa"), "exit_full_screen");
  EXPECT_EQ(Kind("activa los subtítulos"), "captions_on");
  EXPECT_EQ(Kind("quita los subtítulos"), "captions_off");
  EXPECT_EQ(Kind("salta el anuncio"), "skip_ad");
  EXPECT_EQ(Kind("acelera"), "speed_up");
  EXPECT_EQ(Kind("velocidad normal"), "normal_speed");
}

TEST(SpanishGrammarTest, ReadingAndQuestions) {
  EXPECT_EQ(Kind("lee la página"), "read_page");
  EXPECT_EQ(Kind("léeme el artículo"), "read_page");
  EXPECT_EQ(Kind("sigue leyendo"), "read_from_here");
  EXPECT_EQ(Kind("lee los títulos"), "read_headings");
  EXPECT_EQ(Kind("lee el título"), "read_title");
  EXPECT_EQ(Kind("lee los enlaces"), "read_links");
  EXPECT_EQ(Kind("lee los resultados"), "read_results");
  EXPECT_EQ(Kind("qué hay en la pantalla"), "describe_screen");
  EXPECT_EQ(Kind("siguiente título"), "next_heading");
  EXPECT_EQ(Kind("la sección anterior"), "unknown");
  EXPECT_EQ(Kind("ve a la sección anterior"), "previous_heading");
  EXPECT_EQ(Kind("siguiente párrafo"), "next_paragraph");
  EXPECT_EQ(Kind("siguiente"), "next_item");
  EXPECT_EQ(Kind("anterior"), "previous_item");
  VoiceCommand c = Parse("lee la sección sobre historia");
  EXPECT_EQ(c.kind, VoiceCommandKind::kReadSection);
  EXPECT_EQ(c.text, "historia");
  c = Parse("¿de qué trata esta página?");
  EXPECT_EQ(c.kind, VoiceCommandKind::kAskPage);
  EXPECT_EQ(c.page_question, PageQuestionKind::kAbout);
  c = Parse("¿qué dice sobre la batería?");
  EXPECT_EQ(c.kind, VoiceCommandKind::kAskPage);
  EXPECT_EQ(c.page_question, PageQuestionKind::kTopic);
  EXPECT_EQ(c.text, "la batería");
  c = Parse("dime los puntos principales");
  EXPECT_EQ(c.page_question, PageQuestionKind::kMainPoints);
  c = Parse("¿quién escribió esto?");
  EXPECT_EQ(c.page_question, PageQuestionKind::kQuestion);
  c = Parse("¿cuánto cuesta?");
  EXPECT_EQ(c.page_question, PageQuestionKind::kQuestion);
  c = Parse("cuéntame más");
  EXPECT_EQ(c.page_question, PageQuestionKind::kMore);
  c = Parse("resúmelo");
  EXPECT_EQ(c.kind, VoiceCommandKind::kUnknown);
  c = Parse("resume esta página");
  EXPECT_EQ(c.page_question, PageQuestionKind::kAbout);
}

TEST(SpanishGrammarTest, Choosing) {
  VoiceCommand c = Parse("reproduce el cuarto");
  EXPECT_EQ(c.kind, VoiceCommandKind::kChoose);
  EXPECT_EQ(c.verb, ChoiceVerb::kPlay);
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_EQ(c.choice.ordinal, 4);
  c = Parse("la cuarta");
  EXPECT_EQ(c.kind, VoiceCommandKind::kChoose);
  EXPECT_EQ(c.choice.ordinal, 4);
  c = Parse("el último vídeo");
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kPosition);
  EXPECT_TRUE(c.choice.from_end);
  EXPECT_EQ(c.choice.ordinal, 1);
  EXPECT_EQ(c.choice.roles, RoleBit(ChoiceRole::kVideo));
  c = Parse("el penúltimo");
  EXPECT_TRUE(c.choice.from_end);
  EXPECT_EQ(c.choice.ordinal, 2);
  c = Parse("abre el número doce");
  EXPECT_EQ(c.verb, ChoiceVerb::kOpen);
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kNumber);
  EXPECT_EQ(c.choice.number, 12);
  // Right after a list, a bare number, with or without its article.
  c = Parse("cuatro", kList);
  EXPECT_EQ(c.kind, VoiceCommandKind::kChoose);
  EXPECT_EQ(c.choice.number, 4);
  c = Parse("el cuatro", kList);
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kNumber);
  EXPECT_EQ(c.choice.number, 4);
  c = Parse("el veintiuno", kList);
  EXPECT_EQ(c.choice.number, 21);
  c = Parse("pon el de Queen en directo");
  EXPECT_EQ(c.kind, VoiceCommandKind::kChoose);
  EXPECT_EQ(c.verb, ChoiceVerb::kPlay);
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kDescription);
  ASSERT_EQ(c.choice.by_words.size(), 3u);
  EXPECT_EQ(c.choice.by_words[0], "queen");
  c = Parse("el siguiente");
  EXPECT_EQ(c.choice.kind, ChoiceRef::Kind::kRelative);
  EXPECT_EQ(c.choice.relative, ChoiceRef::Relative::kNext);
  c = Parse("haz clic en el botón de enviar");
  EXPECT_EQ(c.verb, ChoiceVerb::kClick);
  EXPECT_EQ(Kind("muestra los números"), "show_numbers");
  EXPECT_EQ(Kind("oculta los números"), "hide_numbers");
}

TEST(SpanishGrammarTest, ConversationAndDictation) {
  EXPECT_EQ(Kind("sí", kYesNo), "yes");
  EXPECT_EQ(Kind("sí, hazlo", kYesNo), "yes");
  EXPECT_EQ(Kind("vale", kYesNo), "yes");
  EXPECT_EQ(Kind("no", kYesNo), "no");
  EXPECT_EQ(Kind("no, gracias", kYesNo), "no");
  // Yes and no only answer a question.
  EXPECT_EQ(Kind("sí"), "unknown");
  EXPECT_EQ(Kind("cancela"), "cancel");
  EXPECT_EQ(Kind("para"), "stop");
  EXPECT_EQ(Kind("deja de hablar"), "stop_speaking");
  EXPECT_EQ(Kind("cállate"), "stop_speaking");
  EXPECT_EQ(Kind("repite"), "repeat");
  EXPECT_EQ(Kind("¿qué puedo decir?"), "help");
  EXPECT_EQ(Kind("¿dónde estoy?"), "where_am_i");
  EXPECT_EQ(Kind("habla más rápido"), "speak_faster");
  EXPECT_EQ(Kind("sé breve"), "verbosity");
  VoiceCommand c = Parse("no, dije YouTube");
  EXPECT_EQ(c.kind, VoiceCommandKind::kCorrection);
  EXPECT_EQ(c.text, "YouTube");
  c = Parse("no dije eso, dije Queen");
  EXPECT_EQ(c.kind, VoiceCommandKind::kCorrection);
  EXPECT_EQ(c.text, "Queen");
  c = Parse("escribe hola mundo");
  EXPECT_EQ(c.kind, VoiceCommandKind::kType);
  EXPECT_EQ(c.text, "hola mundo");
  EXPECT_EQ(Kind("nueva línea", kField), "new_line");
  EXPECT_EQ(Kind("borra la última palabra", kField), "delete_last_word");
  EXPECT_EQ(Kind("borra el campo", kField), "clear_field");
  EXPECT_EQ(Kind("envía", kField), "submit");
  EXPECT_EQ(Kind("nueva línea"), "unknown");
}

TEST(SpanishGrammarTest, RoutinesAndVoiceMode) {
  VoiceCommand c = Parse("recuerda esto como mis noticias");
  EXPECT_EQ(c.kind, VoiceCommandKind::kRememberRoutine);
  EXPECT_EQ(c.text, "mis noticias");
  c = Parse("ejecuta mis noticias");
  EXPECT_EQ(c.kind, VoiceCommandKind::kRunRoutine);
  EXPECT_EQ(c.text, "mis noticias");
  c = Parse("olvida mis noticias");
  EXPECT_EQ(c.kind, VoiceCommandKind::kForgetRoutine);
  EXPECT_EQ(c.text, "mis noticias");
  c = Parse("olvida la palabra Queen");
  EXPECT_EQ(c.kind, VoiceCommandKind::kForgetWord);
  EXPECT_EQ(c.text, "Queen");
  EXPECT_EQ(Kind("olvida todo lo que aprendiste"), "forget_learning");
  EXPECT_EQ(Kind("¿cuáles son mis rutinas?"), "list_routines");
  EXPECT_EQ(Kind("olvida eso"), "cancel");
  EXPECT_EQ(Kind("desactiva el modo de voz"), "voice_mode_off");
  EXPECT_EQ(Kind("deja de escuchar"), "voice_mode_off");
  EXPECT_EQ(Kind("activa el modo de voz"), "voice_mode_on");
}

TEST(SpanishGrammarTest, TheWakePhrase) {
  for (std::string_view said :
       {"Oye Molt, abre YouTube", "Hola Molt abre YouTube",
        "hey molt abre youtube", "oye mol, abre youtube",
        "ok oye molt abre youtube"}) {
    const VoiceCommand c = Parse(said);
    EXPECT_EQ(c.kind, VoiceCommandKind::kOpenSite) << said;
    EXPECT_EQ(c.site, "youtube") << said;
  }
  // How a Spanish recognizer may spell the name.
  EXPECT_EQ(Parse("oye molde abre youtube").site, "youtube");
  // A wake phrase the user chose.
  ParseContext context = Context(kNone);
  context.wake_phrase = "ordenador";
  EXPECT_EQ(ParseVoiceCommand("ordenador, recarga", context).kind,
            VoiceCommandKind::kReload);
}

TEST(SpanishGrammarTest, EnglishIsNotSpanish) {
  // Each language parses its own words only.
  EXPECT_EQ(Kind("open YouTube"), "unknown");
  ParseContext english;
  EXPECT_EQ(ParseVoiceCommand("abre YouTube", english).kind,
            VoiceCommandKind::kUnknown);
  EXPECT_EQ(ParseVoiceCommand("open YouTube", english).kind,
            VoiceCommandKind::kOpenSite);
}

TEST(SpanishGrammarTest, RefusesRatherThanTruncates) {
  EXPECT_EQ(Parse(std::string(kMaxUtteranceBytes + 1, 'a')).kind,
            VoiceCommandKind::kRefused);
  EXPECT_EQ(Parse("abre \xff YouTube").kind, VoiceCommandKind::kRefused);
  const VoiceCommand c = Parse("busca " + std::string(600, 'a'));
  EXPECT_EQ(c.kind, VoiceCommandKind::kUnknown);
}

TEST(SpanishPhrasesTest, EveryPatternIsWellFormed) {
  for (const CommandPattern& pattern : CommandPatterns(Language::kSpanish)) {
    EXPECT_TRUE(IsValidCommandPattern(pattern.pattern)) << pattern.pattern;
  }
}

TEST(SpanishPhrasesTest, EveryKindIsDocumentedAndItsExampleWorks) {
  std::set<VoiceCommandKind> documented;
  std::vector<VoiceCommandKind> english_order;
  for (const CommandInfo& info : CommandInfos(Language::kEnglish)) {
    english_order.push_back(info.kind);
  }
  std::vector<VoiceCommandKind> spanish_order;
  for (const CommandInfo& info : CommandInfos(Language::kSpanish)) {
    spanish_order.push_back(info.kind);
    EXPECT_TRUE(documented.insert(info.kind).second)
        << VoiceCommandKindName(info.kind);
    EXPECT_FALSE(info.summary.empty());
    ParseContext context = Context(kNone);
    context.expecting_yes_no = info.context == PatternContext::kYesNo;
    context.expecting_choice = info.context == PatternContext::kChoice;
    context.text_field_focused = info.context == PatternContext::kTextField;
    EXPECT_EQ(ParseVoiceCommand(info.example, context).kind, info.kind)
        << info.example;
  }
  // The same commands in the same order as English's reference.
  EXPECT_EQ(spanish_order, english_order);
  std::set<VoiceCommandKind> patterned;
  for (const CommandPattern& pattern : CommandPatterns(Language::kSpanish)) {
    patterned.insert(pattern.kind);
  }
  for (VoiceCommandKind kind : documented) {
    EXPECT_TRUE(patterned.count(kind)) << VoiceCommandKindName(kind);
  }
}

// --- The confirmation words -------------------------------------------------

TEST(SpanishConfirmationWordsTest, FortyWordsAllWithAKey) {
  EXPECT_EQ(SpanishConfirmationWords().size(), 40u);
  std::set<std::string_view> seen;
  for (std::string_view word : SpanishConfirmationWords()) {
    EXPECT_TRUE(seen.insert(word).second) << word;
    EXPECT_FALSE(SpanishPhoneticKey(word).empty()) << word;
    EXPECT_FALSE(PhoneticKey(word).empty()) << word;
    EXPECT_EQ(FoldForComparison(word), word);
  }
}

TEST(SpanishConfirmationWordsTest, NoTwoWordsEitherKeyConfuses) {
  const auto words = SpanishConfirmationWords();
  for (size_t i = 0; i < words.size(); ++i) {
    for (size_t j = i + 1; j < words.size(); ++j) {
      EXPECT_FALSE(SpanishWordsConfusable(words[i], words[j], /*margin=*/1))
          << words[i] << " / " << words[j] << " ("
          << SpanishPhoneticKey(words[i]) << " " << SpanishPhoneticKey(words[j])
          << ")";
    }
  }
}

TEST(SpanishConfirmationWordsTest, NoWordSoundsLikeAnAnswerOrANumber) {
  std::vector<std::string_view> answers(SpanishAnswerWords().begin(),
                                        SpanishAnswerWords().end());
  // Every number word a list is chosen from, and the ordinals.
  for (const NumberWord& number : PhrasesFor(Language::kSpanish).numbers) {
    if (number.value <= 20) {
      answers.push_back(number.word);
    }
  }
  for (std::string_view word : SpanishConfirmationWords()) {
    for (std::string_view answer : answers) {
      EXPECT_FALSE(SpanishWordsConfusable(word, answer, /*margin=*/0))
          << word << " / " << answer;
    }
    for (std::string_view leading : SpanishConfirmationLeading()) {
      EXPECT_FALSE(SpanishWordsConfusable(word, leading, /*margin=*/0))
          << word << " / " << leading;
    }
  }
}

// A plural adds "s" or "es" ("manzanas", "camiones"), or nothing
// ("cactus"), so a matcher takes it off exactly; no plural is another word.
TEST(SpanishConfirmationWordsTest, PluralsAreRegular) {
  const auto words = SpanishConfirmationWords();
  for (std::string_view word : words) {
    const char last = word.back();
    EXPECT_NE(last, 'z') << word;  // "-ces"
    const bool vowel =
        last == 'a' || last == 'e' || last == 'i' || last == 'o' || last == 'u';
    const std::string plural = last == 's'
                                   ? std::string(word)
                                   : std::string(word) + (vowel ? "s" : "es");
    for (std::string_view other : words) {
      if (other != word) {
        EXPECT_NE(plural, other) << word;
      }
    }
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
