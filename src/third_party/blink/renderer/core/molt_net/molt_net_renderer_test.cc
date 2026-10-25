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

TEST_F(MoltNetRendererTest, DisplayCaptureTypes) {
  using T = mojom::MediaStreamType;
  // Cameras, microphones and "none" aren't display capture ...
  for (T type :
       {T::NO_SERVICE, T::DEVICE_AUDIO_CAPTURE, T::DEVICE_VIDEO_CAPTURE}) {
    EXPECT_FALSE(IsDisplayCaptureType(type)) << static_cast<int>(type);
  }
  // ... every screen, window and tab type is.
  for (T type :
       {T::GUM_TAB_AUDIO_CAPTURE, T::GUM_TAB_VIDEO_CAPTURE,
        T::GUM_DESKTOP_VIDEO_CAPTURE, T::GUM_DESKTOP_AUDIO_CAPTURE,
        T::DISPLAY_VIDEO_CAPTURE, T::DISPLAY_AUDIO_CAPTURE,
        T::DISPLAY_VIDEO_CAPTURE_THIS_TAB, T::DISPLAY_VIDEO_CAPTURE_SET}) {
    EXPECT_TRUE(IsDisplayCaptureType(type)) << static_cast<int>(type);
  }
}

TEST_F(MoltNetRendererTest, DisplayCaptureAllowedByDefault) {
  using T = mojom::MediaStreamType;
  EXPECT_FALSE(MoltNetDisplayCaptureDenied());
  EXPECT_FALSE(MoltNetRefusesDisplayCapture(T::DISPLAY_AUDIO_CAPTURE,
                                            T::DISPLAY_VIDEO_CAPTURE));
}

TEST_F(MoltNetRendererTest, MoltNetRefusesDisplayCaptureOnly) {
  using T = mojom::MediaStreamType;
  WebMoltNet::DisableWebRtcPeerConnections();
  EXPECT_TRUE(MoltNetDisplayCaptureDenied());
  // getDisplayMedia(), with and without audio, and preferCurrentTab.
  EXPECT_TRUE(
      MoltNetRefusesDisplayCapture(T::NO_SERVICE, T::DISPLAY_VIDEO_CAPTURE));
  EXPECT_TRUE(MoltNetRefusesDisplayCapture(T::DISPLAY_AUDIO_CAPTURE,
                                           T::DISPLAY_VIDEO_CAPTURE));
  EXPECT_TRUE(MoltNetRefusesDisplayCapture(T::NO_SERVICE,
                                           T::DISPLAY_VIDEO_CAPTURE_THIS_TAB));
  // getUserMedia()'s chromeMediaSource forms, audio alone included.
  EXPECT_TRUE(MoltNetRefusesDisplayCapture(T::GUM_DESKTOP_AUDIO_CAPTURE,
                                           T::NO_SERVICE));
  EXPECT_TRUE(
      MoltNetRefusesDisplayCapture(T::NO_SERVICE, T::GUM_TAB_VIDEO_CAPTURE));
  // A camera and a microphone are still asked for as before.
  EXPECT_FALSE(MoltNetRefusesDisplayCapture(T::DEVICE_AUDIO_CAPTURE,
                                            T::DEVICE_VIDEO_CAPTURE));
  EXPECT_FALSE(
      MoltNetRefusesDisplayCapture(T::NO_SERVICE, T::DEVICE_VIDEO_CAPTURE));
}

}  // namespace
}  // namespace blink
