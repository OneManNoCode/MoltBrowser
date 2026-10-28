// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the site a network key's state counts against in MoltNet's
// network context: the key's top-level site, for its first-party and
// third-party keys alike; for an opaque top-level site (a page its server
// sandboxed), the site the opaque origin was derived from. nullopt for the
// keys no site can be named for (a one-off key derived from nothing:
// net::IsolationInfo::CreateTransient(), an empty key). A key with a nonce
// (a page's credentialless frames') counts against its page's top-level
// site.
//
// Used by the per-site socket pools and connection budgets
// (net/socket/molt_connection_budget.h: MoltBudgetSiteFor()) and by the
// per-site shares of the fixed-size caches (molt_site_share_policy.h).

#ifndef NET_BASE_MOLT_NETWORK_KEY_SITE_H_
#define NET_BASE_MOLT_NETWORK_KEY_SITE_H_

#include <optional>

#include "net/base/network_anonymization_key.h"
#include "net/base/network_isolation_key.h"
#include "net/base/schemeful_site.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace net {

using MoltNetworkKeySite = std::optional<SchemefulSite>;

// The site of a top-level site `top_frame_site` as above.
inline MoltNetworkKeySite MoltSiteOfTopFrameSite(
    const std::optional<SchemefulSite>& top_frame_site) {
  if (!top_frame_site) {
    return std::nullopt;
  }
  if (!top_frame_site->opaque()) {
    return *top_frame_site;
  }
  const url::SchemeHostPort precursor =
      top_frame_site->MoltGetTupleOrPrecursorTupleIfOpaque();
  if (!precursor.IsValid()) {
    return std::nullopt;
  }
  SchemefulSite site(precursor.GetURL());
  if (site.opaque()) {
    return std::nullopt;
  }
  return site;
}

inline MoltNetworkKeySite MoltSiteOfNetworkKey(
    const NetworkAnonymizationKey& key) {
  return MoltSiteOfTopFrameSite(key.GetTopFrameSite());
}

inline MoltNetworkKeySite MoltSiteOfNetworkKey(const NetworkIsolationKey& key) {
  return MoltSiteOfTopFrameSite(key.GetTopFrameSite());
}

}  // namespace net

#endif  // NET_BASE_MOLT_NETWORK_KEY_SITE_H_
