// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage / sessionStorage caches, totals and commit rates
// per site
// (components/services/storage/dom_storage/molt_dom_storage_site_policy.h,
// std-only).

#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace storage {
namespace {

constexpr size_t kMiB = 1024 * 1024;

// A site's unused areas are purged when its own areas pass either per-site
// limit, and only if it has unused areas.
TEST(MoltNetDomStorageSitePolicyTest, PurgesBySiteLimits) {
  EXPECT_FALSE(MoltDomStoragePurgesSite(0, 0, 0));
  EXPECT_FALSE(MoltDomStoragePurgesSite(kMoltDomStorageAreasPerSite, 3,
                                        kMoltDomStorageCachePerSiteBytes));
  EXPECT_TRUE(MoltDomStoragePurgesSite(kMoltDomStorageAreasPerSite + 1, 1, 0));
  EXPECT_TRUE(MoltDomStoragePurgesSite(1, 1,
                                       kMoltDomStorageCachePerSiteBytes + 1));
  // Nothing unused: nothing to purge, however full.
  EXPECT_FALSE(MoltDomStoragePurgesSite(100, 0, 100 * kMiB));
}

// A quarter of Chromium's desktop limits for a whole partition.
TEST(MoltNetDomStorageSitePolicyTest, Limits) {
  EXPECT_EQ(kMoltDomStorageAreasPerSite * 4, 48u);
  EXPECT_EQ(kMoltDomStorageCachePerSiteBytes * 4, 20 * kMiB);
}

// Unused areas leave the cache after 30 minutes unused, checked every
// minute.
TEST(MoltNetDomStorageSitePolicyTest, IdleExpiry) {
  EXPECT_FALSE(MoltDomStorageAreaExpired(0));
  EXPECT_FALSE(MoltDomStorageAreaExpired(kMoltDomStorageIdleExpirySeconds - 1));
  EXPECT_TRUE(MoltDomStorageAreaExpired(kMoltDomStorageIdleExpirySeconds));
  EXPECT_EQ(kMoltDomStorageIdleExpirySeconds, 1800);
  EXPECT_EQ(kMoltDomStorageSweepIntervalSeconds, 60);
}

// A site's areas together hold at most one area's own limit: a site of one
// origin stores what Chromium lets it; its subdomains share it; another site
// keeps all of its own whatever the first stores.
TEST(MoltNetDomStorageSitePolicyTest, OneTotalPerSite) {
  constexpr size_t kShare = kMoltDomStorageBytesPerSite;
  EXPECT_EQ(kShare, 10 * kMiB + 100 * 1024);
  MoltDomStorageSiteTotals totals;
  EXPECT_TRUE(totals.MayGrowTo("https://a.test", "https://a.test", kShare));
  EXPECT_FALSE(totals.MayGrowTo("https://a.test", "https://a.test", kShare + 1));
  // A subdomain of the full site gets nothing more.
  EXPECT_FALSE(totals.MayGrowTo("https://x.a.test", "https://a.test", 1));
  EXPECT_EQ(totals.SiteBytes("https://a.test"), kShare);
  // Another site, all of its own.
  EXPECT_TRUE(totals.MayGrowTo("https://b.test", "https://b.test", kShare));
  EXPECT_EQ(totals.total(), 2 * kShare);
  // Shrinking makes room for the site's other areas.
  totals.ShrankTo("https://a.test", "https://a.test", kShare - 10);
  EXPECT_TRUE(totals.MayGrowTo("https://x.a.test", "https://a.test", 10));
  EXPECT_FALSE(totals.MayGrowTo("https://x.a.test", "https://a.test", 11));
  // Staying as large is no growth.
  EXPECT_TRUE(totals.MayGrowTo("https://x.a.test", "https://a.test", 10));
  totals.ShrankTo("https://x.a.test", "https://a.test", 0);
  EXPECT_EQ(totals.SiteBytes("https://a.test"), kShare - 10);
  // Forgetting an area (its data deleted) gives its bytes back.
  totals.Forget("https://a.test");
  EXPECT_EQ(totals.SiteBytes("https://a.test"), 0u);
  EXPECT_EQ(totals.total(), kShare);
  // Refused areas leave nothing behind.
  MoltDomStorageSiteTotals other;
  EXPECT_FALSE(other.MayGrowTo("m1", "https://c.test", kShare + 1));
  EXPECT_EQ(other.total(), 0u);
  EXPECT_EQ(other.SiteBytes("https://c.test"), 0u);
}

// A sessionStorage map copied for a tab counts as it is, made with what it
// copies, even past the site's share: the site then grows no more until it
// shrinks. Shrinking never counts what wasn't there.
TEST(MoltNetDomStorageSitePolicyTest, ACopyCountsAsItIs) {
  constexpr size_t kShare = kMoltDomStorageBytesPerSite;
  MoltDomStorageSiteTotals totals;
  ASSERT_TRUE(totals.MayGrowTo("1", "https://a.test", 6 * kMiB));
  totals.ShrankTo("2", "https://a.test", 6 * kMiB);
  EXPECT_EQ(totals.SiteBytes("https://a.test"), 12 * kMiB);
  EXPECT_EQ(totals.total(), 12 * kMiB);
  EXPECT_GT(totals.SiteBytes("https://a.test"), kShare);
  EXPECT_FALSE(totals.MayGrowTo("3", "https://a.test", 1));
  EXPECT_FALSE(totals.MayGrowTo("2", "https://a.test", 6 * kMiB + 1));
  // Staying as large, or shrinking, is still allowed.
  EXPECT_TRUE(totals.MayGrowTo("2", "https://a.test", 6 * kMiB));
  totals.ShrankTo("2", "https://a.test", 4 * kMiB);
  EXPECT_EQ(totals.SiteBytes("https://a.test"), 10 * kMiB);
  EXPECT_TRUE(totals.MayGrowTo("3", "https://a.test", 100 * 1024));
  // A counted area's site stays the one it was counted for.
  totals.ShrankTo("2", "https://b.test", 3 * kMiB);
  EXPECT_EQ(totals.SiteBytes("https://a.test"), 9 * kMiB + 100 * 1024);
  EXPECT_EQ(totals.SiteBytes("https://b.test"), 0u);
  // An empty copy isn't counted.
  totals.ShrankTo("4", "https://c.test", 0);
  EXPECT_EQ(totals.SiteBytes("https://c.test"), 0u);
  totals.Forget("2");
  totals.Forget("1");
  totals.Forget("3");
  EXPECT_EQ(totals.total(), 0u);
}

// Many subdomains together still hold one share.
TEST(MoltNetDomStorageSitePolicyTest, SubdomainsShareTheSitesTotal) {
  MoltDomStorageSiteTotals totals;
  size_t stored = 0;
  for (int i = 0; i < 1000; ++i) {
    const std::string area = "https://s" + std::to_string(i) + ".a.test";
    if (totals.MayGrowTo(area, "https://a.test", kMiB)) {
      stored += kMiB;
    }
  }
  EXPECT_EQ(stored, 10 * kMiB);
  EXPECT_LE(totals.SiteBytes("https://a.test"), kMoltDomStorageBytesPerSite);
}

// The valve hears past the floor, once per MiB of growth.
TEST(MoltNetDomStorageSitePolicyTest, GrowthPastTheFloor) {
  constexpr size_t kFloor = kMoltDomStorageFloorBytes;
  EXPECT_EQ(kFloor, 5 * kMoltDomStorageBytesPerSite);
  EXPECT_FALSE(MoltDomStorageGrowthNotifies(0, kFloor));
  EXPECT_TRUE(MoltDomStorageGrowthNotifies(kFloor, kFloor + 1));
  EXPECT_FALSE(MoltDomStorageGrowthNotifies(kFloor + 1, kFloor + 2));
  EXPECT_FALSE(MoltDomStorageGrowthNotifies(kFloor + 2, kFloor + 1));
  const size_t next_step = (kFloor / kMiB + 1) * kMiB;
  EXPECT_TRUE(MoltDomStorageGrowthNotifies(next_step - 1, next_step));
  EXPECT_TRUE(MoltDomStorageGrowthNotifies(0, 10 * kFloor));
}

// A site's areas together write to the database at one area's rate.
TEST(MoltNetDomStorageSitePolicyTest, OneCommitRatePerSite) {
  constexpr int64_t kHour = kMoltDomStorageHourMs;
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(0, 0, 0), 0);
  // 60 an hour: the 61st waits for the hour.
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(60, 0, 0), kHour);
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(60, 0, kHour / 2), kHour / 2);
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(60, 0, kHour), 0);
  // 10 MiB an hour.
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(1, 10 * kMiB, 0), kHour);
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(1, 5 * kMiB, 0), kHour / 2);
  // No overflow for a long-lived, busy site.
  // (2^40 bytes at 10 MiB an hour: 104857.6 hours.)
  EXPECT_EQ(MoltDomStorageSiteCommitDelayMs(0, int64_t{1} << 40, 0),
            104857 * kHour + kHour * 6 / 10);
}

}  // namespace
}  // namespace storage
