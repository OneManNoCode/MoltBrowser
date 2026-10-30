// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Text in and out of molt-speech's engines:
// - the models' vocabularies: word pieces with scores (the bpe.vocab files
//   make_speech_vocab.py writes) and token lists (tokens.txt);
// - the wake phrase as a keyword line for sherpa-onnx's spotter, cut into
//   the spotter's pieces the way its SentencePiece model cuts words
//   (highest-scoring split, as docs/VOICE_BENCH.md §3.8 found);
// - hotwords from page text, cleaned so that sherpa-onnx's hotword parser
//   sees no syntax and no word the recognizer can't spell
//   (docs/VOICE_BENCH.md §5.3);
// - recognized text for events: valid UTF-8, no control characters, capped.
// Std-only, no exceptions.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_TEXT_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_TEXT_H_

#include <stddef.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace molt_speech {

// SentencePiece's word-start mark, U+2581.
inline constexpr std::string_view kWordStart = "\xE2\x96\x81";

// Hotword limits (docs/design/SPEECH_HELPER_PROTOCOL.md §2.2).
inline constexpr size_t kMaxHotwordPhrases = 200;
inline constexpr size_t kMaxHotwordWords = 6;
inline constexpr size_t kMaxHotwordPhraseBytes = 64;
inline constexpr size_t kMaxHotwordsBytes = 4096;

// The spotter's threshold for every wake phrase (docs/VOICE_BENCH.md §3.8:
// the lowest measured, 12% of clean wake phrases missed, about two false
// wake-ups an hour from sound-alike chatter, none from page audio).
inline constexpr std::string_view kWakeThreshold = "0.15";

// Word pieces and their scores.
class PieceVocab {
 public:
  PieceVocab();
  PieceVocab(const PieceVocab&);
  PieceVocab& operator=(const PieceVocab&);
  ~PieceVocab();

  // Reads "<piece> <score>" lines (space or tab; the score a decimal with
  // an optional sign and fraction). False, with |error|, on a malformed
  // line, a duplicate piece, a piece over 64 bytes or more than 100,000
  // pieces.
  bool Load(std::string_view text, std::string* error);

  // The highest-scoring split of kWordStart + |word| into pieces; on a tie
  // the longer first piece. False if no split exists or |word| is empty or
  // over 64 bytes.
  bool Split(std::string_view word, std::vector<std::string>* pieces) const;

  bool Contains(std::string_view piece) const;
  size_t size() const { return scores_.size(); }

 private:
  std::unordered_map<std::string, double> scores_;
  size_t longest_ = 0;
};

// The tokens of a tokens.txt ("<token> <id>" per line).
using TokenSet = std::unordered_set<std::string>;
bool LoadTokenSet(std::string_view text, TokenSet* tokens, std::string* error);

// The spotter's keyword line for a normalized wake phrase
// (NormalizeWakePhrase()): each word upper-cased and split into |vocab|'s
// pieces, every one of which must be in |tokens|, then the threshold and
// the phrase as its label, e.g.
//   "▁HE Y ▁M OL T #0.15 @HEY_MOLT".
// False if a word can't be spelled; the spotter would refuse the line (and
// end the process).
bool WakeKeywordLine(std::string_view phrase,
                     const PieceVocab& vocab,
                     const TokenSet& tokens,
                     std::string* line);

// sherpa-onnx's per-stream hotwords argument for |phrases| as the browser
// sent them: phrases joined by '/', each cleaned
// (docs/design/SPEECH_HELPER_PROTOCOL.md §2.2): anything but an ASCII
// letter, digit or apostrophe separates words; a word must have a letter
// or digit, and every character of it and the word-start mark must be a
// piece of |vocab| that is also in |tokens| (so sherpa-onnx's own split of
// it never fails or yields <unk>); at most kMaxHotwordWords words and
// kMaxHotwordPhraseBytes per phrase, kMaxHotwordPhrases phrases and
// kMaxHotwordsBytes in all; no duplicates. Case is kept. Empty if nothing
// is left.
std::string HotwordsArgument(const std::vector<std::string>& phrases,
                             const PieceVocab& vocab,
                             const TokenSet& tokens);

// Text for an event: well-formed UTF-8 only (other bytes dropped), no
// control characters (C0, DEL, C1, line and paragraph separators), runs of
// white space as one space, trimmed, at most |max_bytes|: cut at the last
// space in the final 64 bytes if there is one, else at a character
// boundary.
std::string CleanEventText(std::string_view text, size_t max_bytes);

}  // namespace molt_speech

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPEECH_HELPER_SPEECH_TEXT_H_
