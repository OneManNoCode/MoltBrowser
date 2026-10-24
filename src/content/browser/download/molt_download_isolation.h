// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: gives the download requests the browser makes for a page the
// network isolation molt_download_isolation_policy.h chooses, in profiles
// that keep downloads with their site
// (ContentBrowserClient::ShouldKeepDownloadsWithTheirSite(): MoltNet's).
// Called from DownloadManagerImpl::BeginDownloadInternal() (new and resumed
// downloads) and SaveFileManager::SaveURL() (Save Page As). Other profiles
// are left as Chromium makes them.
//
// A request whose isolation is changed also keeps its first party on
// redirects (update_first_party_url_on_redirect off), so a redirect doesn't
// move it to the new destination's partition or make it first-party there.

#ifndef CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_H_
#define CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_H_

#include <optional>

#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/browser/download/molt_download_isolation_policy.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/content_client.h"
#include "net/base/isolation_info.h"
#include "net/base/schemeful_site.h"
#include "services/network/public/cpp/resource_request.h"

namespace content {

inline bool MoltKeepsDownloadsWithTheirSite(BrowserContext* browser_context) {
  ContentClient* content_client = GetContentClient();
  ContentBrowserClient* browser_client =
      content_client ? content_client->browser() : nullptr;
  return browser_context && browser_client &&
         browser_client->ShouldKeepDownloadsWithTheirSite(browser_context);
}

// Whether `a` and `b` have the same top-level site and nonce: the same
// network partition, connections and Tor circuit.
inline bool MoltOnSameTopLevelSite(const net::IsolationInfo& a,
                                   const net::IsolationInfo& b) {
  return a.top_frame_origin().has_value() &&
         b.top_frame_origin().has_value() &&
         net::SchemefulSite(*a.top_frame_origin()) ==
             net::SchemefulSite(*b.top_frame_origin()) &&
         a.nonce() == b.nonce();
}

// The isolation for `choice` (not kAsChromium).
inline net::IsolationInfo MoltDownloadIsolationInfo(
    MoltDownloadIsolation choice,
    RenderFrameHost* frame,
    const std::optional<net::IsolationInfo>& original) {
  switch (choice) {
    case MoltDownloadIsolation::kOriginal:
      return *original;
    case MoltDownloadIsolation::kFrame:
      return frame->GetIsolationInfoForSubresources();
    case MoltDownloadIsolation::kAsChromium:
    case MoltDownloadIsolation::kTransient:
      break;
  }
  return net::IsolationInfo::CreateTransient(std::nullopt);
}

inline MoltDownloadIsolation MoltChooseFor(
    BrowserContext* browser_context,
    const std::optional<net::IsolationInfo>& isolation,
    const std::optional<net::IsolationInfo>& original,
    RenderFrameHost* frame) {
  const bool has_isolation = isolation.has_value() && !isolation->IsEmpty();
  return MoltChooseDownloadIsolation(
      MoltKeepsDownloadsWithTheirSite(browser_context), has_isolation,
      has_isolation && frame &&
          MoltOnSameTopLevelSite(*isolation,
                                 frame->GetIsolationInfoForSubresources()),
      original.has_value() && !original->IsEmpty(), frame != nullptr);
}

// DownloadManagerImpl::BeginDownloadInternal(): `frame` is the frame that
// started the download (or null), `resumed` the download it resumes (or
// null).
inline void MoltKeepDownloadWithItsSite(BrowserContext* browser_context,
                                        RenderFrameHost* frame,
                                        const download::DownloadItem* resumed,
                                        download::DownloadUrlParameters& params) {
  const std::optional<net::IsolationInfo> original =
      resumed ? resumed->GetIsolationInfo() : std::nullopt;
  const MoltDownloadIsolation choice =
      MoltChooseFor(browser_context, params.isolation_info(), original, frame);
  if (choice == MoltDownloadIsolation::kAsChromium) {
    return;
  }
  params.set_isolation_info(MoltDownloadIsolationInfo(choice, frame, original));
  params.set_update_first_party_url_on_redirect(false);
}

// SaveFileManager::SaveURL(): `request` fetches a resource of the page being
// saved for the frame `frame` (or null).
inline void MoltKeepSavedResourceWithItsFrame(
    BrowserContext* browser_context,
    RenderFrameHost* frame,
    network::ResourceRequest& request) {
  std::optional<net::IsolationInfo> isolation;
  if (request.trusted_params) {
    isolation = request.trusted_params->isolation_info;
  }
  const MoltDownloadIsolation choice =
      MoltChooseFor(browser_context, isolation, std::nullopt, frame);
  if (choice == MoltDownloadIsolation::kAsChromium) {
    return;
  }
  const net::IsolationInfo isolation_info =
      MoltDownloadIsolationInfo(choice, frame, std::nullopt);
  if (!request.trusted_params) {
    request.trusted_params.emplace();
  }
  request.trusted_params->isolation_info = isolation_info;
  request.site_for_cookies = isolation_info.site_for_cookies();
  request.update_first_party_url_on_redirect = false;
}

}  // namespace content

#endif  // CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_H_
