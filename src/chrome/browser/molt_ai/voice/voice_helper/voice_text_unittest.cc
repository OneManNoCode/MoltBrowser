// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_voice {
namespace {

// The sentences as text: words, <spelled>, "," for pauses, the end mark,
// " | " between sentences.
std::string Said(std::string_view text) {
  std::string out;
  for (const SpokenSentence& sentence : NormalizeForSpeech(text)) {
    if (!out.empty()) {
      out += " | ";
    }
    bool first = true;
    for (const SpokenWord& word : sentence.words) {
      if (!first) {
        out += ' ';
      }
      first = false;
      switch (word.kind) {
        case WordKind::kWord:
          out += word.text;
          break;
        case WordKind::kSpell:
          out += "<" + word.text + ">";
          break;
        case WordKind::kPause:
          out += ',';
          break;
      }
    }
    out += sentence.end;
  }
  return out;
}

TEST(VoiceTextTest, NumbersAsWords) {
  EXPECT_EQ("zero", CardinalWords(0));
  EXPECT_EQ("one thousand two hundred thirty four", CardinalWords(1234));
  EXPECT_EQ("one million", CardinalWords(1000000));
  EXPECT_EQ("first", OrdinalWords(1));
  EXPECT_EQ("twenty second", OrdinalWords(22));
  EXPECT_EQ("thirtieth", OrdinalWords(30));
  EXPECT_EQ("one hundredth", OrdinalWords(100));
  EXPECT_EQ("nineteen eighty five", YearWords(1985));
  EXPECT_EQ("two thousand five", YearWords(2005));
  EXPECT_EQ("twenty twenty six", YearWords(2026));
  EXPECT_EQ("nineteen hundred", YearWords(1900));
  EXPECT_EQ("nineteen oh five", YearWords(1905));
  // Past the trillions: digit by digit.
  EXPECT_EQ(
      "one zero zero zero zero zero zero zero zero zero zero zero zero zero "
      "zero zero",
      CardinalWords(1000000000000000ULL));
}

TEST(VoiceTextTest, Sentences) {
  EXPECT_EQ("YouTube is open.", Said("YouTube is open."));
  EXPECT_EQ("twenty results. | Say a number or a name.",
            Said("20 results. Say a number or a name."));
  EXPECT_EQ("Really? | Yes!", Said("Really?? Yes!"));
  // Pauses: dashes, parentheses, commas; never two in a row, never at an
  // edge.
  EXPECT_EQ("one , Queen , Bohemian Rhapsody , Live Aid.",
            Said("1: Queen - Bohemian Rhapsody (Live Aid)."));
  EXPECT_EQ("", Said("..... ,,,,, !!!! ???? ;;; ---- ((( ))) \"\"\" *** ___"));
  EXPECT_EQ("a , b.", Said(", , a ,,, -- b ,"));
}

TEST(VoiceTextTest, Numbers) {
  EXPECT_EQ("one thousand two hundred thirty four people.",
            Said("1,234 people."));
  EXPECT_EQ("three point five.", Said("3.5"));
  EXPECT_EQ("minus five degrees.", Said("-5 degrees"));
  EXPECT_EQ("the third and twenty second.", Said("the 3rd and 22nd"));
  EXPECT_EQ("one point two million views.", Said("1.2M views"));
  EXPECT_EQ("fifteen thousand.", Said("15K"));
  EXPECT_EQ("fifty percent.", Said("50%"));
  EXPECT_EQ("Live Aid nineteen eighty five.", Said("Live Aid 1985"));
  EXPECT_EQ("the nineteen eighties and nineties.", Said("the 1980s and 90s"));
  EXPECT_EQ("number one hit.", Said("#1 hit"));
  EXPECT_EQ("number five.", Said("No. 5"));
  EXPECT_EQ("five to ten minutes.", Said("5-10 minutes"));
  EXPECT_EQ("four out of five stars.", Said("4/5 stars"));
  EXPECT_EQ("ten gigabytes.", Said("10GB"));
  EXPECT_EQ("seventy two degrees Fahrenheit.", Said("72\xC2\xB0"
                                                    "F"));
  EXPECT_EQ("two point zero point one.", Said("2.0.1"));
  // Leading zeros and very long numbers: digit by digit, at most
  // kMaxDigitsRead digits.
  EXPECT_EQ("zero zero seven.", Said("007"));
  const std::string long_number(60, '9');
  const std::vector<SpokenSentence> sentences = NormalizeForSpeech(long_number);
  ASSERT_EQ(1u, sentences.size());
  EXPECT_EQ(kMaxDigitsRead, sentences[0].words.size());
}

TEST(VoiceTextTest, Money) {
  EXPECT_EQ("three hundred forty nine dollars and ninety nine cents.",
            Said("$349.99"));
  EXPECT_EQ("one dollar.", Said("$1"));
  EXPECT_EQ("five dollars.", Said("$5"));
  EXPECT_EQ("ninety nine cents.", Said("$0.99"));
  EXPECT_EQ("two hundred ninety nine euros.", Said("\xE2\x82\xAC"
                                                   "299"));
  EXPECT_EQ("one point two million dollars.", Said("$1.2M"));
  EXPECT_EQ("four pounds and fifty pence.", Said("\xC2\xA3"
                                                 "4.50"));
}

TEST(VoiceTextTest, TimesAndDates) {
  EXPECT_EQ("six oh seven.", Said("6:07"));
  EXPECT_EQ("ten o'clock.", Said("10:00"));
  EXPECT_EQ("ten thirty <pm>.", Said("10:30pm"));
  EXPECT_EQ("one hour two minutes three seconds.", Said("1:02:03"));
  EXPECT_EQ("forty five seconds.", Said("0:45"));
  EXPECT_EQ("forty five minutes thirty seconds.", Said("45:30"));
  EXPECT_EQ("October thirtieth , twenty twenty six.", Said("2026-10-30"));
  EXPECT_EQ("October thirtieth , twenty twenty six.", Said("10/30/2026"));
  // Not dates.
  EXPECT_EQ("twenty twenty six to thirteen to forty.",
            Said("2026-13-40"));
}

TEST(VoiceTextTest, WordsAndAbbreviations) {
  EXPECT_EQ("doctor Smith and mister Jones.", Said("Dr. Smith & Mr. Jones"));
  EXPECT_EQ("for example <us> , that is it.", Said("e.g. U.S., i.e. it"));
  EXPECT_EQ("Sennheiser versus Bose.", Said("Sennheiser vs Bose"));
  EXPECT_EQ("mp three.", Said("mp3"));
  EXPECT_EQ("C plus plus and <c> sharp.", Said("C++ and C#"));
  EXPECT_EQ("Node dot js.", Said("Node.js"));
  EXPECT_EQ("state of the art.", Said("state-of-the-art"));
  EXPECT_EQ("don't stop , it's Wayne's.", Said("don't stop, it's Wayne's"));
  EXPECT_EQ("with milk.", Said("w/ milk"));
}

TEST(VoiceTextTest, Addresses) {
  EXPECT_EQ("Visit example dot com.",
            Said("Visit https://www.example.com/path?q=1&x=2#top"));
  EXPECT_EQ("youtube dot com.", Said("youtube.com/watch?v=abc"));
  EXPECT_EQ("bbc dot co dot <uk>.", Said("bbc.co.uk"));
  EXPECT_EQ("someone at example dot org.", Said("someone@example.org"));
  EXPECT_EQ("john dot smith four two at mail dot example dot com.",
            Said("john.smith42@mail.example.com"));
  // Not addresses.
  EXPECT_EQ("file dot txt.", Said("file.txt"));
}

TEST(VoiceTextTest, Unicode) {
  // Accents go; quotes, dashes and spaces fold; fullwidth letters fold.
  EXPECT_EQ("Beyonce's cafe.", Said("Beyonc\xC3\xA9\xE2\x80\x99s caf\xC3\xA9"));
  EXPECT_EQ("Fullwidth.", Said("\xEF\xBC\xA6\xEF\xBD\x95\xEF\xBD\x8C\xEF\xBD"
                               "\x8C\xEF\xBD\x97\xEF\xBD\x89\xEF\xBD\x84\xEF"
                               "\xBD\x94\xEF\xBD\x88"));
  // Zero-width characters and bidirectional overrides vanish; combining
  // marks too (zalgo).
  EXPECT_EQ("Hey evil Molt.", Said("Hey\xE2\x80\xAE evil\xE2\x80\xAC Molt"));
  EXPECT_EQ("withzerowidth.", Said("with\xE2\x80\x8Bzero\xE2\x80\x8Bwidth"));
  EXPECT_EQ("zalgo.", Said("z\xCC\xB7\xCC\xA2\xCD\x88"
                           "a\xCC\xB4lgo"));
  // Other scripts and emoji separate words and aren't read.
  EXPECT_EQ("a b.", Said("a\xF0\x9F\x8E\x89"
                         "b"));
  EXPECT_EQ("x y.", Said("x \xE4\xB8\xAD\xE6\x96\x87 y"));
  // Invalid UTF-8 separates words.
  EXPECT_EQ("a b c.", Said("a\xFF"
                           "b\xC0\x80"
                           "c"));
}

TEST(VoiceTextTest, Bounded) {
  // Only kMaxSpeakTextBytes are read.
  std::string text;
  while (text.size() < 3 * kMaxSpeakTextBytes) {
    text += "word ";
  }
  size_t words = 0;
  for (const SpokenSentence& sentence : NormalizeForSpeech(text)) {
    words += sentence.words.size();
  }
  // 819 times "word " and a "w".
  EXPECT_EQ(820u, words);
  // A long word is cut.
  const std::vector<SpokenSentence> sentences =
      NormalizeForSpeech(std::string(500, 'a'));
  ASSERT_EQ(1u, sentences.size());
  ASSERT_EQ(1u, sentences[0].words.size());
  EXPECT_EQ(kMaxWordLetters, sentences[0].words[0].text.size());
  // At most kMaxSpokenWords words, pauses included.
  std::string many;
  for (int i = 0; i < 2000; ++i) {
    many += "a,";
  }
  words = 0;
  for (const SpokenSentence& sentence : NormalizeForSpeech(many)) {
    words += sentence.words.size();
  }
  EXPECT_LE(words, kMaxSpokenWords);
}

}  // namespace
}  // namespace molt_voice
