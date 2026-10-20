// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What Blink refuses in a renderer process that hosts a MoltNet (Tor)
// profile. The embedder (chrome/renderer/molt_ai/tor/) calls it once at
// renderer start when the browser put --molt-net-renderer on this process's
// command line (chrome/common/molt_ai/tor/molt_net_webrtc_policy.h); a
// renderer process only ever hosts one BrowserContext. Independent of
// WebMoltFingerprinting and of its kill switch.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_NET_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_NET_H_

#include "third_party/blink/public/platform/web_common.h"

namespace blink {

class BLINK_EXPORT WebMoltNet {
 public:
  // From now on `new RTCPeerConnection()` (and webkitRTCPeerConnection)
  // throws NotSupportedError, with the message Chromium uses when WebRTC is
  // unavailable, before it looks at the configuration. The browser refuses
  // the process's P2P sockets anyway (chrome/browser/molt_ai/tor/
  // molt_net_webrtc.h); this makes pages fail at once instead of waiting
  // for ICE candidates. Call on the main thread before any script runs. It
  // can't be undone.
  static void DisableWebRtcPeerConnections();
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_NET_H_
