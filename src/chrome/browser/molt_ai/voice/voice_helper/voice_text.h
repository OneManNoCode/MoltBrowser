// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's text front end, first half: what the browser asks the voice
// to say, as the words a person reading it aloud would say
// (docs/design/VOICE_HELPER_PROTOCOL.md §4). The text is page-derived and
// untrusted: any bytes at all, at most kMaxSpeakTextBytes.
//
//   - Unicode: invalid UTF-8, control and format characters (bidirectional
//     overrides, zero-width characters) and combining marks go; Latin
//     letters with accents lose them ("Beyoncé" is "Beyonce"); quotes,
//     dashes and spaces become their ASCII forms; other scripts, emoji and
//     symbols separate words and are not read.
//   - Numbers: "1,234" "one thousand two hundred thirty four", "3.5"
//     "three point five", "-5", "1st" "first", "50%", "$4.99", "1.2M"
//     "one point two million", "1985" (alone) "nineteen eighty five",
//     "1980s", "2026-10-30" and "10/30/2026" "October thirtieth, twenty
//     twenty six", "6:07" "six oh seven", "1:02:03" "one hour two minutes
//     three seconds", "10GB", a long string of digits digit by digit.
//   - Words: "Mr." "mister", "e.g." "for example", "vs" "versus", "&"
//     "and", "U.S." spelled out, "mp3" "m p three", "C++" "c plus plus".
//   - Addresses: "https://www.example.com/path?q=1" "example dot com",
//     "someone@example.org" "someone at example dot org".
//   - Sentences end at ". ! ?"; ", ; : ( ) -" and the like are pauses.
//
// Everything is linear in the input and bounded: at most kMaxSpokenWords
// words, kMaxWordLetters letters a word, kMaxDigitsRead digits a number.
// Std-only, no exceptions: molt_ai_unittests, the helper (CMake) and
// molt_voice_helper_fuzzer build the same code.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_TEXT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_TEXT_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace molt_voice {

// A SPEAK frame's text, in bytes.
inline constexpr size_t kMaxSpeakTextBytes = 4096;
// Words said for one SPEAK, pauses included; the rest is not said.
inline constexpr size_t kMaxSpokenWords = 1500;
// A word longer than this is cut.
inline constexpr size_t kMaxWordLetters = 40;
// Digits read in one number; more are not said.
inline constexpr size_t kMaxDigitsRead = 24;
// Integers up to this many digits are read as numbers ("one million"); a
// longer one, or one with leading zeros, digit by digit.
inline constexpr size_t kMaxDigitsAsNumber = 15;

enum class WordKind : uint8_t {
  // Said as a word: the lexicon, else letter-to-sound (voice_phonemes.h).
  kWord,
  // Said letter by letter ("GPU", "U.S.").
  kSpell,
  // A short pause (a comma's).
  kPause,
};

struct SpokenWord {
  WordKind kind = WordKind::kWord;
  // kWord: ASCII letters, with apostrophes only between letters, case as
  // written ("YouTube", "don't"). kSpell: lower-case ASCII letters. kPause:
  // empty.
  std::string text;
};

struct SpokenSentence {
  SpokenSentence();
  SpokenSentence(const SpokenSentence&);
  SpokenSentence& operator=(const SpokenSentence&);
  SpokenSentence(SpokenSentence&&);
  SpokenSentence& operator=(SpokenSentence&&);
  ~SpokenSentence();

  // Never starts or ends with a pause, never has two pauses in a row, and
  // never is empty.
  std::vector<SpokenWord> words;
  // How it ends: '.', '?' or '!'.
  char end = '.';
};

// |text| (any bytes; only the first kMaxSpeakTextBytes are read) as
// sentences to say.
std::vector<SpokenSentence> NormalizeForSpeech(std::string_view text);

// Numbers as words, each word separated by one space: "one thousand two
// hundred thirty four". Up to 999 trillion; larger is read digit by digit.
std::string CardinalWords(uint64_t n);
// "first", "twenty second", "one hundredth".
std::string OrdinalWords(uint64_t n);
// A year as said: "nineteen eighty five", "two thousand five", "twenty
// twenty six", "nineteen hundred", "nineteen oh five".
std::string YearWords(int year);

// The whole text after the Unicode step above: printable ASCII and spaces,
// with currency and degree signs as the private bytes 0x01 to 0x06 (for
// tests and the fuzzer).
std::string FoldToAscii(std::string_view text);

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_TEXT_H_
