// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Blink's per-process MoltNet state, set by the embedder through
// blink::WebMoltNet (public/web/web_molt_net.h) from a fact the browser
// sent: the --molt-net-renderer switch on this process's command line.
// Never from page input.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_

#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

// True once WebMoltNet::DisableWebRtcPeerConnections() ran: this process
// hosts a MoltNet window, and RTCPeerConnection::Create() refuses
// (modules/peerconnection/rtc_peer_connection.cc).
CORE_EXPORT bool MoltNetWebRtcPeerConnectionsDisabled();

// Whether a MediaStream request's audio or video part of `type` captures a
// screen, a window or a tab rather than a camera or a microphone:
// getDisplayMedia() (preferCurrentTab included), getAllScreensMedia() and
// getUserMedia()'s chromeMediaSource forms. Every type is listed, so a new
// one has to be sorted before this compiles.
CORE_EXPORT bool IsDisplayCaptureType(mojom::MediaStreamType type);

// True in a process that hosts MoltNet (the fact behind
// MoltNetWebRtcPeerConnectionsDisabled()): with no WebRTC there, a screen,
// window or tab capture has nowhere to go, so none is offered.
// navigator.permissions reports display-capture as "denied"
// (molt_fingerprinting::ReportsPermissionDenied), and UserMediaProcessor
// answers a request for one (MoltNetRefusesDisplayCapture) without asking
// the browser, which refuses it as well (chrome/browser/molt_ai/
// fingerprinting/molt_net_display_capture.h).
CORE_EXPORT bool MoltNetDisplayCaptureDenied();

// Whether a request with these audio and video types (NO_SERVICE for a part
// it doesn't ask for) is refused: MoltNetDisplayCaptureDenied() and either
// part IsDisplayCaptureType(). The answer is the browser's for a user's
// Cancel in the picker, PERMISSION_DENIED_BY_USER: getDisplayMedia()
// rejects with NotAllowedError "Permission denied by user", with no picker.
CORE_EXPORT bool MoltNetRefusesDisplayCapture(mojom::MediaStreamType audio,
                                              mojom::MediaStreamType video);

// Tests only: back to the default (peer connections allowed).
CORE_EXPORT void ResetMoltNetRendererForTesting();

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_NET_MOLT_NET_RENDERER_H_
