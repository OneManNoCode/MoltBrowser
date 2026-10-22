// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_locale.h"

#include "third_party/blink/renderer/core/molt_fingerprinting/farbling.h"
#include "third_party/blink/renderer/platform/language.h"

namespace blink::molt_fingerprinting {

void PinUniformTextLocale() {
  // A string literal: the hook hands every thread the same pointer and
  // builds the AtomicString on the reading thread.
  SetPlatformLanguageOverride(kUniformLocale);
}

void UnpinTextLocaleForTesting() {
  SetPlatformLanguageOverride(nullptr);
}

}  // namespace blink::molt_fingerprinting
