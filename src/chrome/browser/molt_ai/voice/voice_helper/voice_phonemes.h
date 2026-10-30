// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's text front end, second half: the words of a sentence
// (voice_text.h) as the phonemes the voice speaks (docs/design/
// VOICE_HELPER_PROTOCOL.md §4). sherpa-onnx 2.x reads English only through
// a lexicon and silently skips a word it doesn't have, so the helper hands
// it phonemes it made itself instead, for every word:
//
//   1. a few function words as said in running text ("a", "or", "her",
//      which the lexicon lacks or gives as letter names);
//   2. the lexicon (sherpa-onnx's lexicon-en-us.txt: CMUdict and espeak-ng's
//      word list, 168,000 words, in espeak-ng's IPA, which the Piper voice
//      was trained on);
//   3. acronyms ("GPU", "HTML") and words without vowels spelled out;
//   4. its forms: "'s", plurals, "-ed", "-ing", "-er", "-ly", "-ness" and
//      the like, and prefixes ("un-", "re-") of a word the lexicon has;
//   5. joined words: "MoltBrowser", "QuietComfort", "chillhop";
//   6. letter-to-sound rules (voice_lts.h).
//
// Every code point that reaches the voice is one of its tokens
// (tokens.txt); anything else is dropped here.
//
// Std-only, no exceptions; bounded work per word (at most kMaxWordLetters
// letters, two levels of splitting).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_PHONEMES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_PHONEMES_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/voice_helper/voice_text.h"

namespace molt_voice {

// The lexicon file is 6.3 MB.
inline constexpr size_t kMaxLexiconBytes = 16 << 20;
// One unit of synthesis, in phoneme code points: a longer sentence is
// split at a pause or between words.
inline constexpr size_t kMaxUnitCodePoints = 320;

// sherpa-onnx's English lexicon format: "word || p h o n e s" per line,
// "#" comments. Only entries whose word is ASCII lower-case letters and
// apostrophes are kept (the others can't come out of voice_text.h).
class Lexicon {
 public:
  Lexicon();
  Lexicon(const Lexicon&) = delete;
  Lexicon& operator=(const Lexicon&) = delete;
  Lexicon(Lexicon&&);
  Lexicon& operator=(Lexicon&&);
  ~Lexicon();

  // Takes the file. False, with |error|, if it is too big, isn't UTF-8, or
  // has a line that isn't an entry or a comment.
  bool Load(std::string data, std::string* error);

  // The phones of |word| (lower case) as UTF-8 IPA without spaces, or "".
  std::string Find(std::string_view word) const;
  bool Has(std::string_view word) const;
  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    uint32_t word_offset = 0;
    uint32_t word_size = 0;
    uint32_t phones_offset = 0;
    uint32_t phones_size = 0;
  };

  std::string_view WordOf(const Entry& entry) const;
  const Entry* Lookup(std::string_view word) const;

  std::string data_;
  std::vector<Entry> entries_;  // sorted by word
};

// tokens.txt of a Piper voice: "<symbol> <id>" per line, one code point per
// symbol (a line of just an id is the space). The code points, in |tokens|.
bool LoadVoiceTokens(std::string_view text,
                     std::vector<uint32_t>* tokens,
                     std::string* error);

class Phonemizer {
 public:
  // |tokens|: the code points the voice knows (empty: all).
  Phonemizer(Lexicon lexicon, std::vector<uint32_t> tokens);
  Phonemizer(const Phonemizer&) = delete;
  Phonemizer& operator=(const Phonemizer&) = delete;
  ~Phonemizer();

  // A sentence as phonemes: words separated by spaces, pauses as ",", the
  // sentence's own mark at the end. Empty if nothing in it can be said.
  std::vector<int32_t> Sentence(const SpokenSentence& sentence) const;

  // Phonemes in units of at most kMaxUnitCodePoints, cut after a pause if
  // there is one in the second half of the unit, else between words, else
  // anywhere (a unit cut mid-sentence ends in ",").
  static std::vector<std::vector<int32_t>> Units(
      const std::vector<int32_t>& phonemes);

  // One word (ASCII letters and inner apostrophes, any case) as UTF-8 IPA;
  // |all_caps_sentence| says the words around it are capitals too (a
  // title written in capitals: "GET" is a word there, not G E T).
  std::string Word(std::string_view word, bool all_caps_sentence) const;
  // Letters spelled out ("g p u"), each its own word, as UTF-8 IPA.
  std::string Spell(std::string_view letters) const;

  const Lexicon& lexicon() const { return lexicon_; }

 private:
  std::string WordAt(std::string_view word,
                     bool all_caps_sentence,
                     int depth) const;
  // |lower| from the lexicon, or one of its forms; "" if neither.
  std::string Known(std::string_view lower, int depth) const;
  std::string Forms(std::string_view lower) const;
  std::string Compound(std::string_view lower, int depth) const;
  void AppendFiltered(std::string_view phones, std::vector<int32_t>* out) const;

  Lexicon lexicon_;
  // Sorted.
  std::vector<uint32_t> tokens_;
};

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_PHONEMES_H_
