// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_net/molt_net_renderer.h"

#include <atomic>

#include "third_party/blink/public/web/web_molt_net.h"

namespace blink {

namespace {

// Written once on the main thread before any script runs; atomic so a read
// from another thread is well-defined too.
std::atomic<bool> g_molt_net_webrtc_peer_connections_disabled{false};

}  // namespace

bool MoltNetWebRtcPeerConnectionsDisabled() {
  return g_molt_net_webrtc_peer_connections_disabled.load(
      std::memory_order_relaxed);
}

bool IsDisplayCaptureType(mojom::MediaStreamType type) {
  switch (type) {
    case mojom::MediaStreamType::NO_SERVICE:
    case mojom::MediaStreamType::DEVICE_AUDIO_CAPTURE:
    case mojom::MediaStreamType::DEVICE_VIDEO_CAPTURE:
    case mojom::MediaStreamType::NUM_MEDIA_TYPES:
      return false;
    case mojom::MediaStreamType::GUM_TAB_AUDIO_CAPTURE:
    case mojom::MediaStreamType::GUM_TAB_VIDEO_CAPTURE:
    case mojom::MediaStreamType::GUM_DESKTOP_VIDEO_CAPTURE:
    case mojom::MediaStreamType::GUM_DESKTOP_AUDIO_CAPTURE:
    case mojom::MediaStreamType::DISPLAY_VIDEO_CAPTURE:
    case mojom::MediaStreamType::DISPLAY_AUDIO_CAPTURE:
    case mojom::MediaStreamType::DISPLAY_VIDEO_CAPTURE_THIS_TAB:
    case mojom::MediaStreamType::DISPLAY_VIDEO_CAPTURE_SET:
      return true;
  }
  // Not a known type: count it as a capture.
  return true;
}

bool MoltNetDisplayCaptureDenied() {
  return MoltNetWebRtcPeerConnectionsDisabled();
}

bool MoltNetRefusesDisplayCapture(mojom::MediaStreamType audio,
                                  mojom::MediaStreamType video) {
  return MoltNetDisplayCaptureDenied() &&
         (IsDisplayCaptureType(audio) || IsDisplayCaptureType(video));
}

void ResetMoltNetRendererForTesting() {
  g_molt_net_webrtc_peer_connections_disabled.store(false,
                                                     std::memory_order_relaxed);
}

// static
void WebMoltNet::DisableWebRtcPeerConnections() {
  g_molt_net_webrtc_peer_connections_disabled.store(true,
                                                     std::memory_order_relaxed);
}

}  // namespace blink
