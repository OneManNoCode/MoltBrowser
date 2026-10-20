// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Blink's per-process MoltNet state, set by the embedder through
// blink::WebMoltNet (public/web/web_molt_net.h) from a fact the browser
// sent: the --molt-net-renderer switch on this process's command line.
// Never from page input.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_

#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

// True once WebMoltNet::DisableWebRtcPeerConnections() ran: this process
// hosts a MoltNet window, and RTCPeerConnection::Create() refuses
// (modules/peerconnection/rtc_peer_connection.cc).
CORE_EXPORT bool MoltNetWebRtcPeerConnectionsDisabled();

// Tests only: back to the default (peer connections allowed).
CORE_EXPORT void ResetMoltNetRendererForTesting();

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_
