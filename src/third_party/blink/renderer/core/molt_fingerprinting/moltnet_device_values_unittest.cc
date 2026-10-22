// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_device_values.h"

#include <cstdint>
#include <limits>
#include <set>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

// network::mojom::WebClientHintsType at the pin, by name.
namespace hints {
enum : int32_t {
  kDeviceMemory_DEPRECATED = 0,
  kDpr_DEPRECATED = 1,
  kResourceWidth_DEPRECATED = 2,
  kViewportWidth_DEPRECATED = 3,
  kRtt_DEPRECATED = 4,
  kDownlink_DEPRECATED = 5,
  kEct_DEPRECATED = 6,
  kUA = 8,
  kUAArch = 9,
  kUAPlatform = 10,
  kUAModel = 11,
  kUAMobile = 12,
  kUAFullVersion = 13,
  kUAPlatformVersion = 14,
  kPrefersColorScheme = 15,
  kUABitness = 16,
  kViewportHeight = 18,
  kDeviceMemory = 19,
  kDpr = 20,
  kResourceWidth = 21,
  kViewportWidth = 22,
  kUAFullVersionList = 23,
  kUAWoW64 = 25,
  kSaveData = 27,
  kPrefersReducedMotion = 28,
  kUAFormFactors = 29,
  kPrefersReducedTransparency = 30,
};
}  // namespace hints

TEST(MoltNetDeviceValuesTest, AllowListNamesTheEnumValues) {
  EXPECT_EQ(kHintUA, hints::kUA);
  EXPECT_EQ(kHintUAPlatform, hints::kUAPlatform);
  EXPECT_EQ(kHintUAMobile, hints::kUAMobile);
  EXPECT_EQ(kHintPrefersColorScheme, hints::kPrefersColorScheme);
  EXPECT_EQ(kHintSaveData, hints::kSaveData);
  EXPECT_EQ(kHintPrefersReducedMotion, hints::kPrefersReducedMotion);
  EXPECT_EQ(kHintPrefersReducedTransparency,
            hints::kPrefersReducedTransparency);
}

// The low-entropy defaults sites rely on, and the preference hints.
TEST(MoltNetDeviceValuesTest, SendsTheLowEntropyAndPreferenceHints) {
  for (int32_t type :
       {hints::kUA, hints::kUAMobile, hints::kUAPlatform, hints::kSaveData,
        hints::kPrefersColorScheme, hints::kPrefersReducedMotion,
        hints::kPrefersReducedTransparency}) {
    EXPECT_TRUE(SendsClientHint(type)) << type;
  }
}

TEST(MoltNetDeviceValuesTest, WithholdsDeviceNetworkAndHighEntropyHints) {
  // The device: pixel ratio, viewport, resource width and memory.
  for (int32_t type :
       {hints::kDpr_DEPRECATED, hints::kDpr, hints::kViewportWidth_DEPRECATED,
        hints::kViewportWidth, hints::kViewportHeight,
        hints::kResourceWidth_DEPRECATED, hints::kResourceWidth,
        hints::kDeviceMemory_DEPRECATED, hints::kDeviceMemory}) {
    EXPECT_FALSE(SendsClientHint(type)) << type;
  }
  // The network-quality estimate.
  for (int32_t type : {hints::kRtt_DEPRECATED, hints::kDownlink_DEPRECATED,
                       hints::kEct_DEPRECATED}) {
    EXPECT_FALSE(SendsClientHint(type)) << type;
  }
  // The high-entropy user agent.
  for (int32_t type :
       {hints::kUAArch, hints::kUAModel, hints::kUAFullVersion,
        hints::kUAPlatformVersion, hints::kUABitness, hints::kUAFullVersionList,
        hints::kUAWoW64, hints::kUAFormFactors}) {
    EXPECT_FALSE(SendsClientHint(type)) << type;
  }
}

// An allow-list: removed values, values a later Chromium may add and
// nonsense are all withheld.
TEST(MoltNetDeviceValuesTest, EverythingElseIsWithheld) {
  const std::set<int32_t> allowed = {hints::kUA,
                                     hints::kUAMobile,
                                     hints::kUAPlatform,
                                     hints::kSaveData,
                                     hints::kPrefersColorScheme,
                                     hints::kPrefersReducedMotion,
                                     hints::kPrefersReducedTransparency};
  for (int32_t type = -8; type < 256; ++type) {
    EXPECT_EQ(SendsClientHint(type), allowed.count(type) == 1) << type;
  }
  EXPECT_FALSE(SendsClientHint(std::numeric_limits<int32_t>::min()));
  EXPECT_FALSE(SendsClientHint(std::numeric_limits<int32_t>::max()));
}

TEST(MoltNetDeviceValuesTest, DeviceMemoryIsAValidDesktopValue) {
  // The Device Memory spec's values are powers of two; Chromium 152 reports
  // 2 to 32 on desktop.
  EXPECT_EQ(kDeviceMemory, 8.0f);
  EXPECT_GE(kDeviceMemory, 2.0f);
  EXPECT_LE(kDeviceMemory, 32.0f);
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
