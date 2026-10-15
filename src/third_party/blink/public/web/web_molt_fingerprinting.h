// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Entry point for MoltBrowser's engine-level fingerprinting protection. The
// embedder (ChromeContentRendererClient::RenderThreadStarted, through
// chrome/renderer/molt_ai/fingerprinting/) calls Configure() once per renderer
// process with the profile's mode and session seed, which the browser put on
// the renderer's command line. A renderer process only ever hosts one
// BrowserContext, so a per-process mode is exact.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_

#include <cstdint>

#include "base/containers/span.h"
#include "third_party/blink/public/platform/web_common.h"

namespace blink {

class BLINK_EXPORT WebMoltFingerprinting {
 public:
  enum class Mode {
    // No protection (feature disabled, or not a web-content profile).
    kOff,
    // Normal and Incognito windows: per-site, per-session farbling.
    kFarble,
    // MoltNet (Tor) windows: farbling plus uniform values (UTC, en-US,
    // letterboxed screen metrics, fixed hardwareConcurrency, no unmasked
    // WebGL renderer strings).
    kUniform,
  };

  // Must be called on the main thread after blink::Initialize() and before
  // any script runs. `session_seed` must be exactly 32 bytes; anything else
  // turns protection off. Only the first call has an effect.
  static void Configure(Mode mode, base::span<const uint8_t> session_seed);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
