// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's speech rate as a setting (docs/design/TOUCHLESS_BROWSING.md
// §5.4): "speak faster", "slower", and "faster" and "slower" while a page is
// read, set it, and it is kept with MoltBrowser's other settings in
// ~/.moltbrowser/settings.json (as voice_session_settings.h keeps voice
// mode's):
//
//   "voice_speech_rate"  a number from kMinSpeechRate to kMaxSpeechRate
//                        (voice_controller_logic.h); default 1
//
// A value out of range, or not a number, reads as the default. Blocking
// (file I/O): call on a thread that may block.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READING_SETTINGS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READING_SETTINGS_H_

namespace molt_ai {
namespace voice {

inline constexpr char kVoiceSpeechRateSettingsKey[] = "voice_speech_rate";

double ReadVoiceSpeechRate();

// Sets the key and keeps every other one. False, and nothing written, if
// settings.json exists but isn't a JSON object, or can't be written.
bool WriteVoiceSpeechRate(double rate);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_READING_SETTINGS_H_
