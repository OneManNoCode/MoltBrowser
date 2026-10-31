// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_language_settings.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/voice/voice_language.h"

namespace molt_ai {
namespace voice {

std::string ReadVoiceLanguageSetting() {
  const std::optional<base::DictValue> values = ReadMoltSettings();
  if (!values) {
    return std::string();
  }
  const std::string* stored = values->FindString(kVoiceLanguageSettingsKey);
  if (!stored || !StoredVoiceLanguage(*stored)) {
    return std::string();
  }
  return *stored;
}

bool WriteVoiceLanguageSetting(const std::string& code) {
  if (!code.empty() && !StoredVoiceLanguage(code)) {
    return false;
  }
  return UpdateMoltSettings([&](base::DictValue& settings) {
    if (code.empty()) {
      settings.Remove(kVoiceLanguageSettingsKey);
    } else {
      settings.Set(kVoiceLanguageSettingsKey, code);
    }
    return true;
  });
}

}  // namespace voice
}  // namespace molt_ai
