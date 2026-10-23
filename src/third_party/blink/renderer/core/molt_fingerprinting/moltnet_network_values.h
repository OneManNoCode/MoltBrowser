// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the connection and network quality
// MoltNet renderers report, in navigator.connection and to Blink's own
// heuristics (lazy-loading distances, the web-font and document.write
// interventions, media preload, Save-Data). Pure and std-only (tested
// standalone, like farbling.h); the Blink side is moltnet_network.h.
//
// Chromium keeps one network-quality estimate for the whole browser and
// sends it, with the operating system's connection type, to every renderer.
// It describes the user's own connection rather than Tor's, it differs
// between users, and pages that load heavily can move it, so two sites (or
// one site before and after New Identity) could signal to each other
// through it. MoltNet renderers report these fixed values instead, the same
// for every MoltNet user, and they never change.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_VALUES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_VALUES_H_

#include <cstdint>
#include <limits>

namespace blink::molt_fingerprinting::moltnet {

// blink::WebEffectiveConnectionType::kType4G and
// blink::kWebConnectionTypeUnknown at the pin (moltnet_network.cc checks
// them against the enums).
inline constexpr int kEffectiveConnectionType4G = 5;
inline constexpr int kConnectionTypeUnknown = 9;

// effectiveType "4g": the fastest of the four classes and what nearly every
// desktop reports. It is also the class in which Blink applies none of its
// slow-network measures (3g or slower: text shown in a fallback font at
// once while a web font loads; 2g: no document.written scripts; larger
// lazy-loading distances), and in which sites serve their normal pages, as
// they do in Firefox, Safari and Tor Browser, which have no
// navigator.connection.
inline constexpr int kUniformEffectiveConnectionType =
    kEffectiveConnectionType4G;

// rtt 200 and downlink 1.6: Chromium's own typical network quality for
// "4g" (net/nqe's kTypicalHttpRttEffectiveConnectionType, 175 ms, and
// kTypicalDownlinkKbpsEffectiveConnectionType, 1600 kbps), which Blink
// already reports for a type when it shows the type but not the
// measurement (the network-quality web holdback), on NetInfo's 50 ms and
// 50 kbps grid (175 ms rounds to 200). Chromium's estimator derives "4g"
// from any HTTP RTT under 272 ms, so the three values agree with each
// other. A circuit through Tor is usually slower than that, which Chromium
// would call "3g"; but "3g" turns on the slow-network measures above and
// makes sites serve their low-bandwidth pages, and Tor's throughput usually
// doesn't call for them. A moderate downlink makes adaptive players that
// start from it begin at a moderate quality and adapt from there.
inline constexpr int64_t kUniformHttpRttMs = 200;
inline constexpr int32_t kUniformDownlinkKbps = 1600;
// Not exposed to pages: Chromium's typical transport RTT for "4g".
inline constexpr int64_t kUniformTransportRttMs = 125;

// saveData false: data saver only exists on Android, and Chromium never
// turns it on for an off-the-record profile, which MoltNet's is
// (ChromeContentBrowserClient::IsDataSaverEnabled). So the user's setting
// never counted in MoltNet; false also keeps the Save-Data header of
// subresource requests and navigator.connection.saveData in agreement.
inline constexpr bool kUniformSaveData = false;

// type "unknown" and downlinkMax +Infinity (both exposed only on Android
// and ChromeOS): what Chromium reports when it hides the connection type
// (the NetInfoConstantType feature). Not a cellular type, so media
// elements keep their usual preload.
inline constexpr int kUniformConnectionType = kConnectionTypeUnknown;
inline constexpr double kUniformMaxBandwidthMbps =
    std::numeric_limits<double>::infinity();

}  // namespace blink::molt_fingerprinting::moltnet

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_NETWORK_VALUES_H_
