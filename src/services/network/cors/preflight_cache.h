// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_CORS_PREFLIGHT_CACHE_H_
#define SERVICES_NETWORK_CORS_PREFLIGHT_CACHE_H_

#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "base/component_export.h"
#include "base/unguessable_token.h"            // MoltBrowser
#include "net/base/molt_network_key_site.h"    // MoltBrowser
#include "net/base/molt_site_share_policy.h"   // MoltBrowser
#include "net/base/network_isolation_key.h"
#include "net/http/http_request_headers.h"
#include "services/network/cors/preflight_result.h"
#include "services/network/public/mojom/clear_data_filter.mojom-forward.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/origin.h"

class GURL;

namespace net {
class NetLogWithSource;
}  // namespace net

namespace network {

namespace cors {

// A class to implement CORS-preflight cache that is defined in the fetch spec,
// https://fetch.spec.whatwg.org/#concept-cache.
// TODO(toyoshim): We may consider to clear all cached entries when users'
// network configuration is changed.
class COMPONENT_EXPORT(NETWORK_SERVICE) PreflightCache final {
 public:
  PreflightCache();

  PreflightCache(const PreflightCache&) = delete;
  PreflightCache& operator=(const PreflightCache&) = delete;

  ~PreflightCache();

  // Appends new `preflight_result` entry to the cache for a specified `origin`
  // and `url`.
  void AppendEntry(const url::Origin& origin,
                   const GURL& url,
                   const net::NetworkIsolationKey& network_isolation_key,
                   std::unique_ptr<PreflightResult> preflight_result);

  // Consults with cached results, and decides if we can skip CORS-preflight or
  // not.
  bool CheckIfRequestCanSkipPreflight(
      const url::Origin& origin,
      const GURL& url,
      const net::NetworkIsolationKey& network_isolation_key,
      mojom::CredentialsMode credentials_mode,
      const std::string& method,
      const net::HttpRequestHeaders& headers,
      bool is_revalidating,
      const net::NetLogWithSource& net_log,
      bool acam_preflight_spec_conformant,
      bool is_ad_auction_trusted_signals_request);

  void ClearCache(mojom::ClearDataFilterPtr url_filter);

  // MoltBrowser: removes the entries kept under a network key with `nonce`
  // (a gone page's credentialless frames', which no other request carries:
  // services/network/molt_nonce_drop_policy.h).
  void MoltClearForNonce(const base::UnguessableToken& nonce);

  // MoltBrowser: from now on each site (the network isolation key's
  // top-level site) keeps a share of the cache of its own
  // (net/base/molt_site_share_policy.h, kMoltPreflightShares): a new result
  // takes the place of its own site's least recently used one, never
  // another site's, or isn't kept; nothing is purged at random. Called by
  // MoltNet's network context before its first request.
  void MoltKeepPerSiteShares();

  // Counts cached entries for testing.
  size_t CountEntriesForTesting() const;

  bool DoesEntryExistForTesting(
      const url::Origin& origin,
      const std::string& url,
      const net::NetworkIsolationKey& network_isolation_key);

  // Purges one cache entry if number of entries is larger than
  // `max_entries` for testing.
  void MayPurgeForTesting(size_t max_entries, size_t purge_unit);

 private:
  void MayPurge(size_t max_entries, size_t purge_unit);

  // MoltBrowser: the cache's key, and its record of each site's entries
  // with per-site shares (MoltKeepPerSiteShares()).
  using MoltKey = std::tuple<url::Origin, std::string, net::NetworkIsolationKey>;
  using MoltShares = net::MoltSiteShares<net::MoltNetworkKeySite, MoltKey>;

  // A map for caching. This is accessed by a tuple of origin,
  // url string, and NetworkIsolationKey to find a cached entry.
  std::map<std::tuple<url::Origin /* origin */,
                      std::string /* url */,
                      net::NetworkIsolationKey /* NIK */>,
           std::unique_ptr<PreflightResult>>
      cache_;

  // MoltBrowser: with per-site shares, each site's entries in order of use;
  // else null.
  std::unique_ptr<MoltShares> molt_shares_;
};

}  // namespace cors

}  // namespace network

#endif  // SERVICES_NETWORK_CORS_PREFLIGHT_CACHE_H_
