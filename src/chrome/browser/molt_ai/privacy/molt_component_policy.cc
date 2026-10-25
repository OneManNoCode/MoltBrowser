// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_component_policy.h"

namespace molt_ai {
namespace privacy {

BASE_FEATURE(kMoltGoogleOnlyComponents, base::FEATURE_DISABLED_BY_DEFAULT);

bool ShouldRegisterGoogleOnlyComponents() {
  return base::FeatureList::IsEnabled(kMoltGoogleOnlyComponents);
}

}  // namespace privacy
}  // namespace molt_ai
