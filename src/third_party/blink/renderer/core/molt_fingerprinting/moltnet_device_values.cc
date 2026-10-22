// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.h"

namespace blink::molt_fingerprinting::moltnet {

bool SendsClientHint(int32_t hint) {
  switch (hint) {
    case kHintUA:
    case kHintUAPlatform:
    case kHintUAMobile:
    case kHintPrefersColorScheme:
    case kHintSaveData:
    case kHintPrefersReducedMotion:
    case kHintPrefersReducedTransparency:
      return true;
    default:
      return false;
  }
}

}  // namespace blink::molt_fingerprinting::moltnet
