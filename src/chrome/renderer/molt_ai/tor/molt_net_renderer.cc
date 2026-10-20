// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/renderer/molt_ai/tor/molt_net_renderer.h"

#include "base/command_line.h"
#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"
#include "third_party/blink/public/web/web_molt_net.h"

namespace molt_ai::tor {

void ConfigureMoltNetRendererProcess() {
  const WebRtcNetworkAccess access = WebRtcNetworkAccessForRenderer(
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kMoltNetRenderer));
  if (!access.peer_connections) {
    blink::WebMoltNet::DisableWebRtcPeerConnections();
  }
}

}  // namespace molt_ai::tor
