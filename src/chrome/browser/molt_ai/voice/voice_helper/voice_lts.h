// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// molt-voice's letter-to-sound rules: an English word the lexicon doesn't
// have ("Eilish", "Sennheiser") as the IPA the voice was trained on
// (espeak-ng's symbols for American English), so it is said, not skipped.
//
// The rules are in the style of the Naval Research Laboratory's
// letter-to-sound rules (Elovitz, Johnson, McHugh and Shore, NRL Report
// 7948, 1976, a US government work): for each letter, rules of the form
// "left context | letters | right context -> sounds", the first that
// matches wins. They are a fallback: the lexicon (voice_phonemes.h) has
// 168,000 words and its morphology and compounds come first.
//
// Std-only, no exceptions; linear in the word (which is at most
// kMaxWordLetters letters).

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_LTS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_LTS_H_

#include <string>
#include <string_view>

namespace molt_voice {

// |word|'s sounds as UTF-8 IPA, primary stress (U+02C8) before the first
// vowel. Letters other than a-z (either case) are skipped; "" if none is
// left.
std::string LetterToSound(std::string_view word);

// Whether |phone| (UTF-8, the start of a sound) begins a vowel.
bool StartsWithVowelSound(std::string_view phones);

}  // namespace molt_voice

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_HELPER_VOICE_LTS_H_
