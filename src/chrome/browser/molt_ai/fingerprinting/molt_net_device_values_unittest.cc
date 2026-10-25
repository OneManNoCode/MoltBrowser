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

// A mouse-only desktop. The pointer and hover values are bit sets in
// WebPreferences (ui::PointerType / ui::HoverType) and single values for the
// primary device (blink::mojom::PointerType / HoverType); both use 1 for
// "none", so "fine" and "hover" must be single, non-"none" bits.
TEST(MoltNetBrowserDeviceValuesTest, InputDevicesAreAMouseOnlyDesktop) {
  EXPECT_FALSE(kMoltNetTouchEventFeatureDetection);
  EXPECT_EQ(kMoltNetMaxTouchPoints, 0);
  EXPECT_EQ(kMoltNetAvailablePointerTypes, 4);  // fine only
  EXPECT_EQ(kMoltNetPrimaryPointerType, 4);     // fine
  EXPECT_EQ(kMoltNetAvailableHoverTypes, 2);    // hover only
  EXPECT_EQ(kMoltNetPrimaryHoverType, 2);       // hover
  EXPECT_EQ(kMoltNetAvailablePointerTypes & 1, 0);
  EXPECT_EQ(kMoltNetAvailablePointerTypes & 2, 0);  // no coarse pointer
  EXPECT_EQ(kMoltNetAvailableHoverTypes & 1, 0);
}

TEST(MoltNetBrowserDeviceValuesTest, CpuPerformanceTierIsUnknown) {
  // blink::mojom::PerformanceTier: kUnknown 0, kLow 1 ... kUltra 4; the
  // content side CHECKs that an override is in that range.
  EXPECT_EQ(kMoltNetCpuPerformanceTier, 0);
}

// MoltNet's renderer process limit is never above Chromium's on the machines
// MoltBrowser supports (4 GB, of which firmware and graphics keep up to half a
// GB, and more), so MoltNet never shares processes later than Chromium would.
TEST(MoltNetDeviceValuesTest, ProcessLimitIsTheSmallestMachines) {
  EXPECT_EQ(kMoltNetRendererProcessLimit, 20u);
  for (size_t mib : {3584u, 3840u, 4096u, 8192u, 16384u, 65536u}) {
    EXPECT_LE(kMoltNetRendererProcessLimit, ChromiumRendererProcessLimit(mib))
        << mib;
  }
  // Content's own numbers (render_process_host_impl.cc's table).
  EXPECT_EQ(ChromiumRendererProcessLimit(4096), 24u);
  EXPECT_EQ(ChromiumRendererProcessLimit(16384), 96u);
  EXPECT_EQ(ChromiumRendererProcessLimit(512), 3u);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
