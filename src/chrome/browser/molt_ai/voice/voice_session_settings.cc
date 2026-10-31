// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_session_settings.h"

#include <optional>

#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"

namespace molt_ai {
namespace voice {

VoiceModeSettings ReadVoiceModeSettings() {
  VoiceModeSettings settings;
  const std::optional<base::DictValue> values = ReadMoltSettings();
  if (!values) {
    return settings;
  }
  settings.start_with_browser =
      values->FindBool(kVoiceModeSettingsKey).value_or(false);
  settings.always_listening =
      values->FindBool(kVoiceAlwaysListeningSettingsKey).value_or(false);
  settings.say_word_aloud =
      values->FindBool(kVoiceSayWordAloudSettingsKey).value_or(false);
  return settings;
}

bool WriteVoiceModeSettings(const VoiceModeSettings& settings) {
  base::DictValue changes;
  changes.Set(kVoiceModeSettingsKey, settings.start_with_browser);
  changes.Set(kVoiceAlwaysListeningSettingsKey, settings.always_listening);
  changes.Set(kVoiceSayWordAloudSettingsKey, settings.say_word_aloud);
  return SetMoltSettings(changes);
}

}  // namespace voice
}  // namespace molt_ai
