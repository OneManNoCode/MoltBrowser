// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage / sessionStorage caches, totals, commit and write
// rates per site, and its partitions without a database
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

// An area a page cleared, forgotten (its origin's data deleted, for
// example by a Clear-Site-Data header) after its site's other areas were:
// the site is left with nothing, and nothing else changes.
TEST(MoltNetDomStorageSitePolicyTest, ForgettingAClearedAreaLast) {
  MoltDomStorageSiteTotals totals;
  ASSERT_TRUE(totals.MayGrowTo("https://a.s.test/", "https://s.test", 100));
  totals.ShrankTo("https://a.s.test/", "https://s.test", 0);
  ASSERT_TRUE(totals.MayGrowTo("https://b.s.test/", "https://s.test", 50));
  ASSERT_TRUE(totals.MayGrowTo("https://o.test/", "https://o.test", 7));
  totals.Forget("https://b.s.test/");
  totals.Forget("https://a.s.test/");
  EXPECT_EQ(totals.SiteBytes("https://s.test"), 0u);
  EXPECT_EQ(totals.SiteBytes("https://o.test"), 7u);
  EXPECT_EQ(totals.total(), 7u);
  // The site has its whole share again.
  EXPECT_TRUE(totals.MayGrowTo("https://a.s.test/", "https://s.test",
                               kMoltDomStorageBytesPerSite));
  EXPECT_EQ(totals.total(), 7u + kMoltDomStorageBytesPerSite);
}

// An area counts to its top-level site, but a key with a nonce (a
// credentialless iframe's, whose top-level site is its own origin's site
// wherever it is embedded) to its nonce, and a key with an opaque top-level
// site (which serializes as "null" for all of them) to itself; no such
// group's name is ever a site's.
TEST(MoltNetDomStorageSitePolicyTest, GroupsOfKeysThatDontNameTheirSite) {
  EXPECT_EQ(MoltDomStorageGroupKindFor(false, false),
            MoltDomStorageGroupKind::kTopLevelSite);
  EXPECT_EQ(MoltDomStorageGroupKindFor(true, false),
            MoltDomStorageGroupKind::kNonce);
  EXPECT_EQ(MoltDomStorageGroupKindFor(true, true),
            MoltDomStorageGroupKind::kNonce);
  EXPECT_EQ(MoltDomStorageGroupKindFor(false, true),
            MoltDomStorageGroupKind::kOwnKey);
  EXPECT_EQ(MoltDomStorageGroupName(MoltDomStorageGroupKind::kTopLevelSite,
                                    "https://t.test"),
            "https://t.test");
  const std::string nonce =
      MoltDomStorageGroupName(MoltDomStorageGroupKind::kNonce, "https://t.test");
  const std::string own =
      MoltDomStorageGroupName(MoltDomStorageGroupKind::kOwnKey, "https://t.test");
  EXPECT_NE(nonce, "https://t.test");
  EXPECT_NE(own, "https://t.test");
  EXPECT_NE(nonce, own);
  // A tracker's frames under two pages' nonces have totals of their own,
  // and neither touches its first-party site's.
  MoltDomStorageSiteTotals totals;
  const std::string site = "https://t.test";
  const std::string under_a =
      MoltDomStorageGroupName(MoltDomStorageGroupKind::kNonce, "A");
  const std::string under_b =
      MoltDomStorageGroupName(MoltDomStorageGroupKind::kNonce, "B");
  ASSERT_TRUE(totals.MayGrowTo("t/", site, kMoltDomStorageBytesPerSite));
  EXPECT_TRUE(totals.MayGrowTo("t/^1A", under_a, kMoltDomStorageBytesPerSite));
  EXPECT_TRUE(totals.MayGrowTo("t/^1B", under_b, kMoltDomStorageBytesPerSite));
  EXPECT_EQ(totals.SiteBytes(site), kMoltDomStorageBytesPerSite);
  EXPECT_EQ(totals.SiteBytes(under_a), kMoltDomStorageBytesPerSite);
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

// A partition kept per site in memory has no database; one on disk, or not
// kept per site, keeps Chromium's.
TEST(MoltNetDomStorageSitePolicyTest, NoDatabaseInMemoryPerSite) {
  EXPECT_TRUE(MoltDomStorageKeepsNoDatabase(true, true));
  EXPECT_FALSE(MoltDomStorageKeepsNoDatabase(true, false));
  EXPECT_FALSE(MoltDomStorageKeepsNoDatabase(false, true));
  EXPECT_FALSE(MoltDomStorageKeepsNoDatabase(false, false));
}

// Without a database, an area holding items is never dropped from the
// cache: they are nowhere else.
TEST(MoltNetDomStorageSitePolicyTest, WithoutADatabaseItemsStay) {
  EXPECT_TRUE(MoltDomStorageMayDropArea(true, true));
  EXPECT_TRUE(MoltDomStorageMayDropArea(true, false));
  EXPECT_TRUE(MoltDomStorageMayDropArea(false, false));
  EXPECT_FALSE(MoltDomStorageMayDropArea(false, true));
}

// A site's writes: a burst of 1000 writes and one area's limit in bytes,
// refilled at 100 writes and 1 MiB a second.
TEST(MoltNetDomStorageSitePolicyTest, WriteRateBurst) {
  EXPECT_EQ(kMoltDomStorageSiteWritesBurst, 1000);
  EXPECT_EQ(kMoltDomStorageSiteWritesPerSecond, 100);
  EXPECT_EQ(kMoltDomStorageSiteWriteBytesBurst,
            static_cast<int64_t>(kMoltDomStorageBytesPerSite));
  EXPECT_EQ(kMoltDomStorageSiteWriteBytesPerSecond,
            static_cast<int64_t>(kMiB));
  MoltDomStorageWriteAllowance allowance{/*at_ms=*/5000};
  for (int i = 0; i < kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(MoltDomStorageMayWrite(allowance, 5000, 2)) << i;
  }
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 5000, 2));
  // A refused write takes nothing: 9 ms refill less than one write.
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 5009, 2));
  EXPECT_TRUE(MoltDomStorageMayWrite(allowance, 5010, 2));
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 5010, 2));
  // A second refills 100.
  for (int i = 0; i < kMoltDomStorageSiteWritesPerSecond; ++i) {
    ASSERT_TRUE(MoltDomStorageMayWrite(allowance, 6010, 2)) << i;
  }
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 6010, 2));
}

TEST(MoltNetDomStorageSitePolicyTest, WriteRateBytes) {
  MoltDomStorageWriteAllowance allowance{/*at_ms=*/0};
  // One area's whole limit at once.
  EXPECT_TRUE(
      MoltDomStorageMayWrite(allowance, 0, kMoltDomStorageBytesPerSite));
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 0, 1));
  // 1 MiB a second.
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 999, kMiB));
  EXPECT_TRUE(MoltDomStorageMayWrite(allowance, 1000, kMiB));
  // A write larger than the burst costs the burst, once full again.
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 1000, 100 * kMiB));
  EXPECT_TRUE(MoltDomStorageMayWrite(allowance, 100000, 100 * kMiB));
}

// However long a site waits, its allowance is the burst, no more, and the
// arithmetic stays in range.
TEST(MoltNetDomStorageSitePolicyTest, WriteRateCapped) {
  MoltDomStorageWriteAllowance allowance{/*at_ms=*/0};
  for (int i = 0; i < kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(MoltDomStorageMayWrite(allowance, 0, 0));
  }
  constexpr int64_t kYears = int64_t{1} << 50;
  for (int i = 0; i < kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(MoltDomStorageMayWrite(allowance, kYears, 0));
  }
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, kYears, 0));
  // A clock that went back refills nothing.
  EXPECT_FALSE(MoltDomStorageMayWrite(allowance, 0, 0));
}

// A greedy site using up its allowance leaves another site's untouched: the
// polite site's writes all go.
TEST(MoltNetDomStorageSitePolicyTest, WriteRateIsPerSite) {
  MoltDomStorageWriteAllowance greedy{/*at_ms=*/0};
  MoltDomStorageWriteAllowance polite{/*at_ms=*/0};
  int64_t refused = 0;
  for (int64_t ms = 0; ms < 60000; ms += 1) {
    for (int i = 0; i < 10; ++i) {
      refused += MoltDomStorageMayWrite(greedy, ms, 64 * 1024) ? 0 : 1;
    }
    if (ms % 100 == 0) {
      EXPECT_TRUE(MoltDomStorageMayWrite(polite, ms, 1024)) << ms;
    }
  }
  EXPECT_GT(refused, 0);
}

}  // namespace
}  // namespace storage
