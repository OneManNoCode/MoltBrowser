// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_pixels.h"

#include "media/base/molt_portable_media.h"

namespace blink::molt_fingerprinting {

void PinUniformMediaPixels() {
  media::SetPortableMediaProcess();
}

}  // namespace blink::molt_fingerprinting
