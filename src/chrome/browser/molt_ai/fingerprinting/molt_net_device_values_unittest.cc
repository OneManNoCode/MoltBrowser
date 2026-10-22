// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_device_values.h"

#include <cstdint>

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.h"

namespace molt_ai::fingerprinting {
namespace {

namespace blink_moltnet = blink::molt_fingerprinting::moltnet;

// The low-entropy user-agent hints, Save-Data and the preference hints, at
// compile time (network::mojom::WebClientHintsType values).
static_assert(MoltNetSendsClientHint(8));    // Sec-CH-UA
static_assert(MoltNetSendsClientHint(10));   // Sec-CH-UA-Platform
static_assert(MoltNetSendsClientHint(12));   // Sec-CH-UA-Mobile
static_assert(MoltNetSendsClientHint(15));   // Sec-CH-Prefers-Color-Scheme
static_assert(MoltNetSendsClientHint(27));   // Save-Data
static_assert(MoltNetSendsClientHint(28));   // Sec-CH-Prefers-Reduced-Motion
static_assert(MoltNetSendsClientHint(30));   // ...-Reduced-Transparency
static_assert(!MoltNetSendsClientHint(20));  // Sec-CH-DPR
static_assert(!MoltNetSendsClientHint(19));  // Sec-CH-Device-Memory
static_assert(!MoltNetSendsClientHint(14));  // Sec-CH-UA-Platform-Version

// Navigation requests (the browser) and subresource requests (Blink) carry
// the same hints in MoltNet.
TEST(MoltNetBrowserDeviceValuesTest, ClientHintRuleMatchesBlinks) {
  for (int32_t hint = -8; hint < 256; ++hint) {
    EXPECT_EQ(MoltNetSendsClientHint(hint),
              blink_moltnet::SendsClientHint(hint))
        << hint;
  }
  for (int32_t hint :
       {blink_moltnet::kHintUA, blink_moltnet::kHintUAPlatform,
        blink_moltnet::kHintUAMobile, blink_moltnet::kHintPrefersColorScheme,
        blink_moltnet::kHintSaveData, blink_moltnet::kHintPrefersReducedMotion,
        blink_moltnet::kHintPrefersReducedTransparency}) {
    EXPECT_TRUE(MoltNetSendsClientHint(hint)) << hint;
  }
}

}  // namespace
}  // namespace molt_ai::fingerprinting
