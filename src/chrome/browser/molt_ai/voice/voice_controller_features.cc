// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_controller_features.h"

namespace molt_ai::voice {

BASE_FEATURE(kMoltVoiceMode, base::FEATURE_DISABLED_BY_DEFAULT);

bool VoiceModeEnabled() {
  return base::FeatureList::IsEnabled(kMoltVoiceMode);
}

}  // namespace molt_ai::voice
