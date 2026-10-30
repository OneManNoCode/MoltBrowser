// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Voice mode's settings (docs/VOICE_COMMANDS.md, "Turning voice mode on"),
// kept with MoltBrowser's other settings in ~/.moltbrowser/settings.json:
//
//   "voice_mode"              start listening with the browser (and the
//                             settings switch turns listening on and off
//                             now); default false
//   "voice_always_listening"  every utterance is a command, no wake phrase
//                             needed; default false
//
// molt://ai-settings ("Voice mode", behind chrome://flags/#molt-voice-mode)
// changes them; voice_session.h reads them.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_SETTINGS_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_SETTINGS_H_

namespace molt_ai {
namespace voice {

inline constexpr char kVoiceModeSettingsKey[] = "voice_mode";
inline constexpr char kVoiceAlwaysListeningSettingsKey[] =
    "voice_always_listening";

struct VoiceModeSettings {
  bool start_with_browser = false;
  bool always_listening = false;

  friend bool operator==(const VoiceModeSettings&,
                         const VoiceModeSettings&) = default;
};

// Blocking (file I/O). The defaults when settings.json is missing or isn't
// a JSON object. Best on MoltSettingsFileTaskRunner()
// (common/molt_settings_file.h), which sees every change posted before.
VoiceModeSettings ReadVoiceModeSettings();

// Blocking (file I/O), on MoltSettingsFileTaskRunner() only. Sets both keys
// and keeps every other one, atomically. False, and nothing written, if
// settings.json exists but isn't a JSON object (it is left for the user to
// fix), or if it can't be written.
bool WriteVoiceModeSettings(const VoiceModeSettings& settings);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_SESSION_SETTINGS_H_
