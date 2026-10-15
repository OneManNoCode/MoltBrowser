// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// chrome://flags entry for the fingerprinting-protection kill switch
// (chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h).
// Expanded inside kFeatureEntries in chrome/browser/about_flags.cc next to
// the MoltShield entries, so the upstream file only gains an include and one
// macro line. Defaults to Enabled; Disabled takes effect for renderers
// started afterwards (relaunch to apply everywhere).
//
// The macro uses about_flags.cc's kOsAll and FEATURE_VALUE_TYPE.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_ABOUT_FLAGS_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_ABOUT_FLAGS_H_

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_features.h"

// clang-format off
#define MOLT_FINGERPRINTING_FEATURE_ENTRIES                                   \
    {molt_ai::fingerprinting::flags::kProtection,                             \
     "MoltBrowser: fingerprinting protection",                                \
     "Kill switch. When disabled, pages read real canvas, WebGL and audio "   \
     "output and the real hardwareConcurrency in every window, and MoltNet "  \
     "windows also stop reporting UTC, en-US and letterboxed screen sizes. "  \
     "Only turn this off to check whether it breaks a site.",                 \
     kOsAll,                                                                  \
     FEATURE_VALUE_TYPE(molt_ai::fingerprinting::kMoltFingerprintingProtection)}
// clang-format on

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_FINGERPRINTING_ABOUT_FLAGS_H_
