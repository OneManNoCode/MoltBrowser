// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_reading_settings.h"

#include <cmath>
#include <optional>

#include "base/values.h"
#include "chrome/browser/molt_ai/common/molt_settings_file.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"

namespace molt_ai {
namespace voice {

double ReadVoiceSpeechRate() {
  const std::optional<base::DictValue> values = ReadMoltSettings();
  if (!values) {
    return 1.0;
  }
  const std::optional<double> rate =
      values->FindDouble(kVoiceSpeechRateSettingsKey);
  if (!rate || !std::isfinite(*rate) || *rate < kMinSpeechRate ||
      *rate > kMaxSpeechRate) {
    return 1.0;
  }
  return *rate;
}

bool WriteVoiceSpeechRate(double rate) {
  if (!std::isfinite(rate) || rate < kMinSpeechRate || rate > kMaxSpeechRate) {
    return false;
  }
  base::DictValue changes;
  changes.Set(kVoiceSpeechRateSettingsKey, rate);
  return SetMoltSettings(changes);
}

}  // namespace voice
}  // namespace molt_ai
