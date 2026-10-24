// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the site a URLLoaderFactory's keepalive requests count
// against in a MoltNet network context (molt_keepalive_partitions.h): the
// factory's top-level site; for an opaque top-level origin (a page its
// server sandboxed), the site it was derived from, as MoltNet's connection
// limits count it (net/socket/molt_connection_budget.h); "" for a factory
// without one. Header-only: services/network's sources are Chromium's
// BUILD.gn, which MoltBrowser doesn't change.

#ifndef SERVICES_NETWORK_MOLT_KEEPALIVE_SITE_H_
#define SERVICES_NETWORK_MOLT_KEEPALIVE_SITE_H_

#include <optional>
#include <string>

#include "net/base/isolation_info.h"
#include "net/base/schemeful_site.h"
#include "url/origin.h"
#include "url/scheme_host_port.h"

namespace network {

inline std::string MoltKeepaliveSite(const net::IsolationInfo& isolation_info) {
  const std::optional<url::Origin>& top_frame_origin =
      isolation_info.top_frame_origin();
  if (!top_frame_origin) {
    return std::string();
  }
  const url::SchemeHostPort tuple =
      top_frame_origin->GetTupleOrPrecursorTupleIfOpaque();
  if (!tuple.IsValid()) {
    return std::string();
  }
  return net::SchemefulSite(tuple.GetURL()).Serialize();
}

}  // namespace network

#endif  // SERVICES_NETWORK_MOLT_KEEPALIVE_SITE_H_
