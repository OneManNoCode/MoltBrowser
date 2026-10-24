// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the network-quality estimate and the connection type the
// browser tells the renderers of a BrowserContext whose embedder asks for
// uniform values (ContentBrowserClient::ShouldSendUniformNetworkState(); in
// MoltBrowser, MoltNet's profiles).
//
// The browser keeps one network-quality estimate for the whole browser
// (NetworkQualityObserverImpl) and the operating system's connection type
// and maximum bandwidth (BrowserOnlineStateObserver), and sends both to
// every renderer process when it starts and on each change. They describe
// the user's own connection, not Tor's. MoltNet's renderers report fixed
// values to pages whatever they are sent (Blink's NetworkStateNotifier is
// pinned there: third_party/blink/renderer/core/molt_fingerprinting/
// moltnet_network.h), so for them these messages only carry what a
// compromised renderer could read. These hooks send them the same fixed
// values instead:
//
//   network quality: effective type 4g, HTTP RTT 200 ms, transport RTT
//     125 ms, downlink 1600 kbps (the values moltnet_network_values.h
//     reports to pages), once, when the renderer process starts; the
//     browser's later updates aren't sent to it at all, so it doesn't
//     learn when the estimate changed either;
//   connection: "unknown" with an unknown (infinite) maximum bandwidth,
//     and "none" while the operating system reports no connection, so
//     navigator.onLine and the online and offline events still follow it,
//     as they do in every MoltNet renderer.
//
// Every other BrowserContext gets the browser's values, unchanged.

#ifndef CONTENT_BROWSER_NETWORK_MOLT_UNIFORM_NETWORK_STATE_H_
#define CONTENT_BROWSER_NETWORK_MOLT_UNIFORM_NETWORK_STATE_H_

#include <cstdint>
#include <limits>

#include "base/time/time.h"
#include "content/common/renderer.mojom.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_client.h"
#include "net/base/network_change_notifier.h"
#include "net/nqe/effective_connection_type.h"

namespace content {

namespace molt_uniform_network_state {

// The values, as the integers of the net:: enums they stand for (checked
// against the enums below). The numbers are those of
// third_party/blink/renderer/core/molt_fingerprinting/moltnet_network_values.h.
inline constexpr int kEffectiveConnectionType4G = 5;  // ..._TYPE_4G
inline constexpr int64_t kHttpRttMs = 200;
inline constexpr int64_t kTransportRttMs = 125;
inline constexpr double kDownlinkKbps = 1600;
inline constexpr int kConnectionUnknown = 0;  // CONNECTION_UNKNOWN
inline constexpr int kConnectionNone = 6;     // CONNECTION_NONE
inline constexpr double kMaxBandwidthMbps =
    std::numeric_limits<double>::infinity();

// The connection type a uniform renderer is told while the operating
// system reports `os_connection_type`.
constexpr int ConnectionTypeFor(int os_connection_type) {
  return os_connection_type == kConnectionNone ? kConnectionNone
                                               : kConnectionUnknown;
}

static_assert(kEffectiveConnectionType4G ==
              net::EFFECTIVE_CONNECTION_TYPE_4G);
static_assert(kConnectionUnknown ==
              net::NetworkChangeNotifier::CONNECTION_UNKNOWN);
static_assert(kConnectionNone == net::NetworkChangeNotifier::CONNECTION_NONE);

// Whether `host`'s renderer gets the uniform values.
inline bool IsUniform(RenderProcessHost* host) {
  return host && GetContentClient() && GetContentClient()->browser() &&
         GetContentClient()->browser()->ShouldSendUniformNetworkState(
             host->GetBrowserContext());
}

}  // namespace molt_uniform_network_state

// Whether the network-quality message goes to a renderer when it starts
// (kInitial) or when the browser's estimate changes (kUpdate).
enum class MoltNetworkQualityMessage { kInitial, kUpdate };

// Sends `host`'s renderer the network-quality estimate: the given values,
// or, if its BrowserContext asks for uniform ones, those when it starts and
// nothing on an update. Header-only, so the two observers' hooks need no
// new build target.
inline void MoltNotifyNetworkQualityChanged(
    RenderProcessHost* host,
    MoltNetworkQualityMessage message,
    net::EffectiveConnectionType type,
    base::TimeDelta http_rtt,
    base::TimeDelta transport_rtt,
    double downstream_throughput_kbps) {
  namespace uniform = molt_uniform_network_state;
  if (uniform::IsUniform(host)) {
    if (message == MoltNetworkQualityMessage::kUpdate) {
      return;
    }
    type = static_cast<net::EffectiveConnectionType>(
        uniform::kEffectiveConnectionType4G);
    http_rtt = base::Milliseconds(uniform::kHttpRttMs);
    transport_rtt = base::Milliseconds(uniform::kTransportRttMs);
    downstream_throughput_kbps = uniform::kDownlinkKbps;
  }
  host->GetRendererInterface()->OnNetworkQualityChanged(
      type, http_rtt, transport_rtt, downstream_throughput_kbps);
}

// Sends `host`'s renderer the connection type and maximum bandwidth: the
// given ones, or the uniform ones if its BrowserContext asks for them.
inline void MoltNotifyNetworkConnectionChanged(
    RenderProcessHost* host,
    net::NetworkChangeNotifier::ConnectionType type,
    double max_bandwidth_mbps) {
  namespace uniform = molt_uniform_network_state;
  if (uniform::IsUniform(host)) {
    type = static_cast<net::NetworkChangeNotifier::ConnectionType>(
        uniform::ConnectionTypeFor(static_cast<int>(type)));
    max_bandwidth_mbps = uniform::kMaxBandwidthMbps;
  }
  host->GetRendererInterface()->OnNetworkConnectionChanged(type,
                                                           max_bandwidth_mbps);
}

}  // namespace content

#endif  // CONTENT_BROWSER_NETWORK_MOLT_UNIFORM_NETWORK_STATE_H_
