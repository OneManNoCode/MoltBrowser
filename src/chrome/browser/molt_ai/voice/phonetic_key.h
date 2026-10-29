// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A sound-alike key for English words, so that what a recognizer wrote
// still matches what is on screen ("rap city" / "rhapsody", "Quinn" /
// "Queen", "mold" / "molt"). MoltBrowser's own scheme, written for
// recognition errors rather than for spelling variants of names:
//
//  1. Only lower-case ASCII words have a key (spoken_text.h folds Latin
//     letters to ASCII first). Words with digits or other scripts have
//     none and only ever match exactly.
//  2. The word is read left to right into sound classes, one letter per
//     class:
//       a  any run of vowels (a e i o u, and y when it isn't a consonant)
//       P  p b f v ph (lips)       T  t d th (tongue tip)
//       K  k c(hard) q g ck x(1st) S  s z c(soft: before e, i, y) x(2nd)
//       X  sh ch j tch dge -ge tion sion (hushing)
//       M  m     N  n, final ng, initial kn gn pn
//       L  l     R  r, initial wr rh
//       W  w before a vowel, wh, the u of qu
//       Y  y before a vowel at the start
//       H  h at the start of a syllable (before a vowel, not after one)
//     Silent letters are skipped: a final e after a consonant (when the
//     word has another vowel), gh inside a word ("night"), b after a
//     final m ("climb"), g before a final n ("sign"), w and h after a
//     vowel ("cow", "oh").
//  3. Repeats of a class collapse ("ll", "ck", "pf" are one sound).
//
// Keys are compared by PhoneticDistance(): a weighted edit distance, in
// half-edits, inside a diagonal band. Recognition errors mostly confuse
// close sounds, so those cost less:
//   - the same class: 0;
//   - M/N, L/R, S/X, K/X, K/S, T/S, P/W (nasals, liquids, hushing and
//     hissing, the two sounds of c, th/s, v/w): 1;
//   - any other two classes: 3 (a recognizer that heard the word rarely
//     swaps a sound for an unrelated one);
//   - inserting or deleting a vowel or a W, Y or H: 1; anything else: 2.
// Keys are joined (JoinPhoneticKeys()) to compare words a recognizer split
// or merged: "rap"+"city" is "RaP"+"SaTa" = "RaPSaTa", the key of
// "rhapsody".

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace molt_ai {
namespace voice {

// Words longer than this get no key (no English word is; it bounds work).
inline constexpr size_t kMaxPhoneticWordBytes = 40;
// The widest band PhoneticDistance() searches.
inline constexpr int kMaxPhoneticBand = 8;

// The key of |word|; "" when it has none (see 1. above).
std::string PhoneticKey(std::string_view word);

// Whether two sound classes are the same or close (see above).
bool SameOrCloseSounds(char a, char b);

// |a| followed by |b|, with a repeat at the seam collapsed.
std::string JoinPhoneticKeys(std::string_view a, std::string_view b);

// The cheapest edit from |a| to |b| in half-edits (see above), or
// |bound| + 1 when it is more than |bound|. Time O(min(|a|, |b|) * bound).
int PhoneticDistance(std::string_view a, std::string_view b, int bound);

// The most PhoneticDistance() a pair of keys may have and still match:
// half an edit per two letters of the longer key, at most 5 (two and a
// half edits). Keys of one or two letters must be equal; a three-letter
// key allows one close sound or one vowel.
int PhoneticBound(size_t longer_key_size);

// How alike two keys sound, in [0, 0.9]: cap * (1 - distance / longer key
// length) up to the bound, 0 past it. The cap is 0.9, but 0.8
// for four-letter keys and 0.7 for shorter ones: short keys collide more
// ("live", "life" and "love" are all "LaP"). Always below an exact (1.0)
// or plural (0.95) spelling match, so spelling wins ties.
double PhoneticSimilarity(std::string_view a, std::string_view b);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_H_
