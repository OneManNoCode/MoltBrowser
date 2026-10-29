// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The words voice mode knows, per language: numbers, positions, lengths of
// time, the words of choice references, fillers and politeness, and the
// words of a spoken address. The command patterns themselves are in
// voice_command_phrases.h.
//
// Adding a language: write spoken_phrases_<lang>.cc and
// voice_command_phrases_<lang>.cc with the same tables, and add the
// language to Language (spoken_text.h) and to the two dispatchers. The
// code reading the tables doesn't change, as long as the language's
// numbers are built like English ones (units, teens, tens, then hundred
// and thousand as multipliers); see spoken_numbers.h.
//
// Every entry is folded as spoken_text.h folds words (lower-case ASCII, no
// apostrophes). An entry of several words ("next to last") is written with
// single spaces.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_PHRASES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_PHRASES_H_

#include <cstddef>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_choice.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

enum class NumberWordKind : uint8_t {
  kUnit,      // 0..9
  kTeen,      // 10..19
  kTens,      // 20, 30, ... 90
  kHundred,   // multiplies by 100
  kThousand,  // multiplies by 1000
};

struct NumberWord {
  std::string_view word;
  NumberWordKind kind = NumberWordKind::kUnit;
  int value = 0;
  // "fourth", "twentieth", "hundredth".
  bool ordinal = false;
  // A word that sounds like a number but usually isn't one ("for", "to",
  // "won", "ate", "forth"): read as a number only where a number is
  // expected (NumberOptions::homophones).
  bool homophone = false;
};

struct TimeUnitWord {
  std::string_view word;
  int seconds = 0;
};

struct RoleWord {
  std::string_view word;
  // The roles it names; 0 for a word that names an item without saying
  // what it is ("one", "result", "item").
  ChoiceRoleMask roles = 0;
};

struct ValueWord {
  std::string_view word;  // may be several words
  int value = 0;
};

struct AddressWord {
  std::string_view word;
  char mark = 0;  // what it stands for in an address: '.', '/', '-', '_'
};

struct LanguagePhrases {
  LanguagePhrases();
  ~LanguagePhrases();
  LanguagePhrases(const LanguagePhrases&);
  LanguagePhrases& operator=(const LanguagePhrases&);
  LanguagePhrases(LanguagePhrases&&);
  LanguagePhrases& operator=(LanguagePhrases&&);

  Language language = Language::kEnglish;

  // --- Numbers (spoken_numbers.h) ------------------------------------------
  std::vector<NumberWord> numbers;  // sorted by word
  // Joins a hundred or a thousand to the rest: "one hundred and five".
  std::vector<std::string_view> number_and;
  // Stands for "one" before a multiplier: "a hundred".
  std::vector<std::string_view> number_article;
  // After digits: "4th", "21st".
  std::vector<std::string_view> ordinal_suffixes;
  // "oh" in a year: "nineteen oh five".
  std::vector<std::string_view> year_zero;
  // "number 4", "num 4". ("No." and "#" become "number" in spoken_text.h.)
  std::vector<std::string_view> number_prefixes;

  // --- Positions from the end ----------------------------------------------
  // "last": the first from the end.
  std::vector<std::string_view> last_words;
  // Between an ordinal and a last word: "second to last", "third from
  // last". ("second last" needs none.)
  std::vector<std::string_view> from_end_links;
  // After an ordinal: "second from the end".
  std::vector<std::string_view> end_phrases;
  // The second from the end: "next to last", "penultimate", "last but one",
  // "one before last".
  std::vector<std::string_view> second_to_last_phrases;
  // Said before a position and ignored: "the very last one".
  std::vector<std::string_view> position_intensifiers;

  // --- Lengths of time ------------------------------------------------------
  std::vector<TimeUnitWord> time_units;
  // "half a minute", "a minute and a half".
  std::vector<std::string_view> half_words;

  // --- Choice references (spoken_choice.h) ----------------------------------
  // Words that start a reference and say nothing: "the", "a".
  std::vector<std::string_view> articles;
  // Whole references to the current, next and previous item.
  std::vector<std::string_view> current_phrases;
  std::vector<std::string_view> next_phrases;
  std::vector<std::string_view> previous_phrases;
  // Cues: the words after them match the author fields ("by Queen") or
  // the source, date and other fields ("from the BBC", "from 1985").
  std::vector<std::string_view> by_cues;
  std::vector<std::string_view> from_cues;
  // Not description words (sorted). Role words are left out separately.
  std::vector<std::string_view> choice_stopwords;
  // Short words that count only when an item has them ("Log in" / "Log
  // out"), and weigh nothing when none does ("the one with the guitar").
  // Sorted.
  std::vector<std::string_view> choice_particles;
  std::vector<RoleWord> role_words;  // sorted by word

  // --- Clean-up before the grammar (voice_command.h) ------------------------
  // Hesitations, dropped wherever they are ("um", "uh").
  std::vector<std::string_view> fillers;
  // Politeness and preambles dropped from the start ("could you", "I want
  // to", "please"), and from the end ("please", "thank you").
  std::vector<std::string_view> leading_phrases;
  std::vector<std::string_view> trailing_phrases;
  // Dropped from the end too, but kept in free text that ends the
  // utterance ("play it now", but "search for Apocalypse Now").
  std::vector<std::string_view> trailing_if_command;
  // Dropped from the start only when more follows ("okay, louder").
  std::vector<std::string_view> leading_if_more;
  // Greetings before the wake word: "hey", "ok".
  std::vector<std::string_view> wake_greetings;
  // The wake phrase when ParseContext gives none.
  std::string_view default_wake_phrase;

  // --- Values in commands -------------------------------------------------
  // Volume words: "half" -> 50, "max" -> 100.
  std::vector<ValueWord> volume_words;
  // "percent", "per cent".
  std::vector<std::string_view> percent_words;
  // "out of ten": a volume on a scale of ten.
  std::vector<std::string_view> out_of_ten;
  // Scroll amounts: "a page", "half a page", "a bit" (half), "a screen".
  std::vector<std::string_view> page_words;   // "page", "pages", "screen"
  std::vector<std::string_view> scroll_half;  // "half a page", "a bit"
  std::vector<std::string_view> scroll_page;  // "a page", "one page"

  // --- Addresses ----------------------------------------------------------
  std::vector<AddressWord> address_words;
  // Dropped from the end of a site name: "the YouTube website".
  std::vector<std::string_view> site_suffixes;
  // Dropped from the start of a site name: "the".
  std::vector<std::string_view> site_prefixes;
};

// The tables of |language|, built on first use and kept.
const LanguagePhrases& PhrasesFor(Language language);

// spoken_phrases_en.cc's tables (PhrasesFor() keeps one copy).
LanguagePhrases MakeEnglishPhrases();

// --- Lookups shared by the readers ------------------------------------------

// The number of words of |phrase| (single spaces) if they are
// |words[at...]|, else 0.
size_t PhraseLengthAt(std::string_view phrase,
                      const WordList& words,
                      size_t at);
// The longest of |phrases| at |words[at]|, in words; 0 if none.
size_t LongestPhraseAt(const std::vector<std::string_view>& phrases,
                       const WordList& words,
                       size_t at);
// Whether |phrases| holds |phrase| (exactly).
bool HasPhrase(const std::vector<std::string_view>& phrases,
               std::string_view phrase);
// Binary search of a sorted list.
bool InSortedList(const std::vector<std::string_view>& sorted,
                  std::string_view word);
const NumberWord* FindNumberWord(const LanguagePhrases& phrases,
                                 std::string_view word);
const RoleWord* FindRoleWord(const LanguagePhrases& phrases,
                             std::string_view word);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_PHRASES_H_
