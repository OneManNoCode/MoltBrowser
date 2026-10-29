// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Text as a speech recognizer writes it, cut into the words voice mode
// compares: the direct-command grammar (voice_command.h) and the
// spoken-choice matcher (spoken_choice.h) both start here
// (docs/design/TOUCHLESS_BROWSING.md sections 6.2 and 6.4).
//
// NormalizeSpokenText() reads any bytes in one linear pass:
//   - UTF-8 is decoded strictly. Each invalid sequence counts as a word
//     break and is reported (|had_invalid_utf8|); the grammar refuses such
//     text, the matcher reads around it.
//   - Words are runs of letters and digits, folded for comparison: ASCII
//     and fullwidth forms to lower-case ASCII, Latin-1 and Latin
//     Extended-A letters to their base letters ("Beyoncé" -> "beyonce",
//     "Straße" -> "strasse", "Łódź" -> "lodz"), combining accents dropped,
//     Greek and Cyrillic capitals lowered. Other scripts are kept as they
//     are (they get no phonetic key, so they only ever match exactly).
//   - An apostrophe inside a word is dropped ("what's" -> "whats").
//     Invisible format characters (zero-width, bidi controls, soft hyphen,
//     variation selectors, BOM, tag characters) are dropped without
//     breaking the word, so "you​tube" is "youtube". Everything else
//     (spaces, controls, punctuation, symbols, emoji) breaks words.
//   - Digit groups stay one number ("1,000" -> "1000").
//   - Each word remembers the bytes it came from and the punctuation that
//     glued it to the previous word ("example.com": "com" is glued by '.'),
//     so a slot can be read back as written (SourceText()) and an address
//     can be rebuilt.
//   - With |marks| (the grammar), "%" after a number becomes the word
//     "percent", "#" before one and "No." before one become "number".
//
// Pure, std-only and linear: the unit tests, the fuzzers and a standalone
// harness build exactly this code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_TEXT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_TEXT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// The languages voice mode understands. Each has its phrase tables
// (spoken_phrases.h, voice_command_phrases.h).
enum class Language {
  kEnglish,
};

// "en", "en-US", "en_GB" (any case) -> kEnglish; nullopt for a language
// voice mode has no tables for.
std::optional<Language> LanguageFromTag(std::string_view tag);

// Folded words, as the readers (spoken_numbers.h, spoken_choice.h,
// voice_command.h) take them.
using WordList = std::vector<std::string_view>;

struct SpokenWord {
  // The folded word: offsets into NormalizedText::folded.
  uint32_t begin = 0;
  uint32_t size = 0;
  // The bytes of the source it came from, [source_begin, source_end).
  // A word the normalizer made up ("percent" for '%') points at the mark.
  uint32_t source_begin = 0;
  uint32_t source_end = 0;
  // '.', '/', '-', '_', ':' or '@' when that one character, and nothing
  // else, separates this word from the previous one in the source; 0
  // otherwise.
  char joiner = 0;
  // The first character after the word when it is ASCII punctuation
  // (before any space): '.' for "No." and "end.", ',' for "no,". 0
  // otherwise.
  char trailing = 0;
  // Only ASCII digits.
  bool numeric = false;
  // Only ASCII letters and digits after folding (what phonetic keys read).
  bool ascii = false;
};

struct NormalizedText {
  NormalizedText();
  ~NormalizedText();
  NormalizedText(const NormalizedText&);
  NormalizedText& operator=(const NormalizedText&);
  NormalizedText(NormalizedText&&);
  NormalizedText& operator=(NormalizedText&&);

  std::string_view Word(size_t i) const {
    return std::string_view(folded).substr(words[i].begin, words[i].size);
  }

  // The folded words, back to back (no separators).
  std::string folded;
  std::vector<SpokenWord> words;
  bool had_invalid_utf8 = false;
  // More than the |max_words| asked for: the rest were not read.
  bool truncated = false;
};

struct NormalizeOptions {
  // Turn '%', '#' and "No." into words (see above).
  bool marks = false;
  // Words read at most; a longer text sets |truncated|.
  size_t max_words = 512;
};

NormalizedText NormalizeSpokenText(std::string_view text,
                                   const NormalizeOptions& options);

// Whether |text| is valid UTF-8 (no overlong forms, surrogates or code
// points past U+10FFFF).
bool IsValidUtf8(std::string_view text);

// |text| made safe to show and to hand to the planner: every C0 and C1
// control, bidi control, zero-width and other invisible format character
// removed, each run of white space made one space, and trimmed. Invalid
// UTF-8 sequences become U+FFFD. At most |max_bytes| bytes, never cut
// inside a character.
std::string CleanSpokenText(std::string_view text, size_t max_bytes);

// The source text of |source| that words [first, last] of |text| came
// from, as written ("AC/DC", "node.js", "Bohemian Rhapsody"), cleaned as
// CleanSpokenText() does. Words in |skip| (indices, sorted) are left out
// with a space in their place (fillers such as "um").
std::string SourceText(std::string_view source,
                       const NormalizedText& text,
                       size_t first,
                       size_t last,
                       const std::vector<size_t>& skip);

// The folded words of |text| joined by single spaces ("Beyoncé - Halo" ->
// "beyonce halo"): a name compared as voice mode compares it.
std::string FoldForComparison(std::string_view text);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_TEXT_H_
