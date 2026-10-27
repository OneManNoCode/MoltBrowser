// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a page's Background Fetch request that the browser makes
// without its service worker's URL loader factory is checked for Local
// Network Access all the same, in every profile.
//
// At Chromium 156, with features::kBackgroundFetchLocalNetworkAccess (on by
// default), a regular profile's Background Fetch requests go through a URL
// loader factory made for the registration's service worker, which carries
// the worker's client security state (its IP address space and Local Network
// Access policy) and an observer that asks the user's permission for the
// worker's origin (BackgroundFetchJobController::InitializeUrlLoaderFactory(),
// passed to the download service and on to DownloadUrlParameters). Two
// requests got none, and so went out on the storage partition's default
// factory for the browser process, which carries no client security state:
// the network service doesn't check Local Network Access for them at all, and
// a public site's fetch could reach the user's local network:
//  * a download resumed by the download item itself (DownloadItemImpl::
//    AutoResumeIfValid() when a response stops early or a connection
//    drops), which carries the item's factory only if the download service
//    set one (ResumeWithFactory()), and it didn't for a download it started;
//  * a download the download service resumes after the browser restarted
//    (a request active at shutdown): it asks Background Fetch for the
//    factory (GetUploadData()), and gets none if the registration isn't
//    loaded yet, then resumes with none.
// A server can make the first happen at will (end its response early), and
// a site can make the second likely (keep a fetch running at shutdown).
//
// So a download the download service makes for a page (one with an initiator,
// which only a Background Fetch sets) that comes with no factory of its own
// gets a factory that checks Local Network Access for its page
// (DownloadManagerImpl::BeginResourceDownloadOnChecksComplete(),
// content/browser/download/molt_resumed_fetch_factory.h): the client
// security state of a worker of the fetch's origin served from a public
// address (the policy DeriveLocalNetworkAccessRequestPolicy() gives a worker
// there, with the embedder's override for the origin), and the observer that
// asks the origin's permission, as for the worker's own factory when the
// worker isn't running. The worker's real address space may be less public
// (a site on the local network): then the request is held to a stricter
// policy than its first request was, never a looser one. Other downloads,
// and Background Fetch without the feature, are made as at Chromium 156.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_POLICY_H_
#define CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_POLICY_H_

namespace content {

// Whether a download request gets a factory that checks Local Network Access
// for its page: one the download service makes (`from_download_service`:
// DownloadSource::INTERNAL_API) for a page (`has_initiator`), that comes with
// no factory of its own (`has_factory`: the service worker's), while
// Background Fetch checks Local Network Access (`lna_checked`).
constexpr bool MoltFetchNeedsLnaFactory(bool from_download_service,
                                        bool has_initiator,
                                        bool has_factory,
                                        bool lna_checked) {
  return from_download_service && has_initiator && !has_factory && lna_checked;
}

}  // namespace content

#endif  // CONTENT_BROWSER_DOWNLOAD_MOLT_RESUMED_FETCH_POLICY_H_
