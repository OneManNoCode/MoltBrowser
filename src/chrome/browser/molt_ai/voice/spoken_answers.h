// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the browser says, per language (docs/design/TOUCHLESS_BROWSING.md
// §4.1). The English sentence is the key, as gettext's message ids are: the
// voice loop keeps its English sentences as written, and each other
// language has a catalog that gives its own sentence for an English one.
//
//   - A whole sentence: Localized(kSpanish, "Zoomed in.") is "Zoom
//     ampliado."; the voice loop's Say() and Status() pass everything they
//     say through it, so a constant sentence needs nothing at its call.
//   - A sentence with words that change (a page's name, a number): the
//     English sentence with {0} ... {9} where they go,
//     Said(language, "Playing {0}.", {name}). In English it is exactly the
//     sentence the words make; in Spanish, "Reproduciendo {0}." with them.
//   - A sentence the catalog lacks is said as it is, in English;
//     tools/voice/check_spoken_answers.py lists the loop's sentences and
//     fails on one with no Spanish.
//
// What goes in a placeholder is inserted as it is, once: a "{0}" inside
// page text stays text. Std-only and linear in the sentence and its words.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_ANSWERS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_ANSWERS_H_

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// One sentence of a catalog: the English sentence (with its placeholders)
// and the language's.
struct SpokenAnswer {
  std::string_view english;
  std::string_view text;
};

// |language|'s catalog, sorted by |english| (empty for English).
const std::vector<SpokenAnswer>& SpokenAnswers(Language language);

// spoken_answers_es.cc's catalog.
std::vector<SpokenAnswer> MakeSpanishSpokenAnswers();

// |english| in |language|: the catalog's sentence, or |english| itself
// (always in English, and when the catalog lacks it).
std::string_view Translated(Language language, std::string_view english);

// |text|, a whole sentence or a few, in |language| (Translated()).
std::string Localized(Language language, std::string_view text);

// |english| in |language| with |args| in its placeholders: "{0}" is
// args[0], up to "{9}"; "{{" is "{". A placeholder with no argument is
// left out.
std::string Said(Language language,
                 std::string_view english,
                 std::initializer_list<std::string_view> args);

// The browser's own English sentence |english|, made by Said(kEnglish, ...)
// where the language wasn't known (a routine's file read, the activation
// guard's notice, a helper's reason), in |language|: the catalog's
// sentence for it whole, else for the catalog key it was made from, with
// the words that filled the key's placeholders. |english| as it is when
// no key made it, when which key made it can't be told (another key, with
// more text of its own, fits because of what is in a placeholder, but
// doesn't start and end the text as much as one that also fits), and
// always in English. Never for page text or model output alone: only what
// the browser composed from a key. At most
// kMaxRelocalizedBytes are matched (a longer text is returned as it is);
// linear in the text for each key.
inline constexpr size_t kMaxRelocalizedBytes = 2048;
std::string Relocalized(Language language, std::string_view english);

// Marks |english|, a constant sentence or phrase of the browser's made
// where the language isn't known, as one said later in the window's
// language (Localized(), Relocalized()): the catalog's check
// (tools/voice/check_spoken_answers.py) wants it in the catalog. Returns
// |english|.
constexpr std::string_view SaidLater(std::string_view english) {
  return english;
}

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_SPOKEN_ANSWERS_H_
