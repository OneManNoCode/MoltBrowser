// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a download whose parameters carry a credentials mode other
// than "include" is made with that mode, in every profile.
//
// Background Fetch passes each fetch's credentials mode to the download
// service (BackgroundFetchDelegateBase::DownloadUrl(),
// download::RequestParams::credentials_mode), whose download driver puts it
// in the download's parameters (DownloadUrlParameters::
// set_credentials_mode()). Up to 156.0.8078.100 Chromium's download request
// (download::CreateResourceRequest()) never applied it: every download was
// a navigation-mode request with credentials "include", so a Background
// Fetch whose request asked for `credentials: 'omit'` sent the site's
// cookies and HTTP authentication anyway, and one that asked for
// "same-origin" sent them to other origins too.
//
// Since 156.0.8078.108 Chromium applies the parameters' credentials mode
// itself, with the fetch's own request mode (fetch_request_mode(): "cors"
// for a Background Fetch). What stays here is what that leaves open: a
// request still in navigation mode with another credentials mode
// (parameters that carry no fetch request mode: the in-memory download
// service's, molt_registering_site_isolation.h, and a fetch stored before
// Chromium kept the mode, with kBackgroundFetchCorsEnforcement off). The
// network service accepts only "include" for a navigation-mode request, so
// such a request is made in no-cors mode. "same-origin" needs the
// request's initiator to compare with; without one, no origin is the same,
// so nothing is sent, in any mode. Every other download (links, "Save as",
// resumptions of them, the download service's other clients) carries
// "include" and is unchanged.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_FETCH_CREDENTIALS_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_FETCH_CREDENTIALS_H_

#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"

namespace download {

// The credentials mode a download request is made with, for the
// parameters' `mode`: "include" stays; "same-origin" without an initiator
// (`has_initiator`) sends nothing.
constexpr network::mojom::CredentialsMode MoltDownloadCredentialsMode(
    network::mojom::CredentialsMode mode,
    bool has_initiator) {
  if (mode == network::mojom::CredentialsMode::kSameOrigin && !has_initiator) {
    return network::mojom::CredentialsMode::kOmit;
  }
  return mode;
}

// Whether a download request with credentials mode `mode` stays a
// navigation-mode request: only with "include".
constexpr bool MoltDownloadIsNavigation(network::mojom::CredentialsMode mode) {
  return mode == network::mojom::CredentialsMode::kInclude;
}

// Applies the parameters' credentials mode (`mode`) to `request`, built by
// CreateResourceRequest() with the parameters' fetch request mode: the
// mode MoltDownloadCredentialsMode() gives, and no-cors in place of a
// navigation mode that can't carry it. Another request mode (a Background
// Fetch's "cors") is kept.
inline void MoltApplyDownloadCredentialsMode(
    network::mojom::CredentialsMode mode,
    network::ResourceRequest& request) {
  const network::mojom::CredentialsMode applied =
      MoltDownloadCredentialsMode(mode, request.request_initiator.has_value());
  request.credentials_mode = applied;
  if (request.mode == network::mojom::RequestMode::kNavigate &&
      !MoltDownloadIsNavigation(applied)) {
    request.mode = network::mojom::RequestMode::kNoCors;
  }
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_FETCH_CREDENTIALS_H_
