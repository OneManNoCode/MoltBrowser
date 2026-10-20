// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The frame binders that keep WebRTC networking away from a MoltNet frame
// (molt_net_webrtc.h). Needs no Profile or content implementation, so
// molt_ai_unittests drives it over real mojo pipes.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_BINDERS_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_BINDERS_H_

#include "chrome/common/molt_ai/tor/molt_net_webrtc_policy.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/network/public/cpp/network_service_buildflags.h"

#if BUILDFLAG(IS_P2P_ENABLED)
#include "services/network/public/mojom/p2p.mojom-forward.h"
#endif

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::tor {

// Replaces the binder of each interface `access` refuses (and adds one
// where content registered none, e.g. in fenced frames: a renderer that
// asks for an interface with no binder is killed). Leaves `map` alone for
// Chromium's default access.
//   * !p2p_socket_manager: network::mojom::P2PSocketManager is bound to
//     BindRefusingP2PSocketManager().
//   * !mdns_responder: network::mojom::MdnsResponder pipes are closed. The
//     renderer's MdnsResponderAdapter never reconnects and its calls on the
//     closed pipe are dropped.
// `peer_connections` is the renderer's part (AppendMoltNetRendererSwitches).
void ApplyWebRtcNetworkAccessToFrameBinders(
    const WebRtcNetworkAccess& access,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

#if BUILDFLAG(IS_P2P_ENABLED)
// Binds `receiver` to a P2PSocketManager that never reaches the network,
// for as long as the renderer keeps the pipe open:
//   * GetHostAddress(): an empty list (how the network service reports a
//     failed lookup) for every name, ".local" names included. No DNS,
//     DNS-over-HTTPS, mDNS or hosts-file lookup.
//   * CreateSocket(): no socket. Both ends are dropped, which the renderer
//     reports as a socket error, as for a socket the network service
//     refuses.
//   * StartNetworkNotifications(): one empty interface list with empty
//     default addresses, so the renderer stops waiting for one.
// The pipe itself stays open: the renderer's P2PSocketDispatcher reconnects
// whenever it closes.
void BindRefusingP2PSocketManager(
    mojo::PendingReceiver<network::mojom::P2PSocketManager> receiver);
#endif

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_WEBRTC_BINDERS_H_
