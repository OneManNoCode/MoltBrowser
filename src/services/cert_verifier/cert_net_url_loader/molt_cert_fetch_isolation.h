// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the network isolation of a certificate fetch (a missing
// intermediate certificate, an OCSP response, a CRL) in a fetcher that
// isolates each fetch (MoltNet's: CertNetFetcherURLLoader::
// SetIsolateEachFetch()). The fetch is made for a verification that may
// name the site of its connection (net/cert/molt_cert_fetch_site.h): then
// a one-off key of that site, with that site as top-level site and a nonce
// of its own, so that it counts against that site's connection pools and
// SOCKS handshake slots and still shares nothing (no HTTP cache entry is
// kept for a key with a nonce, and its connection and Tor circuit are its
// own); else a one-off key derived from nothing. Header-only: the
// fetcher's sources are Chromium's BUILD.gn, which MoltBrowser doesn't
// change.

#ifndef SERVICES_CERT_VERIFIER_CERT_NET_URL_LOADER_MOLT_CERT_FETCH_ISOLATION_H_
#define SERVICES_CERT_VERIFIER_CERT_NET_URL_LOADER_MOLT_CERT_FETCH_ISOLATION_H_

#include <cstddef>
#include <optional>
#include <string>

#include "base/unguessable_token.h"
#include "net/base/isolation_info.h"
#include "net/cookies/site_for_cookies.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace cert_verifier {

// The longest site name a verification may carry; anything longer is no
// site (a site is a scheme and a registrable domain).
inline constexpr size_t kMoltMaxCertFetchSiteLength = 2048;

inline net::IsolationInfo MoltCertFetchIsolationInfo(const std::string& site) {
  if (!site.empty() && site.size() <= kMoltMaxCertFetchSiteLength) {
    const GURL url(site);
    if (url.is_valid()) {
      const url::Origin origin = url::Origin::Create(url);
      if (!origin.opaque()) {
        return net::IsolationInfo::Create(
            net::IsolationInfo::RequestType::kOther, origin, origin,
            net::SiteForCookies(), base::UnguessableToken::Create());
      }
    }
  }
  return net::IsolationInfo::CreateTransient(/*nonce=*/std::nullopt);
}

}  // namespace cert_verifier

#endif  // SERVICES_CERT_VERIFIER_CERT_NET_URL_LOADER_MOLT_CERT_FETCH_ISOLATION_H_
