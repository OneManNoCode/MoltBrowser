// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_numbers.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_phrases.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai {
namespace voice {
namespace {

// Keeps the normalized text alive with its words.
struct Words {
  explicit Words(std::string_view text)
      : normalized(NormalizeSpokenText(text, {})) {
    for (size_t i = 0; i < normalized.words.size(); ++i) {
      list.push_back(normalized.Word(i));
    }
  }
  NormalizedText normalized;
  WordList list;
};

const LanguagePhrases& English() {
  return PhrasesFor(Language::kEnglish);
}

std::optional<SpokenNumber> Number(std::string_view text,
                                   bool homophones = false) {
  const Words w(text);
  NumberOptions options;
  options.homophones = homophones;
  return ReadNumber(w.list, 0, English(), options);
}

TEST(SpokenNumbersTest, Cardinals) {
  struct Case {
    std::string_view text;
    int64_t value;
    size_t words;
  } cases[] = {
      {"zero", 0, 1},
      {"four", 4, 1},
      {"4", 4, 1},
      {"twelve", 12, 1},
      {"twenty", 20, 1},
      {"twenty one", 21, 2},
      {"twenty-one", 21, 2},
      {"ninety nine", 99, 2},
      {"a hundred", 100, 2},
      {"hundred", 100, 1},
      {"one hundred", 100, 2},
      {"one hundred and five", 105, 4},
      {"one hundred five", 105, 3},
      {"nine hundred ninety nine", 999, 4},
      {"nine hundred and ninety nine", 999, 5},
      {"a thousand", 1000, 2},
      {"two thousand and five", 2005, 4},
      {"twelve thousand three hundred forty five", 12345, 6},
      {"nineteen hundred", 1900, 2},
      {"999999999", 999999999, 1},
      {"1000", 1000, 1},
      {"four five", 4, 1},         // two numbers, not one
      {"twenty nineteen", 20, 1},  // a year, not a cardinal
      {"zero five", 0, 1},
      {"one hundred and", 100, 2},  // "and" must lead somewhere
  };
  for (const Case& c : cases) {
    const std::optional<SpokenNumber> n = Number(c.text);
    ASSERT_TRUE(n) << c.text;
    EXPECT_EQ(n->value, c.value) << c.text;
    EXPECT_EQ(n->words, c.words) << c.text;
    EXPECT_FALSE(n->ordinal) << c.text;
  }
  for (std::string_view none :
       {"", "the", "a", "and", "1234567890", "4x", "a minute", "for", "to"}) {
    EXPECT_FALSE(Number(none)) << none;
  }
}

TEST(SpokenNumbersTest, Ordinals) {
  struct Case {
    std::string_view text;
    int64_t value;
  } cases[] = {
      {"first", 1},
      {"fourth", 4},
      {"4th", 4},
      {"21st", 21},
      {"2nd", 2},
      {"3rd", 3},
      {"twelfth", 12},
      {"twentieth", 20},
      {"twenty first", 21},
      {"twenty-first", 21},
      {"hundredth", 100},
      {"one hundredth", 100},
      {"one hundred and first", 101},
      {"thousandth", 1000},
      {"ninety ninth", 99},
  };
  for (const Case& c : cases) {
    const std::optional<SpokenNumber> n = Number(c.text);
    ASSERT_TRUE(n) << c.text;
    EXPECT_EQ(n->value, c.value) << c.text;
    EXPECT_TRUE(n->ordinal) << c.text;
  }
  const Words w("fourth");
  NumberOptions options;
  options.ordinals = false;
  EXPECT_FALSE(ReadNumber(w.list, 0, English(), options));
  options.ordinals = true;
  options.cardinals = false;
  EXPECT_FALSE(ReadNumber(Words("four").list, 0, English(), options));
}

TEST(SpokenNumbersTest, HomophonesOnlyWhereANumberIsExpected) {
  for (std::string_view word : {"for", "fore", "to", "too", "won", "ate"}) {
    EXPECT_FALSE(Number(word)) << word;
    EXPECT_TRUE(Number(word, true)) << word;
  }
  EXPECT_EQ(Number("for", true)->value, 4);
  EXPECT_EQ(Number("too", true)->value, 2);
  EXPECT_EQ(Number("won", true)->value, 1);
  EXPECT_EQ(Number("ate", true)->value, 8);
  EXPECT_EQ(Number("twenty for", true)->value, 24);
  EXPECT_EQ(Number("twenty for")->value, 20);
  EXPECT_TRUE(Number("forth", true)->ordinal);
}

TEST(SpokenNumbersTest, Positions) {
  struct Case {
    std::string_view text;
    int index;
    bool from_end;
    size_t words;
  } cases[] = {
      {"fourth", 4, false, 1},
      {"4th", 4, false, 1},
      {"last", 1, true, 1},
      {"very last", 1, true, 2},
      {"final", 1, true, 1},
      {"second to last", 2, true, 3},
      {"second last", 2, true, 2},
      {"third from last", 3, true, 3},
      {"third from the end", 3, true, 4},
      {"next to last", 2, true, 3},
      {"penultimate", 2, true, 1},
      {"last but one", 2, true, 3},
      {"one before last", 2, true, 3},
      {"one before the last", 2, true, 4},
      {"one hundredth", 100, false, 2},
      {"twenty first", 21, false, 2},
  };
  for (const Case& c : cases) {
    const Words w(c.text);
    const std::optional<SpokenPosition> p =
        ReadPosition(w.list, 0, English(), false);
    ASSERT_TRUE(p) << c.text;
    EXPECT_EQ(p->index, c.index) << c.text;
    EXPECT_EQ(p->from_end, c.from_end) << c.text;
    EXPECT_EQ(p->words, c.words) << c.text;
  }
  EXPECT_FALSE(ReadPosition(Words("four").list, 0, English(), false));
  EXPECT_FALSE(ReadPosition(Words("forth").list, 0, English(), false));
  EXPECT_EQ(ReadPosition(Words("forth").list, 0, English(), true)->index, 4);
}

TEST(SpokenNumbersTest, Durations) {
  struct Case {
    std::string_view text;
    int seconds;
    size_t words;
  } cases[] = {
      {"30 seconds", 30, 2},
      {"thirty seconds", 30, 2},
      {"a minute", 60, 2},
      {"one minute", 60, 2},
      {"an hour", 3600, 2},
      {"half a minute", 30, 3},
      {"half an hour", 1800, 3},
      {"a minute and a half", 90, 5},
      {"one and a half minutes", 90, 5},
      {"1 minute 30 seconds", 90, 4},
      {"one minute and thirty seconds", 90, 5},
      {"90 secs", 90, 2},
      {"30s", 30, 1},
      {"2mins", 120, 1},
      {"two hours", 7200, 2},
      {"to minutes", 120, 2},           // a homophone is a number here
      {"10 seconds 5 minutes", 10, 2},  // units only get smaller
  };
  for (const Case& c : cases) {
    const Words w(c.text);
    const std::optional<SpokenDuration> d = ReadDuration(w.list, 0, English());
    ASSERT_TRUE(d) << c.text;
    EXPECT_EQ(d->seconds, c.seconds) << c.text;
    EXPECT_EQ(d->words, c.words) << c.text;
  }
  for (std::string_view none :
       {"", "minute", "a", "30", "thirty", "a few seconds", "25 hours",
        "999999 hours", "4x"}) {
    EXPECT_FALSE(ReadDuration(Words(none).list, 0, English())) << none;
  }
}

TEST(SpokenNumbersTest, Years) {
  struct Case {
    std::string_view text;
    int64_t value;
    size_t words;
  } cases[] = {
      {"nineteen eighty five", 1985, 3}, {"nineteen eighty", 1980, 2},
      {"twenty twenty four", 2024, 3},   {"twenty ten", 2010, 2},
      {"nineteen oh five", 1905, 3},     {"eighteen twelve", 1812, 2},
  };
  for (const Case& c : cases) {
    const Words w(c.text);
    const std::optional<SpokenNumber> y = ReadYear(w.list, 0, English());
    ASSERT_TRUE(y) << c.text;
    EXPECT_EQ(y->value, c.value) << c.text;
    EXPECT_EQ(y->words, c.words) << c.text;
  }
  for (std::string_view none :
       {"twenty one", "nineteen", "thirty forty", "1985", "nineteen oh"}) {
    EXPECT_FALSE(ReadYear(Words(none).list, 0, English())) << none;
  }
}

}  // namespace
}  // namespace voice
}  // namespace molt_ai
