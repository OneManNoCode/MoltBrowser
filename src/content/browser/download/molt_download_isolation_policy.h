// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: which network isolation a download the browser makes for a
// page carries, in a profile that keeps downloads with their site
// (ContentBrowserClient::ShouldKeepDownloadsWithTheirSite(): MoltNet's).
// Std-only, so molt_ai_unittests tests it standalone;
// molt_download_isolation.h applies it.
//
// Chromium 156 files some download requests under their destination, as if
// they were top-level navigations there:
//   * downloads a page starts that aren't navigations: a link with
//     `download`, "Save link as", a link dragged out of the page
//     (RenderFrameHost::CreateDownloadUrlParameters() sets no isolation, and
//     download::CreateResourceRequest() then makes one from the URL);
//   * "Save image as", "Save video as" in a top-level page
//     (WebContentsImpl::SaveFrameWithHeaders() gives the request the
//     isolation of a navigation of the frame to the image: for the
//     top-level frame, the image's site as top-level site);
//   * a resumed download (DownloadItemImpl::ResumeInterruptedDownload() sets
//     no isolation, whatever the first request had);
//   * the resources Save Page As fetches for a complete page (an empty
//     isolation, crbug.com/41485695, so the browser-process factory makes
//     one from the URL).
// Such a request went to the destination's partition: its HTTP cache, its
// connections and, through Tor, its circuit as a top-level site, with its
// cookies as a first party. A frame of one site embedded on two others
// could so link them through a download. In a profile that keeps downloads
// with their site, the request gets instead:
//   * when it resumes a download: the isolation that download's first
//     request had;
//   * when a frame started it: that frame's isolation for its subresources,
//     the same as the frame's own requests (its top-level site's partition,
//     connections and circuit), unless the request's own isolation already
//     has that top-level site (saving the page or a frame itself);
//   * otherwise (no frame left, nothing to resume): its own isolation if it
//     has one, else a one-off isolation shared with nothing.
// Navigations that turn into downloads keep their navigation's isolation:
// they don't come through here.

#ifndef CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_POLICY_H_
#define CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_POLICY_H_

namespace content {

enum class MoltDownloadIsolation {
  kAsChromium,  // Leave the request as Chromium makes it.
  kOriginal,    // The isolation of the download it resumes.
  kFrame,       // The subresource isolation of the frame that started it.
  kTransient,   // A one-off isolation of its own.
};

// `has_isolation`: the request has an isolation of its own;
// `isolation_on_frames_site`: that isolation has the top-level site (and
// nonce) of the frame's; `has_original`: it resumes a download whose first
// request had an isolation; `has_frame`: the frame that started it is still
// there.
constexpr MoltDownloadIsolation MoltChooseDownloadIsolation(
    bool keep_with_site,
    bool has_isolation,
    bool isolation_on_frames_site,
    bool has_original,
    bool has_frame) {
  if (!keep_with_site) {
    return MoltDownloadIsolation::kAsChromium;
  }
  if (!has_isolation && has_original) {
    return MoltDownloadIsolation::kOriginal;
  }
  if (has_frame) {
    return has_isolation && isolation_on_frames_site
               ? MoltDownloadIsolation::kAsChromium
               : MoltDownloadIsolation::kFrame;
  }
  return has_isolation ? MoltDownloadIsolation::kAsChromium
                       : MoltDownloadIsolation::kTransient;
}

}  // namespace content

#endif  // CONTENT_BROWSER_DOWNLOAD_MOLT_DOWNLOAD_ISOLATION_POLICY_H_
