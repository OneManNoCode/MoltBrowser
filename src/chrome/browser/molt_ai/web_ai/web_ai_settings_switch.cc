// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_settings_switch.h"

#include <optional>

#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"

namespace molt_ai::web_ai {

bool WebAIApisSwitchFrom(const std::optional<base::DictValue>& settings) {
  if (!settings) {
    return false;
  }
  return settings->FindBool(kWebAIApisSettingsKey).value_or(true);
}

bool ReadWebAIApisSwitch() {
  return WebAIApisSwitchFrom(ReadMoltSettings());
}

bool WriteWebAIApisSwitch(bool enabled) {
  base::DictValue changes;
  changes.Set(kWebAIApisSettingsKey, enabled);
  return SetMoltSettings(changes);
}

}  // namespace molt_ai::web_ai
