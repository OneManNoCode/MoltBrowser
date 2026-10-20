// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"

#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

TEST(MoltNetWebRtcPolicyTest, MoltNetGetsNoWebRtcNetworking) {
  const WebRtcNetworkAccess access =
      WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/true);
  // No RTCPeerConnection, and the browser keeps the network service's P2P
  // sockets (and with them its host-name lookups) and mDNS responder away
  // from the profile's frames, whatever the renderer does.
  EXPECT_FALSE(access.peer_connections);
  EXPECT_FALSE(access.p2p_socket_manager);
  EXPECT_FALSE(access.mdns_responder);
}

TEST(MoltNetWebRtcPolicyTest, OtherProfilesKeepChromiumBehaviour) {
  const WebRtcNetworkAccess access =
      WebRtcNetworkAccessForProfile(/*is_moltnet_profile=*/false);
  EXPECT_TRUE(access.peer_connections);
  EXPECT_TRUE(access.p2p_socket_manager);
  EXPECT_TRUE(access.mdns_responder);
  EXPECT_EQ(access, WebRtcNetworkAccess());
}

TEST(MoltNetWebRtcPolicyTest, OnlyMoltNetRenderersAreMarked) {
  EXPECT_TRUE(ShouldMarkMoltNetRenderer(/*is_moltnet_profile=*/true));
  EXPECT_FALSE(ShouldMarkMoltNetRenderer(/*is_moltnet_profile=*/false));
}

TEST(MoltNetWebRtcPolicyTest, RendererAndBrowserAgree) {
  // The renderer refuses RTCPeerConnection exactly when the browser refuses
  // the profile's P2P sockets, so a page fails at the constructor instead
  // of waiting for candidates that never come.
  for (bool is_moltnet : {false, true}) {
    SCOPED_TRACE(is_moltnet);
    const WebRtcNetworkAccess browser =
        WebRtcNetworkAccessForProfile(is_moltnet);
    const WebRtcNetworkAccess renderer =
        WebRtcNetworkAccessForRenderer(ShouldMarkMoltNetRenderer(is_moltnet));
    EXPECT_EQ(browser, renderer);
    EXPECT_EQ(renderer.peer_connections, browser.p2p_socket_manager);
  }
}

TEST(MoltNetWebRtcPolicyTest, UnmarkedRendererKeepsWebRtc) {
  // A renderer without the switch (every non-MoltNet profile, and a
  // MoltNet renderer in --single-process mode, see molt_net_webrtc.h) keeps
  // Chromium's behaviour; the browser side alone still holds there.
  EXPECT_EQ(WebRtcNetworkAccessForRenderer(false), WebRtcNetworkAccess());
}

TEST(MoltNetWebRtcPolicyTest, SwitchNameIsWellFormed) {
  constexpr std::string_view name = switches::kMoltNetRenderer;
  EXPECT_FALSE(name.empty());
  for (char c : name) {
    EXPECT_TRUE((c >= 'a' && c <= 'z') || c == '-') << c;
  }
  // Distinct from the fingerprinting mode switch, which is only present
  // while fingerprinting protection is on.
  EXPECT_NE(name, std::string_view("molt-fingerprinting-mode"));
}

}  // namespace
}  // namespace molt_ai::tor
