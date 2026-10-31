// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The language voice mode understands and speaks
// (docs/design/TOUCHLESS_BROWSING.md §4.1): which one, from the setting
// (voice_language_settings.h) and the browser's own language, and what
// voice mode can do in it. Std-only and unit-tested.
//
// The grammar, the phrases, the browser's spoken answers and the voice it
// speaks with follow the language. Listening needs the speech helper to
// have a recognizer for it: only English has one so far.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/voice/spoken_text.h"

namespace molt_ai {
namespace voice {

// The language a stored setting chose: "en" or "es" exactly; nullopt for
// anything else (missing, another language, any other text), which means
// the browser's own language.
std::optional<Language> StoredVoiceLanguage(std::string_view stored);

// The language voice mode uses: the stored choice, else |ui_locale|'s ("es",
// "es-419", "en-GB") when voice mode has it, else English.
Language VoiceLanguageFor(std::string_view stored, std::string_view ui_locale);

// Whether voice mode can listen in |language|: whether the speech helper
// has a recognizer for it. English only for now (no Spanish model is
// bundled yet: TOUCHLESS_BROWSING.md §9).
bool ListeningAvailable(Language language);

// What voice mode says, in |language|, when it is asked to listen in a
// language it can't listen in yet; "" when it can.
std::string_view ListeningUnavailableNotice(Language language);

// The language's name for the settings page ("English", "Español").
std::string_view LanguageDisplayName(Language language);

// The language tag voice mode's platform utterances carry: "en-US" for
// English (as always), and for Spanish the browser's own Spanish when its
// language is one ("es-MX"), else "es".
std::string PlatformLanguageTag(Language language, std::string_view ui_locale);

// A voice of the platform's speech engine (content::VoiceData's parts).
struct PlatformVoice {
  std::string name;
  std::string lang;  // "es-ES", "es_MX", "es"; may be empty
  bool remote = false;
};

// The platform voice voice mode speaks |language| with: a voice on this
// computer (never a remote one, which would send the text off it) whose
// language is |language|, the one of |tag|'s region first ("es-MX"), then
// any of the language, in the platform's order. nullopt when there is none:
// the platform's default voice then reads the text, with its own
// language's sounds, and the settings page says so.
std::optional<size_t> PickPlatformVoice(
    const std::vector<PlatformVoice>& voices,
    Language language,
    std::string_view tag);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_H_
