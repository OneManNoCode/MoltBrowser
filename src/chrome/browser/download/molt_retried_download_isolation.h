// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a download the user retries from the download bubble or the
// downloads page keeps the network isolation of the download it retries, in
// a profile that keeps downloads with their site (MoltNet's:
// molt_ai::tor::KeepDownloadsWithTheirSite(), as
// ChromeContentBrowserClient::ShouldKeepDownloadsWithTheirSite() answers).
// Called from DownloadBubbleUIController::RetryDownload() and
// DownloadsDOMHandler::RetryDownload(). Other profiles are left as Chromium
// makes them.
//
// Chromium 156 retries a download as a new download of its URL: from the
// bubble with no isolation and no frame, so download::CreateResourceRequest()
// would make one from the URL (the destination as top-level site); from the
// downloads page with that page's frame and no isolation. With the
// isolation of the download it retries, the request goes to the same
// partition, connections and Tor circuit as the first one did, those of the
// site it came from, and with the same initiator.
// DownloadManagerImpl::BeginDownloadInternal() then leaves a retry with an
// isolation as it is: a retry never takes the downloads page for the frame
// that started it (content/browser/download/molt_download_isolation_policy.h),
// and one whose first request had no isolation gets a one-off one.
//
// Header-only and in no GN target's `sources` (like
// content/browser/download/molt_download_isolation.h), so the two upstream
// files need no new dependency; what it includes is header-only too.

#ifndef CHROME_BROWSER_DOWNLOAD_MOLT_RETRIED_DOWNLOAD_ISOLATION_H_
#define CHROME_BROWSER_DOWNLOAD_MOLT_RETRIED_DOWNLOAD_ISOLATION_H_

#include <optional>

#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/browser_context.h"
#include "net/base/isolation_info.h"

// `browser_context`: the profile the retry is made in; `original`: the
// download it retries (null if it isn't one, e.g. an offline item).
inline void MoltRetryDownloadWithItsSite(
    content::BrowserContext* browser_context,
    const download::DownloadItem* original,
    download::DownloadUrlParameters& params) {
  if (!original || !browser_context ||
      !molt_ai::tor::KeepDownloadsWithTheirSite(molt_ai::tor::IsMoltNetProfile(
          Profile::FromBrowserContext(browser_context)))) {
    return;
  }
  params.set_initiator(original->GetRequestInitiator());
  const std::optional<net::IsolationInfo>& isolation =
      original->GetIsolationInfo();
  if (!isolation || isolation->IsEmpty()) {
    return;
  }
  params.set_isolation_info(*isolation);
  params.set_update_first_party_url_on_redirect(false);
}

#endif  // CHROME_BROWSER_DOWNLOAD_MOLT_RETRIED_DOWNLOAD_ISOLATION_H_
