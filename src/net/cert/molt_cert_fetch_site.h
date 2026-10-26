// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the site a certificate fetch counts against in MoltNet.
//
// To check a server's certificate the verifier may download a missing
// intermediate certificate (AIA) and, with online revocation checks on, an
// OCSP response or a CRL. In MoltNet each such download is made with a
// one-off network key of its own (net::IsolationInfo::CreateTransient()):
// never cached, never sharing a connection or a Tor circuit. No site could
// be named for it, so all of them shared one small pool and one small set
// of SOCKS handshake slots (net/socket/molt_per_site_socket_pool_policy.h),
// whatever site's connection needed them: one site's servers could keep
// another site's certificate checks waiting.
//
// So the site travels with the verification, for MoltNet's network
// contexts:
//
//  * SSLClientSocketImpl names its connection's site (MoltBudgetSiteFor() of
//    its network key, serialized) on CertVerifier::RequestParams
//    (MoltSetCertFetchSite()), where its SSLClientContext asks for it
//    (MoltCarryCertFetchSite(): MoltNet's contexts);
//  * the request goes to the certificate verifier service over mojo with it
//    (cert_verifier.mojom.RequestParams.molt_cert_fetch_site);
//  * MultiThreadedCertVerifier sets it for the worker thread while the
//    verification runs there (MoltScopedCertFetchSite), and every fetch the
//    verification starts on that thread reads it
//    (CertNetFetcherURLLoader::DoFetch(): MoltGetCertFetchSite());
//  * the fetch then gets a one-off key of that site
//    (net::IsolationInfo::Create() with the site as top-level site and a
//    nonce of its own), so it still shares nothing, but counts against the
//    site's own connection pools and handshake slots.
//
// Every other context names no site, and its verifications and fetches are
// Chromium's.

#ifndef NET_CERT_MOLT_CERT_FETCH_SITE_H_
#define NET_CERT_MOLT_CERT_FETCH_SITE_H_

#include <string>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "net/base/net_export.h"

namespace net {

// The site the certificate verification running on this thread is for, or
// null (net/cert/cert_verifier.cc).
NET_EXPORT const std::string* MoltGetCertFetchSite();
NET_EXPORT void MoltSetCertFetchSite(const std::string* site);

// Names `site` ("" for none) for the verification running on this thread,
// for as long as it lives (it keeps its own copy), then what was named
// before.
class MoltScopedCertFetchSite {
 public:
  explicit MoltScopedCertFetchSite(std::string site)
      : site_(std::move(site)), previous_(MoltGetCertFetchSite()) {
    MoltSetCertFetchSite(site_.empty() ? nullptr : &site_);
  }
  MoltScopedCertFetchSite(const MoltScopedCertFetchSite&) = delete;
  MoltScopedCertFetchSite& operator=(const MoltScopedCertFetchSite&) = delete;
  ~MoltScopedCertFetchSite() { MoltSetCertFetchSite(previous_); }

 private:
  const std::string site_;
  const raw_ptr<const std::string> previous_;
};

}  // namespace net

#endif  // NET_CERT_MOLT_CERT_FETCH_SITE_H_
