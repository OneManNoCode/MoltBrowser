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
// set_credentials_mode()). Chromium 156's download request
// (download::CreateResourceRequest()) never applied it: every download is a
// navigation-mode request with credentials "include", so a Background
// Fetch whose request asked for `credentials: 'omit'` sent the site's
// cookies and HTTP authentication anyway, and one that asked for
// "same-origin" sent them to other origins too. The network service accepts
// only "include" for a navigation-mode request, so a request with another
// mode is made in no-cors mode, as the page's own fetch() of it would reach
// the network (Background Fetch refuses requests that would need a CORS
// preflight, and filters cross-origin responses itself). "same-origin"
// needs the request's initiator to compare with; without one, no origin is
// the same, so nothing is sent. Every other download (links, "Save as",
// resumptions of them, the download service's other clients) carries
// "include" and is unchanged. Off the record, Background Fetch goes through
// the in-memory download service instead, which carries the mode itself
// (components/download/internal/background_service/
// molt_registering_site_isolation.h).

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
// CreateResourceRequest() (navigation mode, "include").
inline void MoltApplyDownloadCredentialsMode(
    network::mojom::CredentialsMode mode,
    network::ResourceRequest& request) {
  const network::mojom::CredentialsMode applied =
      MoltDownloadCredentialsMode(mode, request.request_initiator.has_value());
  if (MoltDownloadIsNavigation(applied)) {
    return;
  }
  request.mode = network::mojom::RequestMode::kNoCors;
  request.credentials_mode = applied;
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_FETCH_CREDENTIALS_H_
