// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Entry point for MoltBrowser's engine-level fingerprinting protection. The
// embedder (chrome/renderer/molt_ai/fingerprinting/) calls, on the main
// thread:
//   * Configure() once at renderer start (RenderThreadStarted) with the
//     profile's mode, which the browser put on the command line;
//   * SetSessionSeed() with the profile's session seed, which the browser
//     sends over a channel-associated mojo message right after launch (the
//     seed is a secret and stays off the command line);
//   * SetUnfarbledSites() at start and whenever the user turns MoltShield
//     off or on for a site.
// A renderer process only ever hosts one BrowserContext, so a per-process
// mode and seed are exact.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_

#include <cstdint>
#include <string>
#include <vector>

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
  // any script runs. Only the first call has an effect.
  static void Configure(Mode mode);
  // The profile's 32-byte session seed. Only the first call with exactly 32
  // bytes has an effect, and none while the mode is kOff. Until it arrives,
  // a context that needs a key gets a random one for its lifetime.
  static void SetSessionSeed(base::span<const uint8_t> session_seed);
  // Top-level sites ("https://example.com") with MoltShield turned off:
  // kFarble mode doesn't farble their documents and workers (kUniform
  // ignores the list). Replaces the previous list.
  static void SetUnfarbledSites(const std::vector<std::string>& sites);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
