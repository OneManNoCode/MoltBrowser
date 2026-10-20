// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"

namespace molt_ai::tor {

namespace {

constexpr WebRtcNetworkAccess kNone = {.peer_connections = false,
                                       .p2p_socket_manager = false,
                                       .mdns_responder = false};
constexpr WebRtcNetworkAccess kChromium = {};

}  // namespace

WebRtcNetworkAccess WebRtcNetworkAccessForProfile(bool is_moltnet_profile) {
  return is_moltnet_profile ? kNone : kChromium;
}

WebRtcNetworkAccess WebRtcNetworkAccessForRenderer(
    bool has_moltnet_renderer_switch) {
  return WebRtcNetworkAccessForProfile(has_moltnet_renderer_switch);
}

bool ShouldMarkMoltNetRenderer(bool is_moltnet_profile) {
  return !WebRtcNetworkAccessForProfile(is_moltnet_profile).peer_connections;
}

}  // namespace molt_ai::tor
