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
