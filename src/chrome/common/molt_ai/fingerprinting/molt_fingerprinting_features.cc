// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"

namespace molt_ai::fingerprinting {

// Feature name: "MoltFingerprintingProtection".
BASE_FEATURE(kMoltFingerprintingProtection, base::FEATURE_ENABLED_BY_DEFAULT);

bool IsFingerprintingProtectionEnabled() {
  return base::FeatureList::IsEnabled(kMoltFingerprintingProtection);
}

}  // namespace molt_ai::fingerprinting
