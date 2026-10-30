// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Personal words (docs/design/TOUCHLESS_BROWSING.md §6.8): the names a user
// says often (artists, contacts, sites) and their corrections ("no, I said
// Queen" after "Green" was heard), kept on the device as a small personal
// vocabulary.
//
//   - Names are noted from what the user asked for in commands (a search,
//     a site, "the one by ..."), never from dictation into a field, a
//     MoltNet window, or anything heard while the browser read a page
//     (the caller decides that: voice_learning_loop.h).
//   - Capped at kMaxVocabularyEntries entries (names and corrections
//     together); the least recently used go first, and anything unused for
//     kVocabularyLifetimeSeconds goes at the next Age().
//   - It biases the recognizer: HotwordPhrases() gives the corrections'
//     phrases and the names said at least kHotwordMinUses times, for the
//     speech helper's CONFIG next to the page's names.
//   - A correction fixes recognition directly: ApplyCorrections() replaces
//     the misheard words in a new utterance with the meant ones.
//   - Never learned: a phrase that is, or holds a word that is, an answer
//     to a question ("yes", "okay", "no": the answer that confirms an action
//     must not be biased toward or rewritten into), or one that looks secret
//     (a run of digits, letters mixed with digits, "password").
//
// Std-only (with the grammar): unit-tested, fuzzed through the store's
// parser (molt_voice_learning_fuzzer).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_VOCABULARY_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_VOCABULARY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai {
namespace voice {

// Names and corrections kept, at most.
inline constexpr size_t kMaxVocabularyEntries = 200;
// A phrase (a name, or either side of a correction): at most this many
// words and bytes, as the speech helper's hotwords (kMaxPhraseWords,
// kMaxPhraseBytes in voice_listener_protocol.h).
inline constexpr size_t kMaxVocabularyPhraseWords = 6;
inline constexpr size_t kMaxVocabularyPhraseBytes = 64;
// An entry unused this long is forgotten (180 days).
inline constexpr int64_t kVocabularyLifetimeSeconds = 180LL * 24 * 3600;
// A name said this often becomes a hotword (a correction at once).
inline constexpr int kHotwordMinUses = 2;
// At most this many of CONFIG's hotwords are personal; the page's names
// get the rest.
inline constexpr size_t kMaxPersonalHotwords = 16;
// "uses" is counted up to this, so a stored count can't overflow.
inline constexpr int kMaxVocabularyUses = 1000000;

struct VocabularyEntry {
  VocabularyEntry();
  ~VocabularyEntry();
  VocabularyEntry(const VocabularyEntry&);
  VocabularyEntry& operator=(const VocabularyEntry&);
  VocabularyEntry(VocabularyEntry&&);
  VocabularyEntry& operator=(VocabularyEntry&&);

  // As the user said it, cleaned, case kept ("Queen", "Bohemian
  // Rhapsody"); for a correction, what they meant.
  std::string phrase;
  // A correction: what the recognizer wrote instead, folded ("green");
  // empty for a name.
  std::string heard;
  int uses = 0;
  // Unix seconds.
  int64_t last_used = 0;

  bool correction() const { return !heard.empty(); }
};

// A correction lined up with what it corrects.
struct AlignedCorrection {
  AlignedCorrection();
  ~AlignedCorrection();
  AlignedCorrection(const AlignedCorrection&);
  AlignedCorrection& operator=(const AlignedCorrection&);
  AlignedCorrection(AlignedCorrection&&);
  AlignedCorrection& operator=(AlignedCorrection&&);

  // The words of the previous utterance that were misheard, folded.
  std::string heard;
  // What the user said they meant, cleaned, case kept.
  std::string meant;
  // The previous utterance with |heard| replaced by |meant|: what to do
  // now.
  std::string corrected;
};

class PersonalVocabulary {
 public:
  PersonalVocabulary();
  ~PersonalVocabulary();
  PersonalVocabulary(const PersonalVocabulary&);
  PersonalVocabulary& operator=(const PersonalVocabulary&);
  PersonalVocabulary(PersonalVocabulary&&);
  PersonalVocabulary& operator=(PersonalVocabulary&&);

  // A name the user said in a command, at |now|: added, or its use
  // counted. False (nothing changes) for a phrase LearnablePhrase()
  // refuses.
  bool NoteName(std::string_view phrase, int64_t now);
  // The user corrected |heard| (as recognized) to |meant|. False for a
  // pair either side of which LearnablePhrase() refuses, or that is the
  // same phrase.
  bool NoteCorrection(std::string_view heard,
                      std::string_view meant,
                      int64_t now);
  // A correction applied to a new utterance was used again.
  void NoteCorrectionUsed(std::string_view heard, int64_t now);

  // Forgets every name and correction |phrase| (folded) names, either
  // side of a correction included. How many went.
  size_t Forget(std::string_view phrase);
  void Clear();
  // Drops what was unused for kVocabularyLifetimeSeconds before |now| (and
  // anything dated in the future, past |now| + a day: a clock that moved),
  // then the least recently used beyond kMaxVocabularyEntries.
  void Age(int64_t now);

  // The phrases to bias the recognizer toward, most useful first: the
  // corrections' meant phrases, then names said at least kHotwordMinUses
  // times, each group most recently used first; at most |max|.
  std::vector<std::string> HotwordPhrases(size_t max) const;

  // |utterance| with each correction's misheard words (whole words, the
  // longest first, never overlapping and never applied to their own
  // output) replaced by what was meant; nullopt when none applies. The
  // caller doesn't apply it while a yes-or-no question waits, and drops
  // a result that reads as a yes or a no (voice_learning_loop.h). |used|
  // gets the heard side of each correction applied.
  std::optional<std::string> ApplyCorrections(
      std::string_view utterance,
      std::vector<std::string>* used = nullptr) const;

  // Adds |entry| as it was stored (the store's parser), if valid and not
  // already there; the caller ages the result.
  bool Restore(VocabularyEntry entry);

  const std::vector<VocabularyEntry>& entries() const { return entries_; }
  bool empty() const { return entries_.empty(); }

 private:
  VocabularyEntry* FindName(const std::string& key);
  VocabularyEntry* FindCorrection(const std::string& heard);
  void Evict();

  std::vector<VocabularyEntry> entries_;
};

// Whether |phrase| may be learned: valid text with a letter, 1 to
// kMaxVocabularyPhraseWords words and at most kMaxVocabularyPhraseBytes
// bytes once cleaned; no word of it, nor the whole, an answer to a yes-or-
// no question; nothing that looks secret (four digits or more in a row, a
// word mixing letters and digits, a word such as "password" or "PIN").
bool LearnablePhrase(std::string_view phrase);

// The phrase as it is kept: cleaned (CleanSpokenText()), single spaces;
// empty when LearnablePhrase() refuses it.
std::string LearnedPhrase(std::string_view phrase);

// Lines "no, I said |said|" up with |previous|, the utterance it corrects.
//   - |said| is a whole command sharing words at either end with
//     |previous| ("no, I said open YouTube" after "open you tube"): the
//     words in between differ;
//   - else |said| is the words meant ("no, I said Queen" after "play the
//     live one by Green"): the span of |previous| that sounds most like it
//     (phonetic_key.h), at most one word longer or shorter.
// nullopt when nothing lines up (|said| then stands on its own), or the
// two sides are the same.
std::optional<AlignedCorrection> AlignCorrection(std::string_view previous,
                                                 std::string_view said);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_VOCABULARY_H_
