// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_network.h"

#include <optional>

#include "base/time/time.h"
#include "third_party/blink/public/platform/web_connection_type.h"
#include "third_party/blink/public/platform/web_effective_connection_type.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_network_values.h"

namespace blink::molt_fingerprinting {

namespace {

// The values name the enums' values.
static_assert(static_cast<int>(WebEffectiveConnectionType::kType4G) ==
              moltnet::kEffectiveConnectionType4G);
static_assert(static_cast<int>(kWebConnectionTypeUnknown) ==
              moltnet::kConnectionTypeUnknown);

}  // namespace

NetworkStateNotifier::NetworkState UniformNetworkState() {
  NetworkStateNotifier::NetworkState state;
  state.connection_initialized = true;
  state.type =
      static_cast<WebConnectionType>(moltnet::kUniformConnectionType);
  state.max_bandwidth_mbps = moltnet::kUniformMaxBandwidthMbps;
  state.effective_type = static_cast<WebEffectiveConnectionType>(
      moltnet::kUniformEffectiveConnectionType);
  state.http_rtt = base::Milliseconds(moltnet::kUniformHttpRttMs);
  state.transport_rtt = base::Milliseconds(moltnet::kUniformTransportRttMs);
  state.downlink_throughput_mbps =
      static_cast<double>(moltnet::kUniformDownlinkKbps) / 1000;
  state.save_data = moltnet::kUniformSaveData;
  state.network_quality_web_holdback = std::nullopt;
  return state;
}

void PinUniformNetworkState() {
  GetNetworkStateNotifier().SetUniformNetworkState(UniformNetworkState());
}

}  // namespace blink::molt_fingerprinting
