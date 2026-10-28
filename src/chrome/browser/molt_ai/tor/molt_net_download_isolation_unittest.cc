// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Which network isolation a download the browser makes for a MoltNet page
// carries (content/browser/download/molt_download_isolation_policy.h,
// std-only).

#include "content/browser/download/molt_download_isolation_policy.h"
#include "content/browser/download/molt_resumed_fetch_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace content {
namespace {

using enum MoltDownloadIsolation;

constexpr MoltDownloadIsolation Choose(bool keep,
                                       bool has_isolation,
                                       bool on_frames_site,
                                       bool has_original,
                                       bool has_frame,
                                       bool is_retry = false) {
  return MoltChooseDownloadIsolation(keep, has_isolation, on_frames_site,
                                     has_original, has_frame, is_retry);
}

// Other profiles: every request as Chromium makes it.
TEST(MoltNetDownloadIsolationTest, OtherProfilesAsChromium) {
  for (int bits = 0; bits < 32; ++bits) {
    EXPECT_EQ(Choose(false, (bits & 1) != 0, (bits & 2) != 0,
                     (bits & 4) != 0, (bits & 8) != 0, (bits & 16) != 0),
              kAsChromium);
  }
}

// A link with `download`, "Save link as", a dragged link: no isolation of
// their own, so the frame's.
TEST(MoltNetDownloadIsolationTest, APageDownloadGetsItsFramesIsolation) {
  EXPECT_EQ(Choose(true, /*has_isolation=*/false, false, false,
                   /*has_frame=*/true),
            kFrame);
}

// "Save image as" in a top-level page: the request's isolation is the
// image's site as top-level site, so the frame's replaces it; saving the
// page or a frame itself keeps its own (it already has the frame's
// top-level site).
TEST(MoltNetDownloadIsolationTest, AnotherTopLevelSiteIsReplaced) {
  EXPECT_EQ(Choose(true, /*has_isolation=*/true, /*on_frames_site=*/false,
                   false, /*has_frame=*/true),
            kFrame);
  EXPECT_EQ(Choose(true, /*has_isolation=*/true, /*on_frames_site=*/true,
                   false, /*has_frame=*/true),
            kAsChromium);
}

// A resumed download: its first request's isolation, also when a frame
// would be there.
TEST(MoltNetDownloadIsolationTest, AResumedDownloadKeepsItsFirstIsolation) {
  EXPECT_EQ(Choose(true, false, false, /*has_original=*/true, false),
            kOriginal);
  EXPECT_EQ(Choose(true, false, false, /*has_original=*/true, true),
            kOriginal);
}

// Nothing to take an isolation from: a one-off one, never the destination's.
TEST(MoltNetDownloadIsolationTest, NothingToGoByMeansOneOff) {
  EXPECT_EQ(Choose(true, false, false, false, false), kTransient);
  // A request with an isolation and no frame to check it against keeps it.
  EXPECT_EQ(Choose(true, true, false, false, false), kAsChromium);
  EXPECT_EQ(Choose(true, true, false, true, false), kAsChromium);
}

// A retry from the download bubble (no frame) or the downloads page (that
// page's frame) carries the isolation of the download it retries and keeps
// it; the downloads page is never taken for the frame that started it.
TEST(MoltNetDownloadIsolationTest, ARetryKeepsTheIsolationItCarries) {
  for (bool on_frames_site : {false, true}) {
    for (bool has_frame : {false, true}) {
      EXPECT_EQ(Choose(true, /*has_isolation=*/true, on_frames_site, false,
                       has_frame, /*is_retry=*/true),
                kAsChromium);
    }
  }
}

// A retry of a download whose first request had no isolation: a one-off
// one, neither the destination's nor the downloads page's (which every
// retry from that page would share).
TEST(MoltNetDownloadIsolationTest, ARetryWithoutIsolationIsOneOff) {
  for (bool has_frame : {false, true}) {
    EXPECT_EQ(Choose(true, /*has_isolation=*/false, false, false, has_frame,
                     /*is_retry=*/true),
              kTransient);
  }
}

// In every profile, a page's Background Fetch request that comes without
// its service worker's URL loader factory (one resumed by the download item,
// or after a restart) gets one that checks Local Network Access; one with
// the worker's factory keeps it, and nothing else changes: other downloads
// (no initiator, or not the download service's), or Background Fetch
// without the feature.
TEST(MoltNetDownloadIsolationTest, AResumedFetchKeepsItsLnaCheck) {
  EXPECT_TRUE(MoltFetchNeedsLnaFactory(true, true, false, true));
  EXPECT_FALSE(MoltFetchNeedsLnaFactory(true, true, /*has_factory=*/true,
                                        true));
  EXPECT_FALSE(MoltFetchNeedsLnaFactory(true, /*has_initiator=*/false, false,
                                        true));
  EXPECT_FALSE(MoltFetchNeedsLnaFactory(/*from_download_service=*/false, true,
                                        false, true));
  EXPECT_FALSE(MoltFetchNeedsLnaFactory(true, true, false,
                                        /*lna_checked=*/false));
}

}  // namespace
}  // namespace content
