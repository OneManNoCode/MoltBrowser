// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Numbers as recognizers write them: digits ("4", "1000", "4th") or words
// ("four", "twenty-one", "a hundred and five", "fourth", "one hundredth"),
// positions ("the last one", "second to last"), lengths of time ("a
// minute and a half") and years said as years ("nineteen eighty five").
//
// Each reader takes the folded words (spoken_text.h) and a start, and
// reads the longest number that starts there, saying how many words it
// took. They never look back and never recurse: linear in the words read.
//
// Words that only sound like numbers ("for", "to", "too", "won", "ate",
// "forth") are read as numbers only with |homophones|, which the grammar
// sets where a number is expected ("number for", "click to").

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_NUMBERS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_NUMBERS_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "chrome/browser/molt_ai/voice/spoken_phrases.h"

namespace molt_ai {
namespace voice {

// Number words read up to 999,999; digits up to nine of them.
inline constexpr int64_t kMaxSpokenNumber = 999'999'999;
// The longest length of time read (a day).
inline constexpr int kMaxSpokenSeconds = 24 * 60 * 60;

struct NumberOptions {
  bool homophones = false;
  bool cardinals = true;  // "four", "4"
  bool ordinals = true;   // "fourth", "4th"
};

struct SpokenNumber {
  int64_t value = 0;
  size_t words = 0;  // how many words it took
  bool ordinal = false;
};

// The longest number at |words[at]|; nullopt if none starts there.
std::optional<SpokenNumber> ReadNumber(const WordList& words,
                                       size_t at,
                                       const LanguagePhrases& phrases,
                                       const NumberOptions& options);

struct SpokenPosition {
  // 1-based; counted from the end when |from_end| ("last" is 1 from the
  // end).
  int index = 0;
  bool from_end = false;
  size_t words = 0;
};

// A position: an ordinal ("fourth", "4th", "hundredth"), "last", "second
// to last", "third from the end", "next to last", "penultimate", "last but
// one". Ordinals up to 999,999.
std::optional<SpokenPosition> ReadPosition(const WordList& words,
                                           size_t at,
                                           const LanguagePhrases& phrases,
                                           bool homophones);

struct SpokenDuration {
  int seconds = 0;
  size_t words = 0;
};

// A length of time: "30 seconds", "a minute", "two minutes thirty
// seconds", "1 minute and 30 seconds", "half a minute", "a minute and a
// half", "one and a half minutes", "90 secs". Rounded down to whole
// seconds; nullopt past kMaxSpokenSeconds.
std::optional<SpokenDuration> ReadDuration(const WordList& words,
                                           size_t at,
                                           const LanguagePhrases& phrases);

// A year said in two halves: "nineteen eighty five" (1985), "twenty twenty
// four" (2024), "nineteen oh five" (1905). Plain cardinals ("two thousand
// five") are ReadNumber()'s.
std::optional<SpokenNumber> ReadYear(const WordList& words,
                                     size_t at,
                                     const LanguagePhrases& phrases);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_NUMBERS_H_
