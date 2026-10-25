// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// How the browser tells a renderer process which fingerprinting protection to
// apply. ChromeContentBrowserClient::AppendExtraCommandLineSwitches adds, for
// every renderer of a web profile:
//
//   --molt-fingerprinting-mode=farble|uniform
//
// and ChromeContentRendererClient::RenderThreadStarted hands it to Blink
// (blink::WebMoltFingerprinting). The mode is not a secret (it says whether
// a renderer belongs to a MoltNet window, as its sandbox and proxy settings
// do anyway). The per-profile session seed *is* a secret, so it never goes
// on the command line, where other local processes could read it: the
// browser sends it over the channel-associated FingerprintingConfiguration
// interface (molt_fingerprinting.mojom) at RenderProcessWillLaunch.
//
// A renderer process only ever hosts one BrowserContext (RenderProcessHostImpl
// ::IsSuitableHost and the spare-process manager both refuse a host from
// another context), so the per-process mode and seed are exact.

#ifndef CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_
#define CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace molt_ai::fingerprinting {

namespace switches {
inline constexpr char kMode[] = "molt-fingerprinting-mode";
// On MoltNet (uniform) renderers whose browser has MoltNet's bundled fonts:
// the renderer takes every font from them (blink::WebMoltFingerprinting::
// ExpectBundledFonts) and gets the files, read-only, over the channel
// (FingerprintingConfiguration.SetMoltNetFonts). No value.
inline constexpr char kMoltNetFonts[] = "molt-net-fonts";
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

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_SWITCHES_H_
