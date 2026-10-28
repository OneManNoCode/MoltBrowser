// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what MoltNet's network context drops of what a page's nonce
// keys, and when (NetworkContext::MoltDropNonce(), asked by the browser's
// chrome/browser/molt_ai/tor/molt_net_nonce_storage.cc).
//
// A credentialless iframe's requests, and those of what it holds, carry its
// page's nonce in their network keys (net::NetworkIsolationKey and
// net::NetworkAnonymizationKey), and no request of any other page ever
// carries that nonce. What the network context keeps under such a key, at
// Chromium 156.0.8078.74:
//  * cookies: in partitions keyed by the nonce (net/cookies/
//    molt_nonce_cookie_policy.h says what MoltNet's cookie store keeps);
//  * idle connections and HTTP/2 sessions: the socket pools' groups
//    (net::ClientSocketPool::GroupId) and net::SpdySessionKey carry the
//    request's network anonymization key, so only the nonce's requests reuse
//    them. 156 can close a pool group's idle sockets
//    (TransportClientSocketPool::CloseIdleSocketsInGroup()), but nothing
//    finds a key's groups, and HTTP/2 sessions only close all together
//    (SpdySessionPool::CloseCurrentIdleSessions());
//  * TLS sessions: net::SSLClientSessionCache::Key carries the network
//    anonymization key, so only the nonce's connections resume them; 156
//    flushes them by server only (FlushForServers());
//  * server properties (HTTP/2 support, alternative services, network
//    statistics, servers requiring HTTP/1.1, canonical hosts, QUIC server
//    information): keyed by server and network anonymization key; 156 clears
//    them all or one server's statistics;
//  * CORS preflight results: network::cors::PreflightCache is keyed by
//    origin, URL and network isolation key; 156 clears them by URL filter
//    only;
//  * HTTP authentication entries: net::HttpAuthCache keys server entries by
//    network anonymization key (always, in MoltNet's context); 156 clears
//    them by URL only.
// Each of these is keyed by the network partitioning switches MoltNet
// requires (net::features::kPartitionConnectionsByNetworkIsolationKey and
// the split HTTP authentication cache: chrome/browser/molt_ai/tor/
// molt_net_partitioning_policy.h), so once the nonce's page is gone none of
// it can be used again, but it stayed until a cache's own eviction (1,024
// TLS sessions, 500 server entries, 1,024 preflight results and 20
// authentication entries, shared by every site) or an idle timeout. Here
// the network context drops each by the nonce, with one small method per
// cache (MoltCloseIdleSocketsOfNonce(), MoltCloseIdleSessionsOfNonce(),
// MoltFlushForNonce(), MoltClearForNonce(), MoltClearEntriesForNonce(),
// MoltClearCorsPreflightCacheForNonce()), each a pass over that cache, of a
// size fixed for the whole context. Connections in use stay (a request that
// outlives its page); they go idle afterwards and are dropped the next time.
// Marks of broken alternative services keep Chromium's own expiry (five
// minutes, doubling; at most 200 kept as recently broken). Nothing is
// flushed for every key.
//
// The steps (network::mojom::MoltNonceDrop):
//  * kClosed: the page lives on past its site's bound (its group closed:
//    chrome/browser/molt_ai/tor/molt_net_nonce_storage_policy.h). Its cookies
//    are deleted and further ones refused; its connections are in use.
//  * kGone: the page has gone. The same, and what the caches above keep
//    under the nonce is dropped.
//  * kForget: kMoltNetNonceCookieSweepSeconds after it went, past the
//    longest a request of the page outlives it. Both again, for what such a
//    request added meanwhile, and the nonce is forgotten.
// A context the browser makes again for a storage partition (the network
// service restarted) starts with the nonces the partition's earlier context
// had been sent kClosed or kGone for and not yet kForget
// (NetworkContextParams::molt_refused_nonces): its cookie store refuses
// them as it is made, before the context serves any request
// (CookieMonster::MoltRefuseNonces()); kForget comes later, as for the
// earlier context.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef SERVICES_NETWORK_MOLT_NONCE_DROP_POLICY_H_
#define SERVICES_NETWORK_MOLT_NONCE_DROP_POLICY_H_

namespace network {

// network::mojom::MoltNonceDrop, without Mojo.
enum class MoltNonceDropStep {
  kClosed,
  kGone,
  kForget,
};

// Whether cookies for the nonce's partitions are refused after the step.
constexpr bool MoltNonceCookiesRefusedAfter(MoltNonceDropStep step) {
  return step != MoltNonceDropStep::kForget;
}

// Whether the step drops the idle connections and the network caches kept
// under the nonce (its page has gone).
constexpr bool MoltNonceNetworkStateDropped(MoltNonceDropStep step) {
  return step != MoltNonceDropStep::kClosed;
}

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_NONCE_DROP_POLICY_H_
