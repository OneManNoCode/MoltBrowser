// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The voice gesture's confirmation words in Spanish
// (docs/design/TOUCHLESS_BROWSING.md §7.3; voice_gesture.h has English's
// and the rules): "ejemplo.com quiere usar tu cámara. Para permitirlo, di
// manzana. Para bloquearlo, di no."
//
// Chosen as English's were: 40 short everyday nouns, no two of which are
// within the phonetic key's bound plus a close sound of each other, and
// none within its bound of a word voice mode answers with in Spanish (the
// answers, the words said around them, the numbers and ordinals up to
// twenty). Spanish words have fewer distinct consonants than English ones,
// so many candidates fall out; the list holds under both keys, the English
// one voice_gesture.h compares with today and the Spanish one
// (phonetic_key_es.h) it should compare Spanish with. Each word's plural
// adds "s" or "es" ("manzanas", "camiones"), so a matcher can take it off
// exactly, as voice_gesture.h does for English.
//
// Not yet measured with a Spanish recognizer: no Spanish model is bundled
// (TOUCHLESS_BROWSING.md §9). Std-only.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_CONFIRMATION_WORDS_ES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_CONFIRMATION_WORDS_ES_H_

#include <span>
#include <string_view>

namespace molt_ai {
namespace voice {

// The confirmation words, folded (lower-case ASCII, no accents: "platano"
// for "plátano").
std::span<const std::string_view> SpanishConfirmationWords();

// What voice mode answers its prompts with in Spanish ("sí", "no",
// "bloquear", "salir", "quedarme", "guardar", "nunca", "descartar"...), the
// words around them, and the numbers "uno" to "doce": no confirmation word
// may sound like one.
std::span<const std::string_view> SpanishAnswerWords();

// Said around the word and ignored: "sí, manzana", "di manzana", "la
// manzana"; "manzana, por favor".
std::span<const std::string_view> SpanishConfirmationLeading();
std::span<const std::string_view> SpanishConfirmationTrailing();

// Whether either phonetic key (English's, Spanish's) could take |a| for
// |b|: their keys equal or within PhoneticBound() plus |margin| half-edits.
bool SpanishWordsConfusable(std::string_view a, std::string_view b, int margin);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_CONFIRMATION_WORDS_ES_H_
