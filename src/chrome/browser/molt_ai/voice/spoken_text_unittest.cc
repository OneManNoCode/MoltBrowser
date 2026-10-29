// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_text.h"

#include <string>
#include <string_view>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

std::vector<std::string> Words(std::string_view text, bool marks = false) {
  NormalizeOptions options;
  options.marks = marks;
  const NormalizedText normalized = NormalizeSpokenText(text, options);
  std::vector<std::string> words;
  for (size_t i = 0; i < normalized.words.size(); ++i) {
    words.emplace_back(normalized.Word(i));
  }
  return words;
}

using V = std::vector<std::string>;

TEST(SpokenTextTest, FoldsCasePunctuationAndAccents) {
  EXPECT_EQ(Words("Play the Fourth one!"), (V{"play", "the", "fourth", "one"}));
  EXPECT_EQ(Words("What's on the screen?"),
            (V{"whats", "on", "the", "screen"}));
  EXPECT_EQ(Words("don\xE2\x80\x99t"), (V{"dont"}));  // U+2019
  EXPECT_EQ(Words("Beyonc\xC3\xA9 Stra\xC3\x9F"
                  "e \xC5\x81\xC3\xB3"
                  "d\xC5\xBA"),
            (V{"beyonce", "strasse", "lodz"}));
  EXPECT_EQ(Words("\xC3\x86on \xC5\x92uvre \xC5\x93il"),
            (V{"aeon", "oeuvre", "oeil"}));
  EXPECT_EQ(Words("e\xCC\x81t\xC3\xA9"), (V{"ete"}));  // combining acute
  EXPECT_EQ(Words("\xEF\xBC\xB9\xEF\xBC\xAF\xEF\xBC\xB5"),
            (V{"you"}));  // fullwidth
  EXPECT_EQ(Words("\xC8\x98"
                  "coal\xC4\x83"),
            (V{"scoala"}));
  // Greek and Cyrillic capitals are lowered; other scripts kept.
  EXPECT_EQ(Words("\xCE\x91\xCE\x92 \xD0\x9A\xD0\x98\xD0\x9D\xD0\x9E"),
            (V{"\xCE\xB1\xCE\xB2", "\xD0\xBA\xD0\xB8\xD0\xBD\xD0\xBE"}));
  EXPECT_EQ(Words("\xE6\x97\xA5\xE6\x9C\xAC"), (V{"\xE6\x97\xA5\xE6\x9C\xAC"}));
}

TEST(SpokenTextTest, InvisibleCharactersDropWithoutBreaking) {
  EXPECT_EQ(Words("you\xE2\x80\x8Btube"), (V{"youtube"}));        // ZWSP
  EXPECT_EQ(Words("you\xC2\xADtube"), (V{"youtube"}));            // soft hyphen
  EXPECT_EQ(Words("\xE2\x80\xAEplay\xE2\x80\xAC"), (V{"play"}));  // bidi
  EXPECT_EQ(Words("\xEF\xBB\xBFgo back"), (V{"go", "back"}));     // BOM
  // Controls, emoji and symbols break words.
  EXPECT_EQ(Words("go\x07"
                  "back"),
            (V{"go", "back"}));
  EXPECT_EQ(Words("go\xF0\x9F\x98\x80"
                  "back"),
            (V{"go", "back"}));
  EXPECT_EQ(Words("rock & roll"), (V{"rock", "roll"}));
}

TEST(SpokenTextTest, NumbersAndMarks) {
  EXPECT_EQ(Words("1,000 people"), (V{"1000", "people"}));
  EXPECT_EQ(Words("1,00 x"), (V{"1", "00", "x"}));
  EXPECT_EQ(Words("1,0000"), (V{"1", "0000"}));
  EXPECT_EQ(Words("the 4th one"), (V{"the", "4th", "one"}));
  EXPECT_EQ(Words("50%", true), (V{"50", "percent"}));
  EXPECT_EQ(Words("50%"), (V{"50"}));
  EXPECT_EQ(Words("#4", true), (V{"number", "4"}));
  EXPECT_EQ(Words("No. 4", true), (V{"number", "4"}));
  EXPECT_EQ(Words("no.4", true), (V{"number", "4"}));
  EXPECT_EQ(Words("No. I said", true), (V{"no", "i", "said"}));
  EXPECT_EQ(Words("no, 4", true), (V{"no", "4"}));
}

TEST(SpokenTextTest, JoinersAndTrailing) {
  const NormalizedText t = NormalizeSpokenText("example.com AC/DC no. x-y", {});
  ASSERT_EQ(t.words.size(), 7u);
  EXPECT_EQ(t.words[1].joiner, '.');
  EXPECT_EQ(t.words[3].joiner, '/');
  EXPECT_EQ(t.words[4].trailing, '.');
  EXPECT_EQ(t.words[6].joiner, '-');
  EXPECT_EQ(t.words[0].joiner, 0);
  EXPECT_TRUE(NormalizeSpokenText("1985", {}).words[0].numeric);
  EXPECT_FALSE(NormalizeSpokenText("4th", {}).words[0].numeric);
  EXPECT_TRUE(NormalizeSpokenText("4th", {}).words[0].ascii);
  EXPECT_FALSE(NormalizeSpokenText("\xCE\xB1", {}).words[0].ascii);
}

TEST(SpokenTextTest, InvalidUtf8) {
  EXPECT_TRUE(IsValidUtf8("plain \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
  for (std::string_view bad :
       {"\xFF", "\xC0\xAF", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80",
        "\xC3", "\xE2\x82", "\x80"}) {
    EXPECT_FALSE(IsValidUtf8(bad));
    const NormalizedText t =
        NormalizeSpokenText(std::string("go ") + std::string(bad) + "back", {});
    EXPECT_TRUE(t.had_invalid_utf8);
    EXPECT_EQ(t.words.size(), 2u);
  }
}

TEST(SpokenTextTest, MaxWords) {
  NormalizeOptions options;
  options.max_words = 3;
  const NormalizedText t = NormalizeSpokenText("a b c d e", options);
  EXPECT_EQ(t.words.size(), 3u);
  EXPECT_TRUE(t.truncated);
}

TEST(SpokenTextTest, CleanAndSourceText) {
  EXPECT_EQ(CleanSpokenText("  open\x01 the\xE2\x80\x8F   New\tYork  ", 100),
            "open the New York");
  EXPECT_EQ(CleanSpokenText("a\xFF"
                            "b",
                            100),
            "ab");
  // What page text loses before it is spoken or shown goes here too:
  // private-use characters (U+E000, U+F0000), noncharacters (U+FDD0,
  // U+FFFF), the object replacement and replacement characters, and the
  // Hangul fillers (U+3164, U+115F).
  EXPECT_EQ(CleanSpokenText("open\xEE\x80\x80 you\xF3\xB0\x80\x80tube "
                            "\xEF\xB7\x90\xEF\xBF\xBF\xEF\xBF\xBC\xEF\xBF\xBD"
                            "now \xE3\x85\xA4\xE1\x85\x9F!",
                            100),
            "open youtube now !");
  // Letters of other scripts stay.
  EXPECT_EQ(CleanSpokenText("\xED\x95\x9C \xCE\xB1", 100),
            "\xED\x95\x9C \xCE\xB1");
  // Never cut inside a character.
  EXPECT_EQ(CleanSpokenText("ab\xC3\xA9", 3), "ab");
  const std::string source = "search for um AC/DC, node.js";
  const NormalizedText t = NormalizeSpokenText(source, {});
  ASSERT_EQ(t.words.size(), 7u);
  EXPECT_EQ(SourceText(source, t, 3, 6, {}), "AC/DC, node.js");
  EXPECT_EQ(SourceText(source, t, 0, 6, {2}), "search for AC/DC, node.js");
  EXPECT_EQ(SourceText(source, t, 4, 3, {}), "");
  EXPECT_EQ(FoldForComparison("Beyonc\xC3\xA9 - Halo"), "beyonce halo");
}

TEST(SpokenTextTest, LanguageTags) {
  EXPECT_EQ(LanguageFromTag("en"), Language::kEnglish);
  EXPECT_EQ(LanguageFromTag("en-US"), Language::kEnglish);
  EXPECT_EQ(LanguageFromTag("EN_gb"), Language::kEnglish);
  EXPECT_FALSE(LanguageFromTag("fr"));
  EXPECT_FALSE(LanguageFromTag(""));
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
