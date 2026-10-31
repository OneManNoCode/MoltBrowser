// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/spoken_numbers.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace molt_ai {
namespace voice {

namespace {

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

// The digits at the start of |word| and what follows them.
size_t LeadingDigits(std::string_view word) {
  size_t n = 0;
  while (n < word.size() && IsDigit(word[n]))
    ++n;
  return n;
}

int64_t DigitsValue(std::string_view digits) {
  int64_t value = 0;
  for (char c : digits)
    value = value * 10 + (c - '0');
  return value;
}

const NumberWord* WordAt(const WordList& words,
                         size_t at,
                         const LanguagePhrases& phrases) {
  return at < words.size() ? FindNumberWord(phrases, words[at]) : nullptr;
}

const TimeUnitWord* FindTimeUnit(const LanguagePhrases& phrases,
                                 std::string_view word) {
  for (const TimeUnitWord& unit : phrases.time_units) {
    if (unit.word == word)
      return &unit;
  }
  return nullptr;
}

bool IsWordAt(const std::vector<std::string_view>& list,
              const WordList& words,
              size_t at) {
  return at < words.size() && HasPhrase(list, words[at]);
}

// "and a half" at |at|: 3 words ("y medio": 2), else 0.
size_t AndAHalfAt(const WordList& words,
                  size_t at,
                  const LanguagePhrases& phrases) {
  if (IsWordAt(phrases.number_and, words, at) &&
      IsWordAt(phrases.number_article, words, at + 1) &&
      IsWordAt(phrases.half_words, words, at + 2)) {
    return 3;
  }
  return LongestPhraseAt(phrases.and_half_phrases, words, at);
}

struct DurationPart {
  int64_t half_seconds = 0;  // twice the seconds, for "and a half"
  int unit = 0;
  size_t words = 0;
};

// One "<amount> <unit>" at |at|.
std::optional<DurationPart> ReadDurationPart(const WordList& words,
                                             size_t at,
                                             const LanguagePhrases& phrases) {
  if (at >= words.size())
    return std::nullopt;
  // "half a minute"
  if (IsWordAt(phrases.half_words, words, at) &&
      IsWordAt(phrases.number_article, words, at + 1) &&
      at + 2 < words.size()) {
    if (const TimeUnitWord* unit = FindTimeUnit(phrases, words[at + 2])) {
      return DurationPart{unit->seconds, unit->seconds, 3};
    }
  }
  // "medio minuto"
  if (phrases.half_before_unit && IsWordAt(phrases.half_words, words, at) &&
      at + 1 < words.size()) {
    if (const TimeUnitWord* unit = FindTimeUnit(phrases, words[at + 1])) {
      return DurationPart{unit->seconds, unit->seconds, 2};
    }
  }
  // "30s", "10sec", "2mins": digits with a unit glued on.
  const size_t digits = LeadingDigits(words[at]);
  if (digits > 0 && digits < words[at].size() && digits <= 6) {
    const TimeUnitWord* unit = FindTimeUnit(phrases, words[at].substr(digits));
    if (!unit)
      return std::nullopt;
    return DurationPart{
        2 * DigitsValue(words[at].substr(0, digits)) * unit->seconds,
        unit->seconds, 1};
  }
  int64_t halves = 0;
  size_t i = at;
  if (IsWordAt(phrases.number_article, words, i) && i + 1 < words.size() &&
      FindTimeUnit(phrases, words[i + 1])) {
    halves = 2;  // "a minute"
    ++i;
  } else {
    NumberOptions options;
    options.ordinals = false;
    options.homophones = true;
    const std::optional<SpokenNumber> number =
        ReadNumber(words, i, phrases, options);
    if (!number)
      return std::nullopt;
    halves = 2 * number->value;
    i += number->words;
    if (const size_t n = AndAHalfAt(words, i, phrases)) {
      halves += 1;  // "one and a half minutes"
      i += n;
    }
  }
  if (i >= words.size())
    return std::nullopt;
  const TimeUnitWord* unit = FindTimeUnit(phrases, words[i]);
  if (!unit)
    return std::nullopt;
  ++i;
  if (halves % 2 == 0) {
    if (const size_t n = AndAHalfAt(words, i, phrases)) {
      halves += 1;  // "a minute and a half"
      i += n;
    }
  }
  return DurationPart{halves * unit->seconds, unit->seconds, i - at};
}

}  // namespace

std::optional<SpokenNumber> ReadNumber(const WordList& words,
                                       size_t at,
                                       const LanguagePhrases& phrases,
                                       const NumberOptions& options) {
  if (at >= words.size() || words[at].empty())
    return std::nullopt;

  // Digits, perhaps with an ordinal suffix: "4", "1000", "4th".
  const std::string_view first = words[at];
  const size_t digits = LeadingDigits(first);
  if (digits > 0) {
    if (digits > 9)
      return std::nullopt;
    const int64_t value = DigitsValue(first.substr(0, digits));
    if (digits == first.size()) {
      if (!options.cardinals)
        return std::nullopt;
      return SpokenNumber{value, 1, false};
    }
    if (options.ordinals &&
        HasPhrase(phrases.ordinal_suffixes, first.substr(digits))) {
      return SpokenNumber{value, 1, true};
    }
    return std::nullopt;
  }

  // Words, built as English builds them: units, teens and tens, then
  // hundred and thousand multiplying what came before. "and" may follow a
  // multiplier; "a" may stand for "one" before one. An ordinal word ends
  // the number. Spanish adds whole hundreds in one word ("doscientos",
  // read as a hundred multiplier just said) and "y" between tens and a
  // unit ("treinta y uno"), and ordinals of two words ("vigésimo primero").
  enum class Last {
    kNone,
    kArticle,
    kUnit,
    kTeen,
    kTens,
    kHundred,
    kThousand,
    kAnd,
    kTensAnd
  };
  int64_t total = 0;
  int64_t current = 0;
  Last last = Last::kNone;
  std::optional<SpokenNumber> best;
  const auto usable = [&](const NumberWord* word) {
    return word && (!word->homophone || options.homophones) &&
           (!word->ordinal || options.ordinals);
  };
  for (size_t i = at; i < words.size(); ++i) {
    const std::string_view w = words[i];
    if (last == Last::kNone && HasPhrase(phrases.number_article, w)) {
      const NumberWord* next = WordAt(words, i + 1, phrases);
      if (!usable(next) || (next->kind != NumberWordKind::kHundred &&
                            next->kind != NumberWordKind::kThousand)) {
        break;
      }
      current = 1;
      last = Last::kArticle;
      continue;
    }
    if ((last == Last::kHundred || last == Last::kThousand) &&
        HasPhrase(phrases.number_and, w)) {
      const NumberWord* next = WordAt(words, i + 1, phrases);
      if (!usable(next) || next->kind == NumberWordKind::kHundred ||
          next->kind == NumberWordKind::kThousand || next->value == 0) {
        break;
      }
      last = Last::kAnd;
      continue;
    }
    if (last == Last::kTens && HasPhrase(phrases.tens_unit_and, w)) {
      const NumberWord* next = WordAt(words, i + 1, phrases);
      if (!usable(next) || next->kind != NumberWordKind::kUnit ||
          next->value == 0) {
        break;
      }
      last = Last::kTensAnd;
      continue;
    }
    const NumberWord* word = FindNumberWord(phrases, w);
    if (!usable(word))
      break;
    const bool after_multiplier =
        last == Last::kHundred || last == Last::kThousand || last == Last::kAnd;
    bool fits = false;
    Last now = last;
    switch (word->kind) {
      case NumberWordKind::kUnit:
        if (word->value == 0) {
          fits = last == Last::kNone;  // "zero" stands alone
        } else {
          fits = last == Last::kNone || last == Last::kTens ||
                 last == Last::kTensAnd || after_multiplier;
        }
        current += word->value;
        now = Last::kUnit;
        break;
      case NumberWordKind::kTeen:
      case NumberWordKind::kTens:
        fits = last == Last::kNone || after_multiplier;
        current += word->value;
        now = word->kind == NumberWordKind::kTeen ? Last::kTeen : Last::kTens;
        break;
      case NumberWordKind::kHundred:
        if (last == Last::kNone) {
          fits = true;  // "hundred", "hundredth"
          current = 100;
        } else {
          fits = (last == Last::kUnit || last == Last::kTeen ||
                  last == Last::kTens || last == Last::kArticle) &&
                 current >= 1 && current <= 99;
          current *= 100;
        }
        now = Last::kHundred;
        break;
      case NumberWordKind::kThousand:
        if (last == Last::kNone) {
          fits = true;  // "thousand", "thousandth"
          current = 1;
        } else {
          fits = total == 0 &&
                 (last == Last::kUnit || last == Last::kTeen ||
                  last == Last::kTens || last == Last::kHundred ||
                  last == Last::kArticle) &&
                 current >= 1 && current <= 999;
        }
        total = current * 1000;
        current = 0;
        now = Last::kThousand;
        break;
      case NumberWordKind::kHundreds:
        // "doscientos", "mil trescientos": first, or after the thousands.
        fits = last == Last::kNone || (last == Last::kThousand && current == 0);
        current += word->value;
        now = Last::kHundred;
        break;
    }
    if (!fits)
      break;
    last = now;
    if (word->ordinal) {
      // "vigésimo primero", "décimo tercero".
      if (phrases.compound_ordinals &&
          (word->kind == NumberWordKind::kTens ||
           (word->kind == NumberWordKind::kTeen && word->value == 10))) {
        const NumberWord* next = WordAt(words, i + 1, phrases);
        if (usable(next) && next->ordinal &&
            next->kind == NumberWordKind::kUnit && next->value > 0) {
          return SpokenNumber{total + current + next->value, i - at + 2, true};
        }
      }
      return SpokenNumber{total + current, i - at + 1, true};
    }
    if (options.cardinals)
      best = SpokenNumber{total + current, i - at + 1, false};
    if (word->kind == NumberWordKind::kUnit && word->value == 0)
      break;
  }
  return best;
}

std::optional<SpokenPosition> ReadPosition(const WordList& words,
                                           size_t at,
                                           const LanguagePhrases& phrases,
                                           bool homophones) {
  size_t i = at;
  i += LongestPhraseAt(phrases.position_intensifiers, words, i);
  if (const size_t n =
          LongestPhraseAt(phrases.second_to_last_phrases, words, i)) {
    return SpokenPosition{2, true, i + n - at};
  }
  if (const size_t n = LongestPhraseAt(phrases.last_words, words, i)) {
    return SpokenPosition{1, true, i + n - at};
  }
  if (const size_t n =
          LongestPhraseAt(phrases.third_to_last_phrases, words, i)) {
    return SpokenPosition{3, true, i + n - at};
  }
  NumberOptions options;
  options.cardinals = false;
  options.homophones = homophones;
  const std::optional<SpokenNumber> number =
      ReadNumber(words, i, phrases, options);
  if (!number || !number->ordinal || number->value < 1 ||
      number->value > 999'999) {
    return std::nullopt;
  }
  const int index = static_cast<int>(number->value);
  i += number->words;
  // "second to last", "third from last", "second last".
  const size_t link = LongestPhraseAt(phrases.from_end_links, words, i);
  if (const size_t n = LongestPhraseAt(phrases.last_words, words, i + link)) {
    return SpokenPosition{index, true, i + link + n - at};
  }
  if (const size_t n = LongestPhraseAt(phrases.end_phrases, words, i)) {
    return SpokenPosition{index, true, i + n - at};
  }
  return SpokenPosition{index, false, i - at};
}

std::optional<SpokenDuration> ReadDuration(const WordList& words,
                                           size_t at,
                                           const LanguagePhrases& phrases) {
  int64_t half_seconds = 0;
  size_t i = at;
  int last_unit = 0;
  while (i < words.size()) {
    // "1 minute and 30 seconds": "and" between parts.
    const size_t joiner =
        (last_unit && IsWordAt(phrases.number_and, words, i)) ? 1 : 0;
    const std::optional<DurationPart> part =
        ReadDurationPart(words, i + joiner, phrases);
    // Units only get smaller: "1 minute 30 seconds".
    if (!part || (last_unit && part->unit >= last_unit))
      break;
    half_seconds += part->half_seconds;
    if (half_seconds > 2 * int64_t{kMaxSpokenSeconds})
      return std::nullopt;
    last_unit = part->unit;
    i += joiner + part->words;
  }
  if (!last_unit)
    return std::nullopt;
  return SpokenDuration{static_cast<int>(half_seconds / 2), i - at};
}

std::optional<SpokenNumber> ReadYear(const WordList& words,
                                     size_t at,
                                     const LanguagePhrases& phrases) {
  if (!phrases.years_in_two_parts)
    return std::nullopt;
  const auto plain = [&](size_t i) -> const NumberWord* {
    const NumberWord* word = WordAt(words, i, phrases);
    return (word && !word->ordinal && !word->homophone) ? word : nullptr;
  };
  const NumberWord* high = plain(at);
  if (!high || !(high->kind == NumberWordKind::kTeen ||
                 (high->kind == NumberWordKind::kTens && high->value == 20))) {
    return std::nullopt;
  }
  const int64_t century = high->value * 100;
  // "nineteen oh five"
  if (IsWordAt(phrases.year_zero, words, at + 1)) {
    const NumberWord* unit = plain(at + 2);
    if (unit && unit->kind == NumberWordKind::kUnit && unit->value > 0) {
      return SpokenNumber{century + unit->value, 3, false};
    }
    return std::nullopt;
  }
  const NumberWord* low = plain(at + 1);
  if (!low)
    return std::nullopt;
  if (low->kind == NumberWordKind::kTeen) {
    return SpokenNumber{century + low->value, 2, false};
  }
  if (low->kind != NumberWordKind::kTens)
    return std::nullopt;
  const NumberWord* unit = plain(at + 2);
  if (unit && unit->kind == NumberWordKind::kUnit && unit->value > 0) {
    return SpokenNumber{century + low->value + unit->value, 3, false};
  }
  return SpokenNumber{century + low->value, 2, false};
}

}  // namespace voice
}  // namespace molt_ai
