// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A sound-alike key for Spanish words, in the sound classes of
// phonetic_key.h's English key, so PhoneticDistance(), PhoneticBound() and
// PhoneticSimilarity() compare Spanish keys unchanged.
//
// The English key reads Spanish wrongly where English spelling differs:
// a final "e" is silent in English but said in Spanish ("molde" would be
// "molt"'s key), "h" is silent in Spanish, "qu" and "gu" before e or i
// are a plain k and g ("queso", "guerra"), "ll" and "y" before a vowel
// are one sound, "j" and "g" before e or i are the same sound ("gente",
// "jefe"). Spanish spelling is close to its sounds, so the rules are few:
//
//   a  a run of vowels (a e i o u, and a final "y": "hoy")
//   P  p b v f            T  t d            K  k c(hard) q g(hard) j
//      g(before e, i)     S  s z c(before e, i) x(after the first letter: k s)
//   X  ch                 M  m      N  n (ñ is folded to n)
//   L  l                  R  r rr   Y  ll, y before a vowel
//   W  w, "hu" before a vowel at the start ("huevo")
//
// Silent: "h" (but "ch"), the "u" of "qu" and of "gu" before e or i, a "p"
// before an initial "s" ("psicología"). Repeats collapse as in English.
// Like the English key, only lower-case ASCII words (spoken_text.h folds
// accents and "ñ") of at most kMaxPhoneticWordBytes have a key.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_ES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_ES_H_

#include <string>
#include <string_view>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// The Spanish key of |word|; "" when it has none.
std::string SpanishPhoneticKey(std::string_view word);

// |word|'s key in |language|: PhoneticKey() for English,
// SpanishPhoneticKey() for Spanish.
std::string PhoneticKeyFor(Language language, std::string_view word);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_PHONETIC_KEY_ES_H_
