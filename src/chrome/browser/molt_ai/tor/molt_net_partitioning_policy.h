// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The process-wide switches MoltNet's per-site isolation depends on.
//
// Most of the state a network context keeps per site is keyed by the
// request's NetworkAnonymizationKey or NetworkIsolationKey only while a
// base::Feature says so, and for three of them Chromium 152 decides that for
// the whole process, not per network context: a command-line
// --disable-features, a field trial (MoltBrowser builds apply Chromium's
// field-trial testing config, and a variations seed can be loaded with
// command-line switches) or a test harness can turn them off for every
// profile at once. Each one is required here:
//
//  * Connections: net::NetworkAnonymizationKey::IsPartitioningEnabled()
//    (PartitionConnectionsByNetworkIsolationKey). It keys socket pools,
//    HTTP/2 and QUIC sessions, TLS session resumption and the server
//    properties (alternative services, HTTP/2 support, connection
//    statistics) by top-level site, and without it every MoltNet
//    connection goes to Tor with the same SOCKS username, so sites stop
//    getting circuits of their own (net/socket/socks_connect_job.cc).
//  * The HTTP cache: net::HttpCache::IsSplitCacheEnabled()
//    (SplitCacheByNetworkIsolationKey). It keys cache entries by top-level
//    site and frame site, and keeps the one-off keys of certificate
//    fetches out of the cache.
//  * Site storage in third-party frames:
//    net::features::kThirdPartyStoragePartitioning
//    (blink::StorageKey::IsThirdPartyStoragePartitioningEnabled()). It
//    keys a third-party frame's storage, BroadcastChannel, Web Locks,
//    shared workers and service workers by the top-level site; without it
//    they are keyed by the frame's own origin, the same on every site,
//    wherever the frame may use them.
//
// None of them can be set for MoltNet's network context alone, so a MoltNet
// window doesn't open while any of them is off (MoltNetRouting checks this
// before it starts Tor and again before it creates the MoltNet profile), and
// the error names the switch. Regular windows keep following the flags.
// The per-site settings that can be set per network context are forced for
// MoltNet's instead (molt_net_network_context_policy.h).
//
// Header-only and std-only: molt_ai_unittests tests it, and the glue that
// reads the real switches is in molt_net_routing.cc.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARTITIONING_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARTITIONING_POLICY_H_

#include <string>
#include <string_view>

namespace molt_ai::tor {

// The state of the process-wide switches, as Chromium's own code reads them
// (see above). Defaults: off, so a field nobody filled in refuses.
struct MoltNetProcessWidePartitioning {
  bool connections_partitioned = false;
  bool http_cache_split = false;
  bool third_party_storage_partitioned = false;
};

// The base::Feature names behind each switch, as --disable-features and
// field trials spell them (molt_ai_unittests checks them against the
// features themselves).
inline constexpr std::string_view kMoltNetConnectionsFeature =
    "PartitionConnectionsByNetworkIsolationKey";
inline constexpr std::string_view kMoltNetHttpCacheFeature =
    "SplitCacheByNetworkIsolationKey";
inline constexpr std::string_view kMoltNetThirdPartyStorageFeature =
    "ThirdPartyStoragePartitioning";

// Empty if MoltNet may open. Otherwise the error MoltNet shows instead of a
// window, naming every switch that is off.
inline std::string MoltNetPartitioningError(
    const MoltNetProcessWidePartitioning& state) {
  std::string off;
  auto add = [&off](std::string_view feature) {
    if (!off.empty()) {
      off += ", ";
    }
    off += feature;
  };
  if (!state.connections_partitioned) {
    add(kMoltNetConnectionsFeature);
  }
  if (!state.http_cache_split) {
    add(kMoltNetHttpCacheFeature);
  }
  if (!state.third_party_storage_partitioned) {
    add(kMoltNetThirdPartyStorageFeature);
  }
  if (off.empty()) {
    return std::string();
  }
  return "MoltNet keeps each site's connections, cache and storage apart, "
         "and a command-line flag or field trial has turned that off for "
         "the whole browser (" +
         off +
         "), so MoltNet can't open. Restart MoltBrowser without that "
         "setting to use MoltNet.";
}

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_PARTITIONING_POLICY_H_
