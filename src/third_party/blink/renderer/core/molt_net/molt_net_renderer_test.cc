// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_net/molt_net_renderer.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/web/web_molt_net.h"

namespace blink {
namespace {

class MoltNetRendererTest : public testing::Test {
 protected:
  ~MoltNetRendererTest() override { ResetMoltNetRendererForTesting(); }
};

TEST_F(MoltNetRendererTest, PeerConnectionsAllowedByDefault) {
  // A renderer the browser didn't mark (every non-MoltNet profile) keeps
  // WebRTC.
  EXPECT_FALSE(MoltNetWebRtcPeerConnectionsDisabled());
}

TEST_F(MoltNetRendererTest, EmbedderDisablesPeerConnections) {
  WebMoltNet::DisableWebRtcPeerConnections();
  EXPECT_TRUE(MoltNetWebRtcPeerConnectionsDisabled());
  // Calling it again changes nothing.
  WebMoltNet::DisableWebRtcPeerConnections();
  EXPECT_TRUE(MoltNetWebRtcPeerConnectionsDisabled());
}

}  // namespace
}  // namespace blink
