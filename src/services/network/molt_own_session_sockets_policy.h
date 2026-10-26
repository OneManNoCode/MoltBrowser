// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: no sockets outside a MoltNet network context's own HTTP
// network session.
//
// At Chromium 156 the network service builds an HttpNetworkSession of its
// own from a network context's URLRequestContext, with Chromium's default
// session parameters, in ProxyResolvingClientSocketFactory
// (services/network/proxy_resolving_client_socket_factory.cc). Its creators:
//  * NetworkContext::CreateProxyResolvingSocketFactory() (through
//    ProxyResolvingSocketFactoryMojo), which the browser asks of the
//    system network context (BrowserProcessImpl) and of a profile's default
//    partition (GCMProfileServiceFactory) for GCM, and Fuchsia's WebEngine
//    for push messaging; and
//  * P2PSocketManager::CreateSocket() (services/network/p2p/), for
//    WebRTC's TCP sockets, in the manager NetworkContext::
//    CreateP2PSocketManager() makes for a frame's renderer
//    (P2PSocketDispatcherHost).
// (Its tests and google_apis/gcm's mcs_probe tool make their own.) Such a
// session uses the context's proxy, host resolver and certificate
// verifier, but not what MoltNet's context adds to its own session: the
// SOCKS isolation password (its connections would go to Tor with the
// password every other context sends, so a site's connections from two
// identities could share a circuit), the per-site socket pools, and the
// end of the identity (its connections would not be refused when the
// identity ends).
//
// No MoltNet feature uses either (MoltNet profiles have no GCM, and their
// frames get a P2PSocketManager that reaches nothing: molt_net_webrtc.h), so
// a network context that has any of those settings makes neither: its
// CreateProxyResolvingSocketFactory() drops the factory's pipe, and its
// CreateP2PSocketManager() binds the renderer's pipe to a manager that
// reaches nothing (molt_refusing_p2p_socket_manager.h; a closed pipe would
// make the renderer ask again at once), so no
// ProxyResolvingClientSocketFactory is ever made for it.
//
// The network context's raw sockets go further: no session, no proxy. At
// 156 its entry points are CreateTCPConnectedSocket(),
// CreateTCPServerSocket(), CreateTCPBoundSocket(), CreateUDPSocket(),
// CreateRestrictedUDPSocket(), CreateSocketFactory() (the same TCP and UDP
// sockets behind a narrower interface) and CreateMdnsResponder() (multicast
// DNS on the local network); the browser asks for them for Direct Sockets
// (isolated web apps), Chrome Apps' socket APIs, Cast and WebRTC's mDNS
// names, none of which a MoltNet window has. A network context with any of
// MoltNet's settings refuses each: those with a reply answer
// ERR_ACCESS_DENIED (with no address and no data pipes), the others drop
// their pipes.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef SERVICES_NETWORK_MOLT_OWN_SESSION_SOCKETS_POLICY_H_
#define SERVICES_NETWORK_MOLT_OWN_SESSION_SOCKETS_POLICY_H_

#include <cstdint>

namespace network {

// Whether a network context may make sockets with an HTTP network session
// of their own: not if its connections carry a SOCKS isolation password
// (`socks_isolation_password`: NetworkContextParams'
// molt_socks_isolation_password is set), use per-site socket pools
// (`per_site_socket_pool_size` != 0) or end with their identity
// (`stops_all_when_refusing`).
constexpr bool MoltAllowsOwnSessionSockets(bool socks_isolation_password,
                                           uint32_t per_site_socket_pool_size,
                                           bool stops_all_when_refusing) {
  return !socks_isolation_password && per_site_socket_pool_size == 0 &&
         !stops_all_when_refusing;
}

static_assert(MoltAllowsOwnSessionSockets(false, 0, false));
static_assert(!MoltAllowsOwnSessionSockets(true, 0, false));
static_assert(!MoltAllowsOwnSessionSockets(false, 6, false));
static_assert(!MoltAllowsOwnSessionSockets(false, 0, true));

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_OWN_SESSION_SOCKETS_POLICY_H_
