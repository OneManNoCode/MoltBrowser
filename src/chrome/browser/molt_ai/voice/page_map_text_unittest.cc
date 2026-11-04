// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/page_map_text.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

TEST(PageMapTextTest, KeepsPlainText) {
  EXPECT_EQ(SpeechSafeText("Bohemian Rhapsody", 100), "Bohemian Rhapsody");
  EXPECT_EQ(SpeechSafeText("Queen \xE2\x80\x93 Live Aid", 100),
            "Queen \xE2\x80\x93 Live Aid");
  EXPECT_EQ(SpeechSafeText("", 100), "");
}

TEST(PageMapTextTest, CollapsesSpacesAndLineBreaks) {
  EXPECT_EQ(SpeechSafeText("  a \t\n b\r\n\fc  ", 100), "a b c");
  // NBSP, ideographic space, narrow NBSP, line and paragraph separators.
  EXPECT_EQ(SpeechSafeText("a\xC2\xA0"
                           "b\xE3\x80\x80"
                           "c\xE2\x80\xAF"
                           "d\xE2\x80\xA8"
                           "e\xE2\x80\xA9"
                           "f",
                           100),
            "a b c d e f");
}

TEST(PageMapTextTest, DropsControlBidiAndInvisibleCharacters) {
  // A right-to-left override would make "txt.exe" read as "exe.txt".
  EXPECT_EQ(SpeechSafeText("file\xE2\x80\xAEtxt.exe", 100), "filetxt.exe");
  // Zero-width characters inside a word, a BOM, a soft hyphen, tag
  // characters spelling hidden text.
  EXPECT_EQ(SpeechSafeText("pay\xE2\x80\x8Bpal\xEF\xBB\xBF\xC2\xAD", 100),
            "paypal");
  EXPECT_EQ(SpeechSafeText("ok\xF3\xA0\x81\x81\xF3\xA0\x81\x82", 100), "ok");
  // C0 and C1 controls and DEL.
  EXPECT_EQ(SpeechSafeText("a\x01"
                           "b\x7F"
                           "c\xC2\x85"
                           "d",
                           100),
            "abcd");
  // Private-use (icon font) characters, noncharacters, U+FFFC, U+FFFD.
  EXPECT_EQ(SpeechSafeText("\xEE\x80\x80Play\xEF\xBF\xBC\xEF\xBF\xBD"
                           "\xEF\xB7\x90",
                           100),
            "Play");
  // A NUL in the middle.
  EXPECT_EQ(SpeechSafeText(std::string("a\0b", 3), 100), "ab");
}

TEST(PageMapTextTest, InvalidUtf8ReadsAsASpace) {
  EXPECT_EQ(SpeechSafeText("ad\xFFmin", 100), "ad min");
  EXPECT_EQ(SpeechSafeText("a\xC0\xAF"
                           "b",
                           100),
            "a b");  // overlong
  EXPECT_EQ(SpeechSafeText("a\xED\xA0\x80"
                           "b",
                           100),
            "a b");  // surrogate
  EXPECT_EQ(SpeechSafeText("a\xF4\x90\x80\x80"
                           "b",
                           100),
            "a b");                                      // > 10FFFF
  EXPECT_EQ(SpeechSafeText("end\xE2\x80", 100), "end");  // cut short
}

TEST(PageMapTextTest, CapsLengthAtAWordBoundary) {
  EXPECT_EQ(SpeechSafeText("one two three four", 9), "one two");
  // No space near the cut: cut inside the word.
  EXPECT_EQ(SpeechSafeText("abcdefghijklmnopqrstuvwxyz", 5), "abcde");
  // Code points, not bytes.
  EXPECT_EQ(SpeechSafeText("\xC3\xA9\xC3\xA9\xC3\xA9", 2), "\xC3\xA9\xC3\xA9");
  EXPECT_EQ(SpeechSafeText("abc", 0), "");
  EXPECT_EQ(CountCodePoints("a\xC3\xA9\xE2\x80\x93"), 3u);
}

TEST(PageMapTextTest, ReadsOnlyAPrefix) {
  std::string huge(kMaxSpeechScanBytes * 4, 'x');
  huge.replace(kMaxSpeechScanBytes + 10, 4, "TAIL");
  const std::string out = SpeechSafeText(huge, huge.size());
  EXPECT_EQ(out.size(), kMaxSpeechScanBytes);
  EXPECT_EQ(out.find("TAIL"), std::string::npos);
}

TEST(PageMapTextTest, CleansTypedText) {
  EXPECT_EQ(TypedText("Bohemian Rhapsody", false, 100), "Bohemian Rhapsody");
  // Line breaks stay only in a multi-line field.
  EXPECT_EQ(TypedText("one\r\ntwo\nthree\rfour", true, 100),
            "one\ntwo\nthree\nfour");
  EXPECT_EQ(TypedText("one\r\ntwo\xE2\x80\xA8three", false, 100),
            "one two three");
  EXPECT_EQ(TypedText("one\xE2\x80\xA9two", true, 100), "one\ntwo");
  // Controls, bidi and invisible characters and invalid bytes go; spaces
  // are kept as said.
  EXPECT_EQ(TypedText("a\tb\x01"
                      "c\xE2\x80\xAE"
                      "d\xFF"
                      "e  f",
                      false, 100),
            "a bcde  f");
  EXPECT_EQ(TypedText("caf\xC3\xA9", false, 100), "caf\xC3\xA9");
  EXPECT_EQ(TypedText("", false, 100), "");
  EXPECT_EQ(TypedText("abcd", false, 4), "abcd");
  EXPECT_FALSE(TypedText("abcde", false, 4).has_value());
  EXPECT_FALSE(TypedText(std::string(100000, 'x'), false, 4096).has_value());
}

TEST(PageMapTextTest, ParsesDurations) {
  EXPECT_EQ(ParseDuration("6:07"), 367);
  EXPECT_EQ(ParseDuration(" 0:59 "), 59);
  EXPECT_EQ(ParseDuration("1:02:03"), 3723);
  EXPECT_EQ(ParseDuration("58:43"), 3523);
  EXPECT_EQ(ParseDuration("6 minutes, 7 seconds"), 367);
  EXPECT_EQ(ParseDuration("1 hour, 2 minutes and 3 seconds"), 3723);
  EXPECT_EQ(ParseDuration("1 hr 2 min"), 3720);
  EXPECT_EQ(ParseDuration("1h 2m"), 3720);
  EXPECT_EQ(ParseDuration("45 sec"), 45);
  EXPECT_EQ(ParseDuration("PT6M7S"), 367);
  EXPECT_EQ(ParseDuration("PT1H"), 3600);

  for (const char* not_one :
       {"", "6", "6:7", "6:60", ":07", "6:07:", "1:2:3:4", "12345:00",
        "6 apples", "7 seconds 6 minutes", "6 minutes 6 minutes", "PT", "PT6X",
        "PTM", "Bohemian Rhapsody", "6:07 Bohemian", "-6:07", "1001:00:00"}) {
    EXPECT_FALSE(ParseDuration(not_one).has_value()) << not_one;
  }
}

TEST(PageMapTextTest, ParsesCounts) {
  std::optional<ParsedCount> count = ParseCount("2.1B views");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 2100000000);
  EXPECT_EQ(count->unit, "views");

  count = ParseCount("1,234,567 subscribers");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 1234567);
  EXPECT_EQ(count->unit, "subscribers");

  count = ParseCount("940K views");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 940000);

  count = ParseCount("1.5 million plays");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 1500000);
  EXPECT_EQ(count->unit, "plays");

  count = ParseCount("12 people watching");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 12);
  EXPECT_EQ(count->unit, "people watching");

  count = ParseCount("No views");
  ASSERT_TRUE(count);
  EXPECT_EQ(count->value, 0);
  EXPECT_EQ(count->unit, "views");

  for (const char* not_one :
       {"", "views", "2.1B", "1234", "3 days ago", "6 minutes", "5 years",
        "2,1 views", "1,23 views", "12 big red views", "1e9 views",
        "9999999999999999 views", "2.1B views!"}) {
    EXPECT_FALSE(ParseCount(not_one).has_value()) << not_one;
  }
}

TEST(PageMapTextTest, RecognizesDates) {
  for (const char* date : {"15 years ago",
                           "1 year ago",
                           "an hour ago",
                           "3 weeks ago",
                           "Streamed 2 years ago",
                           "Premiered 5 days ago",
                           "yesterday",
                           "Today",
                           "last week",
                           "Jan 5, 2024",
                           "January 5, 2024",
                           "5 January 2024",
                           "Sept. 9, 1975",
                           "March 2024",
                           "Oct 31",
                           "31st October 1975",
                           "2024-01-05",
                           "1/5/2024",
                           "2024/1/5",
                           "Updated on Jan 5, 2024"}) {
    EXPECT_TRUE(LooksLikeDate(date)) << date;
  }
  for (const char* not_one :
       {"", "May", "2024", "5", "15 years", "ago", "2.1B views", "6:07",
        "Jan Feb 2024", "Jan 5 6", "2024-1-5-6", "1/5", "13/45/99",
        "Queen Official", "a long time ago in a galaxy"}) {
    EXPECT_FALSE(LooksLikeDate(not_one)) << not_one;
  }
}

// A page's fields in its own language: Spanish words on a Spanish page,
// whichever language voice mode speaks.
TEST(PageMapTextTest, ReadsSpanishFieldsInSpanish) {
  constexpr FieldLanguage es = FieldLanguage::kSpanish;
  EXPECT_EQ(FieldLanguageOf("es"), es);
  EXPECT_EQ(FieldLanguageOf("es-419"), es);
  EXPECT_EQ(FieldLanguageOf("ES_mx"), es);
  EXPECT_EQ(FieldLanguageOf("en-US"), FieldLanguage::kEnglish);
  EXPECT_EQ(FieldLanguageOf(""), FieldLanguage::kEnglish);
  EXPECT_EQ(FieldLanguageOf("est"), FieldLanguage::kEnglish);

  EXPECT_EQ(ParseDuration("6:07", es), 367);
  EXPECT_EQ(ParseDuration("1:02:03", es), 3723);
  EXPECT_EQ(ParseDuration("6 minutos y 7 segundos", es), 367);
  EXPECT_EQ(ParseDuration("1 hora, 2 minutos y 3 segundos", es), 3723);
  EXPECT_EQ(ParseDuration("1 h 2 min", es), 3720);
  EXPECT_EQ(ParseDuration("45 seg", es), 45);
  EXPECT_EQ(ParseDuration("PT6M7S", es), 367);
  for (const char* not_one :
       {"", "6", "6 manzanas", "7 segundos y 6 minutos", "6 minutes",
        "Bohemian Rhapsody", "y 6 minutos"}) {
    EXPECT_FALSE(ParseDuration(not_one, es).has_value()) << not_one;
  }

  struct Count {
    const char* text;
    int64_t value;
    const char* unit;
  };
  for (const Count& c : std::initializer_list<Count>{
           {"1,2 M de visualizaciones", 1200000, "visualizaciones"},
           {"1.2 M de vistas", 1200000, "vistas"},
           {"2,1 mil M de visualizaciones", 2100000000, "visualizaciones"},
           {"2,1 mil millones de visualizaciones", 2100000000,
            "visualizaciones"},
           {"310 M de visualizaciones", 310000000, "visualizaciones"},
           {"15 mil visualizaciones", 15000, "visualizaciones"},
           {"15 k suscriptores", 15000, "suscriptores"},
           {"1.234 visualizaciones", 1234, "visualizaciones"},
           {"1.234.567 suscriptores", 1234567, "suscriptores"},
           {"1,234 vistas", 1234, "vistas"},
           {"3 millones de reproducciones", 3000000, "reproducciones"},
           {"1 mill. de vistas", 1000000, "vistas"},
           {"120 me gusta", 120, "me gusta"},
           {"8 v\xC3\xAD"
            "deos",
            8,
            "v\xC3\xAD"
            "deos"},
           {"Sin visualizaciones", 0, "visualizaciones"},
       }) {
    std::optional<ParsedCount> count = ParseCount(c.text, es);
    ASSERT_TRUE(count) << c.text;
    EXPECT_EQ(count->value, c.value) << c.text;
    EXPECT_EQ(count->unit, c.unit) << c.text;
  }
  for (const char* not_one :
       {"", "visualizaciones", "1,2 M", "1234",
        "hace 3 d\xC3\xAD"
        "as",
        "6 minutos", "5 a\xC3\xB1os", "1,2345 vistas",
        "12 grandes rojas vistas", "1,2 M de", "2.1B views!"}) {
    EXPECT_FALSE(ParseCount(not_one, es).has_value()) << not_one;
  }

  for (const char* date : {"hace 15 a\xC3\xB1os", "hace 1 a\xC3\xB1o",
                           "hace una hora", "hace un mes", "hace 3 semanas",
                           "hace 2 d\xC3\xAD"
                           "as",
                           "Emitido hace 2 a\xC3\xB1os",
                           "Transmitido hace 5 d\xC3\xAD"
                           "as",
                           "ayer", "Hoy", "la semana pasada", "el mes pasado",
                           "5 ene 2024", "5 de enero de 2024", "enero de 2024",
                           "Publicado el 5 de enero de 2024", "31 dic.",
                           "2024-01-05", "05/01/2024"}) {
    EXPECT_TRUE(LooksLikeDate(date, es)) << date;
  }
  for (const char* not_one :
       {"", "mayo", "2024", "5", "15 a\xC3\xB1os", "hace", "1,2 M de vistas",
        "6:07", "ene feb 2024", "Queen Official", "hace mucho tiempo",
        "15 years ago"}) {
    EXPECT_FALSE(LooksLikeDate(not_one, es)) << not_one;
  }
}

// An English page reads as it always has, whatever its words look like in
// Spanish.
TEST(PageMapTextTest, EnglishFieldsReadAsBefore) {
  EXPECT_EQ(ParseCount("1,234 views")->value, 1234);
  EXPECT_EQ(ParseCount("1.5 million plays")->value, 1500000);
  EXPECT_FALSE(ParseCount("1,2 M de visualizaciones").has_value());
  EXPECT_FALSE(LooksLikeDate("hace 2 a\xC3\xB1os"));
  EXPECT_FALSE(ParseDuration("6 minutos y 7 segundos").has_value());
  EXPECT_EQ(
      ParseDuration("1 hour, 2 minutes and 3 seconds", FieldLanguage::kEnglish),
      3723);
}

TEST(PageMapTextTest, RecognizesAddresses) {
  EXPECT_TRUE(LooksLikeAddress("https://en.wikipedia.org/wiki/Queen"));
  EXPECT_TRUE(LooksLikeAddress("http://example.com"));
  EXPECT_TRUE(LooksLikeAddress("www.bbc.co.uk"));
  EXPECT_TRUE(LooksLikeAddress("en.wikipedia.org \xE2\x80\xBA wiki"));
  EXPECT_TRUE(LooksLikeAddress("genius.com"));
  EXPECT_FALSE(LooksLikeAddress("Wikipedia"));
  EXPECT_FALSE(LooksLikeAddress("e.g."));
  EXPECT_FALSE(LooksLikeAddress("Queen Official"));
  EXPECT_FALSE(LooksLikeAddress("1.2M"));
  EXPECT_FALSE(LooksLikeAddress(""));
}

TEST(PageMapTextTest, TellsTextWithoutWords) {
  EXPECT_TRUE(HasNoWords("\xE2\x80\xA2"));
  EXPECT_TRUE(HasNoWords(" | - \xC2\xB7 \xE2\x80\x94 "));
  EXPECT_TRUE(HasNoWords(""));
  EXPECT_FALSE(HasNoWords("4K"));
  EXPECT_FALSE(HasNoWords("\xC3\xA9"));
  EXPECT_FALSE(HasNoWords("\xE6\x97\xA5"));
}

TEST(PageMapTextTest, FoldsForComparison) {
  EXPECT_EQ(FoldForCompare(" Queen   OFFICIAL "), "queen official");
  EXPECT_EQ(FoldForCompare(""), "");
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
