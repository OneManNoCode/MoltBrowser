// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: per-site socket pools for MoltNet's network context.
//
// Chromium keeps one socket pool per proxy chain in each network context
// (ClientSocketPoolManagerImpl), with two limits: 6 connections per group (a
// destination, for one network anonymization key, ...) and 128 per proxy
// chain for the whole pool (ClientSocketPoolManager::
// max_sockets_per_proxy_chain(), which the MaxConnectionsPerProxy policy
// changes). All of MoltNet's traffic goes through one proxy chain, Tor's
// SOCKS5 port, so every site shares that pool. One site that keeps 128
// connections open (to 22 hosts, or to slow hosts of its own) makes every
// other site's new connections wait until one of its own closes, and a site
// open at the same time can measure that wait: the known connection-pool
// cross-site leaks.
//
// In MoltNet's network context each top-level site (each network
// anonymization key, which is what a site means everywhere else in
// MoltNet's network state) gets a socket pool of its own for a proxy chain
// (molt_per_site_socket_pools.h): the same TransportClientSocketPool,
// with the same per-group limit, and a fixed limit of its own
// (kMoltPerSitePoolSize, Chromium's default per-proxy limit) that the
// MaxConnectionsPerProxy policy doesn't change, so a page can't tell how
// an administrator set it. Nothing a site does with its connections changes
// when another site's requests get theirs. A site's pool is dropped once it
// has had no socket, request or pending callback, and hasn't been handed out,
// for kMoltUnusedSitePoolTimeoutSeconds; one-off keys (certificate fetches,
// opaque origins) each get one, so this bounds the memory they cost.
//
// Every other network context keeps Chromium's single pool per proxy chain.
// Header-only and std-only, so molt_ai_unittests and a standalone harness
// test it.

#ifndef NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_
#define NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace net {

// The limit of each site's pool in MoltNet's network context: Chromium's
// default per-proxy-chain limit (client_socket_pool_manager.cc), so one site
// alone can have as many connections as it could before.
inline constexpr size_t kMoltPerSitePoolSize = 128;

// How long a site's pool may sit unused before it is dropped.
inline constexpr int64_t kMoltUnusedSitePoolTimeoutSeconds = 60;

// Whether requests through a proxy chain get a pool per site:
// `per_site_pool_size` is the network context's setting (0: Chromium's one
// pool per proxy chain). Direct connections never do (MoltNet makes none;
// its context only ever has Tor's proxy configuration, or one that refuses
// every request).
constexpr bool MoltUsesPerSitePool(size_t per_site_pool_size,
                                   bool proxy_chain_is_direct) {
  return per_site_pool_size > 0 && !proxy_chain_is_direct;
}

// The per-group limit of a site's pool: Chromium's for the pool type,
// never more than the pool's own limit (as for a proxy chain's pool).
constexpr size_t MoltPerSiteGroupLimit(size_t per_site_pool_size,
                                       size_t max_sockets_per_group) {
  return std::min(per_site_pool_size, max_sockets_per_group);
}

// Whether a site's pool may be dropped: nothing uses it (no group, socket,
// connect job, pending callback or higher-layered pool) and it hasn't been
// handed out for the timeout. A pool that is handed out is used at once
// (ClientSocketHandle::Init(), in the same task), so a pool that was
// unused at both ends of the timeout holds nothing a request could need.
constexpr bool MoltMayDropSitePool(bool pool_unused,
                                   int64_t seconds_since_handed_out) {
  return pool_unused &&
         seconds_since_handed_out >= kMoltUnusedSitePoolTimeoutSeconds;
}

}  // namespace net

#endif  // NET_SOCKET_MOLT_PER_SITE_SOCKET_POOL_POLICY_H_
