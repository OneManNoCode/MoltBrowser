// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a page's Background Fetch download that the download service
// takes up again in a later browser session carries what the fetch's request
// carried, in every profile.
//
// A Background Fetch request is a download the download service makes for
// the fetch (DownloadClient::BACKGROUND_FETCH), with the fetch's origin as
// initiator, the network isolation of the page or service worker that
// started it, its credentials mode, and its first party kept on redirects
// (BackgroundFetchDelegateBase::DownloadUrl()). The download service keeps
// its downloads across browser sessions, but at Chromium 156 it stores only
// the URL, method, headers and credentials mode (proto_conversions.cc); the
// initiator and isolation, which can't be stored, come back from the client
// when the download starts again in a new session
// (Client::GetUploadData(), ControllerImpl::OnDownloadReadyToStart()).
// There:
//  * a download whose item the browser kept from the earlier session (the
//    in-progress database: DownloadItemImpl without an initiator or
//    isolation) was resumed (DownloadDriverImpl::ResumeWithFactory()) with
//    none of them, so the resuming request was made as one the browser
//    starts itself: no initiator, its destination's own site as first
//    party, and credentials whatever the fetch asked for
//    (components/download/internal/common/molt_resumed_fetch.h covers
//    only items that still have their first request's metadata);
//  * a download started afresh changed its first party on redirects, as a
//    navigation does (RequestParams::update_first_party_url_on_redirect
//    isn't stored either).
//
// So, once a page's fetch's parameters are back (they name an initiator,
// which only Background Fetch gives), the download keeps its first party on
// redirects, and one whose item is from the earlier session is started
// again from its beginning with them, as the upload of a request body
// already is (ControllerImpl::UpdateDriverState()), rather than resumed
// without them. A fetch that comes back without its parameters, and every
// other client's download, goes on as in Chromium.
//
// Header-only and templated on the request parameters (RequestParams), so
// the download service's target needs no new dependency and
// molt_ai_unittests tests it.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_RESTORED_FETCH_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_RESTORED_FETCH_H_

namespace download {

// Whether a download whose request parameters name an initiator
// (`has_initiator`: a page's fetch) and whose item exists already
// (`has_driver_entry`: kept from an earlier session) is started again
// rather than resumed.
constexpr bool MoltRestartsRestoredFetch(bool has_initiator,
                                         bool has_driver_entry) {
  return has_initiator && has_driver_entry;
}

// ControllerImpl::OnDownloadReadyToStart(), once the client's parameters
// are on `request_params` (RequestParams): a page's fetch keeps its first
// party on redirects. Returns whether the download, whose item exists
// (`has_driver_entry`), is to be started again rather than resumed
// (MoltRestartsRestoredFetch()).
template <typename RequestParams>
bool MoltPrepareRestoredFetch(RequestParams& request_params,
                              bool has_driver_entry) {
  const bool has_initiator = request_params.initiator.has_value();
  if (has_initiator) {
    request_params.update_first_party_url_on_redirect = false;
  }
  return MoltRestartsRestoredFetch(has_initiator, has_driver_entry);
}

}  // namespace download

#endif  // COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_RESTORED_FETCH_H_
