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
// without them.
//
// The parameters don't come back while the fetch's registration isn't loaded
// yet in the new session: Background Fetch knows a download only once its
// registration loads and it takes the download up again
// (BackgroundFetchDelegateBase::GetUploadData() answers with nothing until
// DownloadUrl()). At Chromium 156 such a download was resumed, or started,
// at once without them: no initiator, no isolation, and, with Local Network
// Access for Background Fetch, no factory of the service worker's, so the
// request went out on the partition's default factory, which checks no
// Local Network Access at all. Here a page's fetch whose parameters don't
// come back waits for them (MoltWaitsForFetchParameters()): when Background
// Fetch takes it up (StartDownload() with its GUID, refused as one the
// service has already), it asks for them again, and goes on with them as
// above; one still waiting after kMoltFetchParametersWaitSeconds (its
// registration gone meanwhile) fails, which Background Fetch ignores for a
// download it doesn't know. Every other client's download goes on as in
// Chromium.
//
// Background Fetch may take the download up while the download service is
// still asking for its parameters (ControllerImpl's pending uploads: the
// answer is posted, and Background Fetch's own answer may be posted too).
// That answer can be from before Background Fetch knew the download, so it
// names no initiator; and while it is on its way, asking again does nothing
// (ControllerImpl::UpdateDriverState() returns while an upload is pending).
// So a take-up that comes then is remembered (MoltFetchTakeUps), and when
// the answer comes without the parameters, the download asks again at once
// rather than waiting kMoltFetchParametersWaitSeconds and failing
// (MoltOnFetchTakenUp(), ControllerImpl::OnDownloadReadyToStart()).
//
// Header-only and templated on the request parameters (RequestParams), so
// the download service's target needs no new dependency and
// molt_ai_unittests tests it.

#ifndef COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_RESTORED_FETCH_H_
#define COMPONENTS_DOWNLOAD_INTERNAL_BACKGROUND_SERVICE_MOLT_RESTORED_FETCH_H_

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace download {

// Whether a download whose request parameters name an initiator
// (`has_initiator`: a page's fetch) and whose item exists already
// (`has_driver_entry`: kept from an earlier session) is started again
// rather than resumed.
constexpr bool MoltRestartsRestoredFetch(bool has_initiator,
                                         bool has_driver_entry) {
  return has_initiator && has_driver_entry;
}

// Whether a download of `page_fetch_client` (Background Fetch's: every one
// is a page's fetch) whose request parameters name no initiator once its
// client's are back (`has_initiator`) waits for them rather than going on.
constexpr bool MoltWaitsForFetchParameters(bool page_fetch_client,
                                           bool has_initiator) {
  return page_fetch_client && !has_initiator;
}

// How long such a download waits before it fails. A registration loads
// within seconds of the browser's start; this is far longer.
inline constexpr int64_t kMoltFetchParametersWaitSeconds = 5 * 60;

// What ControllerImpl::StartDownload() does when the client takes up a
// download the service has already (refused as UNEXPECTED_GUID).
enum class MoltFetchTakeUp {
  // Nothing: not a page's fetch waiting for its parameters.
  kNothing,
  // Ask for its parameters again now.
  kAskNow,
  // An answer is on its way: ask again when it comes, if it has none.
  kAskWhenAnswered,
};

// For a download that `waits_for_parameters` (MoltWaitsForFetchParameters(),
// its parameters not had yet) while its parameters are being asked for
// (`upload_pending`) or not.
constexpr MoltFetchTakeUp MoltOnFetchTakenUp(bool waits_for_parameters,
                                             bool upload_pending) {
  if (!waits_for_parameters) {
    return MoltFetchTakeUp::kNothing;
  }
  return upload_pending ? MoltFetchTakeUp::kAskWhenAnswered
                        : MoltFetchTakeUp::kAskNow;
}

// The downloads (by GUID) the client took up while their parameters were
// being asked for (MoltFetchTakeUp::kAskWhenAnswered). An entry lives until
// the answer comes (Take()) or the download ends (Forget()).
class MoltFetchTakeUps {
 public:
  MoltFetchTakeUps();
  MoltFetchTakeUps(const MoltFetchTakeUps&) = delete;
  MoltFetchTakeUps& operator=(const MoltFetchTakeUps&) = delete;
  ~MoltFetchTakeUps();

  void Remember(const std::string& guid) { guids_.insert(guid); }
  // Whether `guid` was taken up while its answer was on its way; forgets it.
  bool Take(const std::string& guid) { return guids_.erase(guid) > 0; }
  void Forget(const std::string& guid) { guids_.erase(guid); }
  size_t size() const { return guids_.size(); }

 private:
  std::set<std::string> guids_;
};

// Out of the class body, as Chromium's style asks of a class with members
// that have constructors.
inline MoltFetchTakeUps::MoltFetchTakeUps() = default;
inline MoltFetchTakeUps::~MoltFetchTakeUps() = default;

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
