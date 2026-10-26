// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the request that resumes a page's Background Fetch download
// carries what the download's first request carried, in every profile.
//
// In a regular profile a Background Fetch request is a download the
// download service makes (DownloadSource::INTERNAL_API), with the fetch's
// origin as initiator, the network isolation of the page or service worker
// that started it, and the fetch's credentials mode
// (BackgroundFetchDelegateBase::DownloadUrl(), DownloadDriverImpl::Start(),
// molt_fetch_credentials.h). A download whose response stops early (a
// connection that drops, fewer bytes than its Content-Length, a server that
// ignores the range asked for) is resumed: at once and up to five times
// (DownloadItemImpl::AutoResumeIfValid()), and again when the download
// service retries it. At Chromium 156 the resuming request
// (DownloadItemImpl::ResumeInterruptedDownload()) carries none of the first
// request's initiator, isolation or credentials mode, so
// download::CreateResourceRequest() makes it as a top-level navigation the
// browser starts itself: no initiator (`Sec-Fetch-Site: none`), the
// destination's own site as first party (its SameSite=Strict and Lax
// cookies sent, third-party cookie blocking and storage partitioning not
// applied), and credentials whatever the fetch asked for; and the server
// a fetch goes to can end its own response early.
//
// So a resumed download the download service made for a page (its first
// request had an initiator, which only a page's fetch sets) keeps that
// initiator, its first request's isolation, and its credentials mode, and
// keeps its first party on redirects, as its first request did. Every
// other download (links, "Save as", the download service's other clients,
// none of which has an initiator there) is resumed as in Chromium.
//
// Header-only and templated on the download parameters
// (DownloadUrlParameters), so the download component's own target needs no
// new dependency and molt_ai_unittests tests it.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_RESUMED_FETCH_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_RESUMED_FETCH_H_

#include <optional>

#include "net/base/isolation_info.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/origin.h"

namespace download {

// Whether the request that resumes a download carries its first request's
// initiator, isolation and credentials mode: for a download the download
// service made (`from_download_service`) whose first request had an
// initiator (`had_initiator`): a page's fetch.
constexpr bool MoltResumeKeepsFetchMetadata(bool from_download_service,
                                            bool had_initiator) {
  return from_download_service && had_initiator;
}

// DownloadItemImpl::ResumeInterruptedDownload(): puts the download's first
// request's `initiator`, `isolation_info` (when it has one) and
// `credentials_mode` on `params`, the resuming request's parameters, for a
// download that keeps them (MoltResumeKeepsFetchMetadata()). `Params` is
// DownloadUrlParameters.
template <typename Params>
void MoltKeepResumedFetchMetadata(
    bool from_download_service,
    const std::optional<url::Origin>& initiator,
    const std::optional<net::IsolationInfo>& isolation_info,
    network::mojom::CredentialsMode credentials_mode,
    Params& params) {
  if (!MoltResumeKeepsFetchMetadata(from_download_service,
                                    initiator.has_value())) {
    return;
  }
  params.set_initiator(initiator);
  if (isolation_info && !isolation_info->IsEmpty()) {
    params.set_isolation_info(*isolation_info);
  }
  params.set_update_first_party_url_on_redirect(false);
  params.set_credentials_mode(credentials_mode);
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_COMMON_MOLT_RESUMED_FETCH_H_
