// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_values.h"

namespace blink::molt_fingerprinting::moltnet {

Orientation OrientationOf(int width, int height) {
  return height >= width ? Orientation::kPortraitPrimary
                         : Orientation::kLandscapePrimary;
}

float PointerPressure(bool buttons_down) {
  return buttons_down ? 0.5f : 0.0f;
}

bool UsesOverlayScrollbars(FontPlatform platform) {
  switch (platform) {
    case FontPlatform::kMac:
    case FontPlatform::kAndroid:
      return true;
    case FontPlatform::kWindows:
    case FontPlatform::kLinux:
    // Not an OS (the bundled font set's list); never asked about.
    case FontPlatform::kMoltNetBundled:
      return false;
  }
  return false;
}

}  // namespace blink::molt_fingerprinting::moltnet
