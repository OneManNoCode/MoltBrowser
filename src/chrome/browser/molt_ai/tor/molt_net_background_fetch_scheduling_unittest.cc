// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's Background Fetch scheduled per site
// (content/browser/background_fetch/molt_background_fetch_site_policy.h)
// and the in-memory download service's limits that match it
// (components/download/internal/background_service/
// molt_in_memory_download_limits.h), both std-only.

#include "content/browser/background_fetch/molt_background_fetch_site_policy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "components/download/internal/background_service/molt_in_memory_download_limits.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::tor {
namespace {

using content::kMoltBackgroundFetchActivePerSite;
using content::kMoltBackgroundFetchRunningPerSite;
using content::kMoltBackgroundFetchRunningTotal;
using content::MoltBackgroundFetchSites;

std::string Site(int i) {
  return "https://site" + std::to_string(i) + ".test";
}

// Each site has Chromium's limits for a whole partition; the total is 64
// sites' whole shares.
TEST(MoltNetBackgroundFetchSchedulingTest, Limits) {
  EXPECT_EQ(kMoltBackgroundFetchActivePerSite, 2);
  EXPECT_EQ(kMoltBackgroundFetchRunningPerSite, 2);
  EXPECT_EQ(kMoltBackgroundFetchRunningTotal, 128);
}

// A site's registrations wait only for its own: two active, the third
// waits; another site's become active whatever the first runs.
TEST(MoltNetBackgroundFetchSchedulingTest, ActivePerSite) {
  MoltBackgroundFetchSites sites;
  const std::string greedy = "https://greedy.test";
  const std::string polite = "https://polite.test";
  ASSERT_TRUE(sites.MayActivate(greedy));
  sites.Activated(greedy);
  sites.Activated(greedy);
  EXPECT_FALSE(sites.MayActivate(greedy));
  EXPECT_TRUE(sites.MayActivate(polite));
  sites.Deactivated(greedy);
  EXPECT_TRUE(sites.MayActivate(greedy));
}

// Chromium 156 runs two downloads for all sites: a greedy site's two slow
// ones keep a polite site's from starting. Per site, the polite site's
// first request starts at once, whatever the greedy site runs.
TEST(MoltNetBackgroundFetchSchedulingTest, AGreedySiteDoesntDelayAnother) {
  MoltBackgroundFetchSites sites;
  const std::string greedy = "https://greedy.test";
  const std::string polite = "https://polite.test";
  sites.Activated(greedy);
  sites.Activated(greedy);
  std::optional<std::string> next = sites.PickNext({greedy, greedy});
  ASSERT_EQ(next, greedy);
  sites.Started(greedy);
  sites.Started(*sites.PickNext({greedy, greedy}));
  // The greedy site's share is full; its downloads never finish.
  EXPECT_FALSE(sites.PickNext({greedy, greedy}).has_value());
  sites.Activated(polite);
  EXPECT_EQ(sites.PickNext({greedy, greedy, polite}), polite);
  sites.Started(polite);
  EXPECT_EQ(sites.PickNext({greedy, greedy, polite}), polite);
  sites.Started(polite);
  EXPECT_EQ(sites.running(polite), 2);
  EXPECT_EQ(sites.running(greedy), 2);
}

// A group of sites can't fill the total so that another waits until 64
// sites download their whole shares at once; then the freed slot goes to
// the waiting site downloading the fewest, not to one of the group.
TEST(MoltNetBackgroundFetchSchedulingTest, TheTotalIsShared) {
  MoltBackgroundFetchSites sites;
  for (int i = 0; i < 63; ++i) {
    sites.Started(Site(i), 2);
  }
  const std::string polite = "https://polite.test";
  EXPECT_TRUE(sites.MayStart(polite));
  sites.Started(polite, 2);
  EXPECT_EQ(sites.total_running(), kMoltBackgroundFetchRunningTotal);
  const std::string late = "https://late.test";
  EXPECT_FALSE(sites.MayStart(late));
  EXPECT_FALSE(sites.PickNext({late, Site(0)}).has_value());

  // Site 0 finishes one; it and the late site both have one waiting: the
  // late site, downloading none, goes first.
  sites.Finished(Site(0));
  EXPECT_EQ(sites.PickNext({Site(0), late}), late);
  sites.Started(late);
  EXPECT_FALSE(sites.PickNext({Site(0), late}).has_value());
}

// Among sites downloading as many, the one served least recently goes first:
// a round robin.
TEST(MoltNetBackgroundFetchSchedulingTest, RoundRobinAmongEquals) {
  MoltBackgroundFetchSites sites;
  const std::string a = "https://a.test";
  const std::string b = "https://b.test";
  const std::string c = "https://c.test";
  sites.Started(a);
  sites.Started(b);
  sites.Started(c);
  sites.Finished(a);
  sites.Finished(b);
  sites.Finished(c);
  // All three dropped (nothing active or running): never served again.
  EXPECT_EQ(sites.site_count(), 0u);
  sites.Activated(a);
  sites.Activated(b);
  sites.Activated(c);
  EXPECT_EQ(sites.PickNext({a, b, c}), a);
  sites.Started(a);
  EXPECT_EQ(sites.PickNext({a, b, c}), b);
  sites.Started(b);
  EXPECT_EQ(sites.PickNext({c, b, a}), c);
  sites.Started(c);
  sites.Finished(a);
  sites.Finished(b);
  sites.Finished(c);
  // Each at none: a was served longest ago.
  EXPECT_EQ(sites.PickNext({c, b, a}), a);
}

// Counts never go below zero, and a site with nothing left isn't kept.
TEST(MoltNetBackgroundFetchSchedulingTest, CountsAreClamped) {
  MoltBackgroundFetchSites sites;
  const std::string a = "https://a.test";
  sites.Started(a, 2);
  sites.Finished(a, 5);
  EXPECT_EQ(sites.running(a), 0);
  EXPECT_EQ(sites.total_running(), 0);
  sites.Deactivated(a);
  EXPECT_EQ(sites.active(a), 0);
  EXPECT_EQ(sites.site_count(), 0u);
  sites.Started(a, 0);
  sites.Finished(a, -1);
  EXPECT_EQ(sites.site_count(), 0u);
}

// The in-memory download service never queues what Background Fetch
// starts: its limits are twice the scheduler's total, never below a field
// trial's.
TEST(MoltNetBackgroundFetchSchedulingTest, DownloadServiceLimitsMatch) {
  static_assert(download::kMoltInMemoryDownloadsLimit >=
                2 * kMoltBackgroundFetchRunningTotal);
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(2),
            download::kMoltInMemoryDownloadsLimit);
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(15),
            download::kMoltInMemoryDownloadsLimit);
  EXPECT_EQ(download::MoltInMemoryDownloadLimit(1000), 1000u);
}

}  // namespace
}  // namespace molt_ai::tor
