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
  const base::Value* value = settings->Find(kWebAIApisSettingsKey);
  if (!value) {
    return true;
  }
  // A hand edit that isn't true or false ("false", 0) leaves them off too.
  return value->is_bool() && value->GetBool();
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
