// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's language as a setting (voice_language.h), kept with
// MoltBrowser's other settings in ~/.moltbrowser/settings.json, as
// voice_session_settings.h keeps voice mode's switches:
//
//   "voice_language"  "en" or "es"; missing (or anything else): the
//                     browser's own language when voice mode has it, else
//                     English
//
// molt://ai-settings ("Voice mode", "Language") changes it; the voice loop
// (voice_controller.h) reads it when a window's voice mode is made, and is
// told when it changes. Blocking (file I/O): read on a thread that may
// block; write on MoltSettingsFileTaskRunner() (common/molt_settings_file.h)
// only.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_SETTINGS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_SETTINGS_H_

#include <string>

namespace molt_ai {
namespace voice {

inline constexpr char kVoiceLanguageSettingsKey[] = "voice_language";

// The stored value ("en", "es"), or "" when the setting is missing, isn't
// one of those, or settings.json can't be read.
std::string ReadVoiceLanguageSetting();

// |code| "en" or "es" stores that language; "" removes the key (the
// browser's language). Every other key is kept, atomically. False, and
// nothing written, for any other |code|, or if settings.json exists but
// isn't a JSON object, or can't be written.
bool WriteVoiceLanguageSetting(const std::string& code);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_LANGUAGE_SETTINGS_H_
