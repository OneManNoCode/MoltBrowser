// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Renderer half of "no WebRTC in MoltNet windows"
// (chrome/browser/molt_ai/tor/molt_net_webrtc.h): reads the
// --molt-net-renderer switch the browser put on this process's command line
// and tells Blink, which then refuses RTCPeerConnection. Independent of
// fingerprinting protection and its kill switch.

#ifndef CHROME_RENDERER_MOLT_AI_TOR_MOLT_NET_RENDERER_H_
#define CHROME_RENDERER_MOLT_AI_TOR_MOLT_NET_RENDERER_H_

namespace molt_ai::tor {

// Call once from ChromeContentRendererClient::RenderThreadStarted (main
// thread, after Blink is initialized, before any frame or worker exists).
// No-op without the switch.
void ConfigureMoltNetRendererProcess();

}  // namespace molt_ai::tor

#endif  // CHROME_RENDERER_MOLT_AI_TOR_MOLT_NET_RENDERER_H_
