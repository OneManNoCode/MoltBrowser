// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How the browser tells a renderer process which fingerprinting protection to
// apply. ChromeContentBrowserClient::AppendExtraCommandLineSwitches adds, for
// every renderer of a web profile:
//
//   --molt-fingerprinting-mode=farble|uniform
//   --molt-fingerprinting-seed=<64 hex digits>
//
// and ChromeContentRendererClient::RenderThreadStarted hands them to Blink
// (blink::WebMoltFingerprinting). A renderer process only ever hosts one
// BrowserContext (RenderProcessHostImpl::IsSuitableHost and the spare-process
// manager both refuse a host from another context), so the per-process mode
// and seed are exact.
//
// The seed is a per-profile, per-session secret: it never touches disk, but
// it is visible in the renderer's argv to other processes of the same user
// (and, on Linux/macOS, to `ps` for other local users). That is outside the
// web-attacker threat model this protects against; see
// docs/FINGERPRINTING.md.

#ifndef CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_
#define CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"

namespace molt_ai::fingerprinting {

namespace switches {
inline constexpr char kMode[] = "molt-fingerprinting-mode";
inline constexpr char kSeed[] = "molt-fingerprinting-seed";
}  // namespace switches

enum class RendererMode {
  kOff,
  // Normal and Incognito profiles: per-site, per-session farbling.
  kFarble,
  // The MoltNet (Tor) profile: farbling plus uniform values.
  kUniform,
};

inline constexpr size_t kSessionSeedSize = 32;
using SessionSeed = std::array<uint8_t, kSessionSeedSize>;

// "farble" / "uniform"; empty for kOff.
std::string_view ModeToSwitchValue(RendererMode mode);
// Anything but "farble" / "uniform" (including empty) is kOff.
RendererMode ModeFromSwitchValue(std::string_view value);

// 64 hex digits.
std::string EncodeSeed(base::span<const uint8_t, kSessionSeedSize> seed);
// Exactly 64 hex digits (either case), else nullopt.
std::optional<SessionSeed> DecodeSeed(std::string_view hex);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_
