// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet windows have no WebRTC networking, as in Tor Browser. Why: see
// chrome/common/molt_ai/tor/molt_net_webrtc_policy.h (in short, the host
// names WebRTC is given are looked up outside Tor, and the network service's
// P2P socket manager trusts the renderer to stay behind the proxy).
//
// Decided by the profile alone (IsMoltNetProfile), not by fingerprinting
// protection or its kill switch, in two places:
//
//   * Browser (the boundary). RegisterMoltNetFrameBinders(), called for
//     every frame from ChromeContentBrowserClient::
//     RegisterBrowserInterfaceBindersForFrame after content's and Chrome's
//     own binders, replaces two binders of a MoltNet frame
//     (molt_net_webrtc_binders.h):
//       - network::mojom::P2PSocketManager. Content would bind it to the
//         network service's (content/browser/browser_interface_binders.cc
//         BindSocketManager -> P2PSocketDispatcherHost::BindReceiver ->
//         NetworkContext::CreateP2PSocketManager). A MoltNet frame gets a
//         browser-side one that answers every host-name lookup with "not
//         found", opens no socket and reports no network interface.
//       - network::mojom::MdnsResponder: the pipe is closed, so nothing is
//         announced on the local network.
//     So no MoltNet renderer, even one that ignores the switch below, can
//     make the browser resolve a name of its choosing, open a socket
//     outside the proxy or learn the machine's addresses through WebRTC.
//
//   * Renderer (clean failure). AppendMoltNetRendererSwitches(), called
//     from ChromeContentBrowserClient::AppendExtraCommandLineSwitches, puts
//     --molt-net-renderer on the command line of the profile's renderer
//     processes (a renderer process only ever hosts one BrowserContext).
//     chrome/renderer/molt_ai/tor/ hands that to Blink at start-up
//     (blink::WebMoltNet), and `new RTCPeerConnection()` throws
//     NotSupportedError with Chromium's own "WebRTC is disabled" message,
//     so a page fails at once instead of waiting for ICE candidates that
//     never come. (--single-process has no renderer command line; there the
//     browser side alone holds.)

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_H_

#include "mojo/public/cpp/bindings/binder_map.h"

namespace base {
class CommandLine;
}  // namespace base

namespace content {
class RenderFrameHost;
class RenderProcessHost;
}  // namespace content

namespace molt_ai::tor {

// For every frame, after content's and Chrome's own binders. No-op unless
// `render_frame_host` belongs to a MoltNet profile.
void RegisterMoltNetFrameBinders(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

// For renderer processes. Appends switches::kMoltNetRenderer
// (molt_net_webrtc_policy.h) if `host` belongs to a MoltNet profile. No-op
// for a null host or command line.
void AppendMoltNetRendererSwitches(content::RenderProcessHost* host,
                                   base::CommandLine* command_line);

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_H_
