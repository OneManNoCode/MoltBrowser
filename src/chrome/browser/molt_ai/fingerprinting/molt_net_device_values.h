// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's uniform device, browser side: the values the browser decides
// for MoltNet windows (molt_net_client_hints.h). Std-only and constexpr, so
// molt_ai_unittests checks them standalone and against the enums they
// mirror (molt_net_client_hints.cc static_asserts the same against the
// real ones).
//
// The client-hint rule is Blink's (moltnet_device_values.h in
// third_party/blink/renderer/core/molt_fingerprinting/), duplicated because
// browser code can't include renderer headers;
// molt_net_device_values_unittest.cc checks that the two agree.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_

#include <cstdint>

namespace molt_ai::fingerprinting {

// --- Client hints ------------------------------------------------------------

// Whether MoltNet windows send the client hint whose
// network::mojom::WebClientHintsType value is `hint`: only Sec-CH-UA,
// Sec-CH-UA-Mobile, Sec-CH-UA-Platform, Save-Data and the
// Sec-CH-Prefers-{Color-Scheme,Reduced-Motion,Reduced-Transparency} hints.
// Everything else (DPR, viewport, width, memory, network quality, the
// high-entropy user agent, and any later hint) is withheld.
constexpr bool MoltNetSendsClientHint(int32_t hint) {
  switch (hint) {
    case 8:   // kUA
    case 10:  // kUAPlatform
    case 12:  // kUAMobile
    case 15:  // kPrefersColorScheme
    case 27:  // kSaveData
    case 28:  // kPrefersReducedMotion
    case 30:  // kPrefersReducedTransparency
      return true;
    default:
      return false;
  }
}

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_DEVICE_VALUES_H_
