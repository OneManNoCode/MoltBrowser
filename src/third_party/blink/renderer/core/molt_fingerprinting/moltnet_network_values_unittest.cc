// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_network_values.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

// blink::WebEffectiveConnectionType and blink::WebConnectionType at the pin,
// by name.
enum : int {
  kTypeUnknown = 0,
  kTypeOffline = 1,
  kTypeSlow2G = 2,
  kType2G = 3,
  kType3G = 4,
  kType4G = 5,
};
enum : int {
  kWebConnectionTypeCellular2G = 0,
  kWebConnectionTypeCellular3G = 1,
  kWebConnectionTypeCellular4G = 2,
  kWebConnectionTypeNone = 8,
  kWebConnectionTypeUnknown = 9,
};

// net/nqe at the pin: the HTTP RTT from which the estimator calls a
// network slow-2g, 2g and 3g (kHttpRttEffectiveConnectionTypeThresholds;
// throughput no longer counts), and its typical network quality for 4g
// (kTypicalHttpRttEffectiveConnectionType,
// kTypicalDownlinkKbpsEffectiveConnectionType, and the typical transport
// RTT in ObtainTypicalNetworkQualities()).
constexpr int64_t kSlow2GFromMs = 2010;
constexpr int64_t k2GFromMs = 1420;
constexpr int64_t k3GFromMs = 272;
constexpr int64_t kTypical4GHttpRttMs = 175;
constexpr int64_t kTypical4GTransportRttMs = 125;
constexpr int32_t kTypical4GDownlinkKbps = 1600;

int EffectiveTypeForHttpRtt(int64_t http_rtt_ms) {
  if (http_rtt_ms >= kSlow2GFromMs) {
    return kTypeSlow2G;
  }
  if (http_rtt_ms >= k2GFromMs) {
    return kType2G;
  }
  if (http_rtt_ms >= k3GFromMs) {
    return kType3G;
  }
  return kType4G;
}

// What NetworkInformation shows for an RTT and a downlink, as
// NetworkStateNotifier::RoundRtt() and RoundMbps() compute it without the
// per-site noise (none in MoltNet): at most 3 s in 50 ms steps, and at most
// 10 Mbps in 50 kbps steps, both rounded to the nearest step.
int64_t WebRttMs(int64_t rtt_ms) {
  const int64_t capped = rtt_ms < 3000 ? rtt_ms : 3000;
  return (capped + 25) / 50 * 50;
}
double WebDownlinkMbps(int32_t kbps) {
  const double capped = kbps < 10000 ? kbps : 10000;
  return std::round(capped / 50) * 50 / 1000;
}

TEST(MoltNetNetworkValuesTest, NamesTheEnumValues) {
  EXPECT_EQ(kEffectiveConnectionType4G, kType4G);
  EXPECT_EQ(kConnectionTypeUnknown, kWebConnectionTypeUnknown);
}

// "4g", and the RTT is one Chromium's own estimator calls 4g: the three
// values agree with each other.
TEST(MoltNetNetworkValuesTest, FourGWithAnRttChromiumCallsFourG) {
  EXPECT_EQ(kUniformEffectiveConnectionType, kType4G);
  EXPECT_EQ(EffectiveTypeForHttpRtt(kUniformHttpRttMs), kType4G);
  EXPECT_GT(kUniformHttpRttMs, 0);
  EXPECT_GT(kUniformDownlinkKbps, 0);
  EXPECT_GT(kUniformTransportRttMs, 0);
  EXPECT_LE(kUniformTransportRttMs, kUniformHttpRttMs);
}

// Chromium's typical network quality for 4g, as NetInfo rounds it.
TEST(MoltNetNetworkValuesTest, ChromiumsTypicalFourG) {
  EXPECT_EQ(kUniformHttpRttMs, WebRttMs(kTypical4GHttpRttMs));
  EXPECT_EQ(kUniformDownlinkKbps, kTypical4GDownlinkKbps);
  EXPECT_EQ(kUniformTransportRttMs, kTypical4GTransportRttMs);
}

// Already on NetInfo's grid and below its caps, so what pages read is the
// value itself: rtt 200, downlink 1.6.
TEST(MoltNetNetworkValuesTest, PagesReadTheValuesThemselves) {
  EXPECT_EQ(WebRttMs(kUniformHttpRttMs), kUniformHttpRttMs);
  EXPECT_EQ(WebRttMs(kUniformHttpRttMs), 200);
  EXPECT_EQ(kUniformDownlinkKbps % 50, 0);
  EXPECT_LT(kUniformDownlinkKbps, 10000);
  EXPECT_EQ(WebDownlinkMbps(kUniformDownlinkKbps), 1.6);
}

// No data saver, and a connection type that is neither cellular (media
// keep their usual preload) nor "none".
TEST(MoltNetNetworkValuesTest, NoDataSaverAndAnUnknownConnection) {
  EXPECT_FALSE(kUniformSaveData);
  EXPECT_EQ(kUniformConnectionType, kWebConnectionTypeUnknown);
  EXPECT_NE(kUniformConnectionType, kWebConnectionTypeCellular2G);
  EXPECT_NE(kUniformConnectionType, kWebConnectionTypeCellular3G);
  EXPECT_NE(kUniformConnectionType, kWebConnectionTypeCellular4G);
  EXPECT_NE(kUniformConnectionType, kWebConnectionTypeNone);
  EXPECT_TRUE(std::isinf(kUniformMaxBandwidthMbps));
  EXPECT_GT(kUniformMaxBandwidthMbps, 0);
}

TEST(MoltNetNetworkValuesTest, AreConstantExpressions) {
  static_assert(kUniformEffectiveConnectionType == kEffectiveConnectionType4G);
  static_assert(kUniformHttpRttMs == 200);
  static_assert(kUniformDownlinkKbps == 1600);
  static_assert(!kUniformSaveData);
  static_assert(kUniformMaxBandwidthMbps ==
                std::numeric_limits<double>::infinity());
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
