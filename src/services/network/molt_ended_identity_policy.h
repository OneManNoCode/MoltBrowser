// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what a MoltNet network context does when its identity ends.
//
// The browser ends a MoltNet identity (New Identity, or a setting the live
// check refuses) by sending its network contexts a proxy configuration that
// refuses every request: a proxy list with no proxy and no DIRECT
// (chrome/browser/molt_ai/tor/molt_net_routing.cc: RefuseAllProxyConfig()).
// From then on the context resolves every new request to no proxy, so it
// fails before it connects. What was already under way would carry on:
// a request whose proxy was resolved before (waiting for a connection, in its
// SOCKS handshake, sending its body, following a redirect on an open
// connection), and every connection still open, so bytes the identity had
// queued could still leave after its end.
//
// So a context with NetworkContextParams::molt_stop_all_when_refusing
// (MoltNet's) stops everything in the same task in which the refusal is
// applied (ProxyConfigServiceMojo::OnProxyConfigUpdated(), after its
// observers, the proxy resolution service among them, have the new
// configuration): every URL request of the context is cancelled, and every
// connection, connection attempt and HTTP/2 session is closed
// (network_context.cc). Nothing the identity started is sent after its
// network context has the refusal; anything the browser still holds for
// its pages it refuses to send from the moment the identity ends
// (ContentBrowserClient::HasNetworkIdentityEnded()).
//
// Std-only, so molt_ai_unittests tests it.

#ifndef SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_
#define SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_

namespace network {

// Whether a proxy configuration refuses every request: a single proxy list
// (`is_single_proxy_list`) without any entry (`proxy_list_empty`), not even
// DIRECT, and no automatic settings (PAC or auto-detect) that could give
// one.
constexpr bool MoltIsRefuseAllProxyConfig(bool is_single_proxy_list,
                                          bool proxy_list_empty,
                                          bool has_automatic_settings) {
  return is_single_proxy_list && proxy_list_empty && !has_automatic_settings;
}

// Whether a context stops everything when a new proxy configuration is
// applied: only one that asked to (`stop_all_when_refusing`), only for a
// configuration that refuses every request, and only on a change (the
// first configuration a context gets is applied before it has anything to
// stop).
constexpr bool MoltStopsEverythingOnConfig(bool stop_all_when_refusing,
                                           bool refuses_all,
                                           bool changed) {
  return stop_all_when_refusing && refuses_all && changed;
}

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_ENDED_IDENTITY_POLICY_H_
