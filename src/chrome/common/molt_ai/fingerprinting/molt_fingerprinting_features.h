// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Kill switch for MoltBrowser's engine-level fingerprinting protection
// (canvas / WebGL / audio farbling and hardwareConcurrency in normal and
// Incognito windows; that plus UTC, en-US and letterboxed screen metrics in
// MoltNet windows):
//
//   chrome://flags/#molt-fingerprinting-protection
//   --disable-features=MoltFingerprintingProtection
//
// Enabled by default. It is checked in the browser when a renderer process is
// launched: when disabled, renderers get no fingerprinting switches and Blink
// runs unmodified. Takes effect for renderers started after the change (in
// practice: after a relaunch). It is deliberately independent of
// --disable-molt-shield, which is a breakage escape hatch for ad blocking and
// must not silently drop MoltNet's uniformity.

#ifndef CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_FEATURES_H_
#define CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_FEATURES_H_

#include "base/feature_list.h"

namespace molt_ai::fingerprinting {

BASE_DECLARE_FEATURE(kMoltFingerprintingProtection);

namespace flags {
// chrome://flags internal name (entry in molt_fingerprinting_about_flags.h).
inline constexpr char kProtection[] = "molt-fingerprinting-protection";
}  // namespace flags

bool IsFingerprintingProtectionEnabled();

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_COMMON_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_FEATURES_H_
