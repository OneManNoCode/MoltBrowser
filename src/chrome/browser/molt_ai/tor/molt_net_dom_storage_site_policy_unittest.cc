// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage / sessionStorage caches, totals, commit and write
// rates per site, and its partitions without a database
// (components/services/storage/dom_storage/molt_dom_storage_site_policy.h,
// std-only); the storage sequence taken in turn per site
// (molt_dom_storage_fair_queue.h, std-only); and an area's work split into
// short tasks (molt_dom_storage_split_work.h, std-only).

#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "components/services/storage/dom_storage/molt_dom_storage_fair_queue.h"
#include "components/services/storage/dom_storage/molt_dom_storage_split_work.h"

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

// What an area counts toward its site's total: its items' bytes, and what
// each item and the area take in the storage service. An area holding
// nothing counts nothing.
TEST(MoltNetDomStorageSitePolicyTest, ItemsAndAreasCountWhatTheyTake) {
  EXPECT_EQ(MoltDomStorageCountedBytes(0, 0), 0u);
  EXPECT_EQ(MoltDomStorageCountedBytes(0, 1),
            kMoltDomStorageItemOverheadBytes +
                kMoltDomStorageAreaOverheadBytes);
  EXPECT_EQ(MoltDomStorageCountedBytes(1000, 10),
            1000 + 10 * kMoltDomStorageItemOverheadBytes +
                kMoltDomStorageAreaOverheadBytes);
  // A map node of 80 bytes and a 16-byte slot's rounding for each of the
  // key's and the value's buffers; the area's objects.
  EXPECT_EQ(kMoltDomStorageItemOverheadBytes, 112u);
  EXPECT_EQ(kMoltDomStorageAreaOverheadBytes, 2048u);
  // An area at Chromium's 10 MiB quota fits the site's total with up to
  // about 900 items.
  constexpr size_t kQuota = 10 * kMiB;
  EXPECT_LE(MoltDomStorageCountedBytes(kQuota, 896),
            kMoltDomStorageBytesPerSite);
  EXPECT_GT(MoltDomStorageCountedBytes(kQuota, 897),
            kMoltDomStorageBytesPerSite);
  // Tiny items: about 94,000 at most, where their bytes alone would allow
  // ten million.
  EXPECT_LE(MoltDomStorageCountedBytes(0, 94000),
            kMoltDomStorageBytesPerSite);
  EXPECT_GT(MoltDomStorageCountedBytes(0, 95000),
            kMoltDomStorageBytesPerSite);
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

// Kept per site without a database, an unused area holding nothing goes as
// it is left; with a database, or not kept per site, the purges decide.
TEST(MoltNetDomStorageSitePolicyTest, UnusedEmptyAreasGoAtOnceWithoutADatabase) {
  EXPECT_TRUE(MoltDomStorageDropsUnusedArea(/*caches_per_site=*/true,
                                            /*has_database=*/false));
  EXPECT_FALSE(MoltDomStorageDropsUnusedArea(true, true));
  EXPECT_FALSE(MoltDomStorageDropsUnusedArea(false, false));
  EXPECT_FALSE(MoltDomStorageDropsUnusedArea(false, true));
}

// The control pipe's and the namespaces' turns have names no site's group
// has: serialized sites start with a scheme, the other groups with "^".
TEST(MoltNetDomStorageSitePolicyTest, TurnsThatArentASitesHaveTheirOwnNames) {
  const std::string control = kMoltDomStorageControlGroup;
  const std::string ns = MoltDomStorageNamespaceGroup("0123-abcd");
  EXPECT_EQ(ns, "^ns 0123-abcd");
  for (const std::string& other :
       {MoltDomStorageGroupName(MoltDomStorageGroupKind::kTopLevelSite,
                                "https://a.test"),
        MoltDomStorageGroupName(MoltDomStorageGroupKind::kNonce, "0123-abcd"),
        MoltDomStorageGroupName(MoltDomStorageGroupKind::kOwnKey,
                                "0123-abcd")}) {
    EXPECT_NE(other, control);
    EXPECT_NE(other, ns);
  }
  EXPECT_NE(control, ns);
  EXPECT_NE(MoltDomStorageNamespaceGroup("a"), MoltDomStorageNamespaceGroup("b"));
}

// A big map is freed a chunk a task: at most 1024 items, or once 1 MiB is
// freed (one item at least); a small one at once.
TEST(MoltNetDomStorageSplitWorkTest, MapsAreFreedInChunks) {
  EXPECT_FALSE(MoltDomStorageFreesInChunks(kMoltDomStorageFreeItemsPerTask,
                                           kMoltDomStorageFreeBytesPerTask));
  EXPECT_TRUE(MoltDomStorageFreesInChunks(kMoltDomStorageFreeItemsPerTask + 1,
                                          0));
  EXPECT_TRUE(MoltDomStorageFreesInChunks(1,
                                          kMoltDomStorageFreeBytesPerTask + 1));

  std::map<std::vector<uint8_t>, std::vector<uint8_t>> tiny;
  for (int i = 0; i < 3000; ++i) {
    tiny[{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}] = {'v'};
  }
  std::vector<size_t> left;
  while (MoltDomStorageFreeChunk(tiny)) {
    left.push_back(tiny.size());
  }
  left.push_back(tiny.size());
  EXPECT_EQ(left, (std::vector<size_t>{1976, 952, 0}));

  std::map<std::vector<uint8_t>, std::vector<uint8_t>> big;
  for (uint8_t i = 0; i < 5; ++i) {
    big[{i}] = std::vector<uint8_t>(600 * 1024, 'v');
  }
  left.clear();
  while (MoltDomStorageFreeChunk(big)) {
    left.push_back(big.size());
  }
  // Two of 600 KiB pass 1 MiB: two a chunk.
  EXPECT_EQ(left, (std::vector<size_t>{3, 1}));
  EXPECT_TRUE(big.empty());

  big[{1}] = std::vector<uint8_t>(10 * 1024 * 1024, 'v');
  EXPECT_FALSE(MoltDomStorageFreeChunk(big));  // one item at least

  std::map<std::vector<uint8_t>, size_t> keys_only;
  for (int i = 0; i < 1500; ++i) {
    keys_only[{static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)}] = 99;
  }
  EXPECT_TRUE(MoltDomStorageFreeChunk(keys_only));
  EXPECT_EQ(keys_only.size(), 1500u - kMoltDomStorageFreeItemsPerTask);
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

// ---- The storage sequence in turn per site ----

using FairQueue = MoltDomStorageFairQueue<int>;

// Runs the next task, charging it `cost` microseconds; the site it was for
// ("" if none was ready).
std::string RunNext(FairQueue& queue, int64_t cost, int* task = nullptr) {
  std::string site;
  int popped = 0;
  if (!queue.Pop(&site, &popped)) {
    return "";
  }
  queue.Charge(site, cost);
  if (task) {
    *task = popped;
  }
  return site;
}

// However many tasks a flooding site has queued, a site that had nothing
// queued goes next.
TEST(MoltNetDomStorageFairQueueTest, ASiteWithNothingQueuedGoesNext) {
  FairQueue queue;
  for (int i = 0; i < 10000; ++i) {
    queue.Push("https://flood.test", i);
  }
  for (int i = 0; i < 100; ++i) {
    ASSERT_EQ(RunNext(queue, 3), "https://flood.test");
  }
  queue.Push("https://polite.test", 0);
  EXPECT_EQ(RunNext(queue, 50), "https://polite.test");
  EXPECT_EQ(RunNext(queue, 3), "https://flood.test");
}

// Also when twenty sites flood with cheap tasks, all at the same virtual
// time: a site using less than its share (here a twenty-first) waits for no
// queued task of theirs.
TEST(MoltNetDomStorageFairQueueTest, ManyFloodingSitesDontDelayAPoliteOne) {
  FairQueue queue;
  for (int s = 0; s < 20; ++s) {
    for (int i = 0; i < 1000; ++i) {
      queue.Push("https://flood" + std::to_string(s) + ".test", i);
    }
  }
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 40 + round % 7; ++i) {
      ASSERT_NE(RunNext(queue, 1), "https://polite.test");
    }
    queue.Push("https://polite.test", round);
    EXPECT_EQ(RunNext(queue, 1), "https://polite.test") << round;
  }
}

// A site that used more than its share waits for the others to catch up:
// its next task starts when its last one finished.
TEST(MoltNetDomStorageFairQueueTest, ASiteThatUsedMoreWaits) {
  FairQueue queue;
  for (int s = 0; s < 4; ++s) {
    for (int i = 0; i < 1000; ++i) {
      queue.Push("https://flood" + std::to_string(s) + ".test", i);
    }
  }
  queue.Push("https://dear.test", 0);
  // The flooding sites' tasks take 1 microsecond, the dear site's 40.
  auto run_next = [&queue]() {
    std::string site;
    int task = 0;
    EXPECT_TRUE(queue.Pop(&site, &task));
    queue.Charge(site, site == "https://dear.test" ? 40 : 1);
    return site;
  };
  int before = 0;
  while (run_next() != "https://dear.test") {
    ++before;
  }
  // Each flooding site's first task, which became ready before it.
  EXPECT_EQ(before, 4);
  queue.Push("https://dear.test", 1);
  int waited = 0;
  while (run_next() != "https://dear.test") {
    ++waited;
  }
  // For its 40 microseconds, 39 more of each flooding site's.
  EXPECT_EQ(waited, 4 * 39);
}

// Sites with work queued share the sequence's time, whatever each task
// costs: a site with dear tasks gets fewer of them.
TEST(MoltNetDomStorageFairQueueTest, BusySitesShareTheTime) {
  FairQueue queue;
  for (int i = 0; i < 100000; ++i) {
    queue.Push("https://cheap.test", i);
    queue.Push("https://dear.test", i);
  }
  std::map<std::string, int64_t> time;
  for (int i = 0; i < 20000; ++i) {
    std::string site;
    int task = 0;
    ASSERT_TRUE(queue.Pop(&site, &task));
    const int64_t cost = site == "https://dear.test" ? 100 : 2;
    queue.Charge(site, cost);
    time[site] += cost;
  }
  const int64_t cheap = time["https://cheap.test"];
  const int64_t dear = time["https://dear.test"];
  EXPECT_LE(cheap > dear ? cheap - dear : dear - cheap, 100);
}

// A site's tasks run in the order they were queued.
TEST(MoltNetDomStorageFairQueueTest, ASitesTasksKeepTheirOrder) {
  FairQueue queue;
  for (int i = 0; i < 100; ++i) {
    queue.Push("https://a.test", i);
    queue.Push("https://b.test", 1000 + i);
  }
  std::map<std::string, std::vector<int>> order;
  std::string site;
  int task = 0;
  while (queue.Pop(&site, &task)) {
    order[site].push_back(task);
    queue.Charge(site, 1 + task % 7);
  }
  ASSERT_EQ(order["https://a.test"].size(), 100u);
  ASSERT_EQ(order["https://b.test"].size(), 100u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(order["https://a.test"][i], i);
    EXPECT_EQ(order["https://b.test"][i], 1000 + i);
  }
}

// A site that was idle gets no credit for it: it starts at the current
// virtual time, and then shares the time like any busy site.
TEST(MoltNetDomStorageFairQueueTest, AnIdleSiteGetsNoCredit) {
  FairQueue queue;
  for (int i = 0; i < 10000; ++i) {
    queue.Push("https://busy.test", i);
  }
  for (int i = 0; i < 1000; ++i) {
    RunNext(queue, 10);
  }
  for (int i = 0; i < 1000; ++i) {
    queue.Push("https://back.test", i);
  }
  int back = 0;
  for (int i = 0; i < 100; ++i) {
    back += RunNext(queue, 10) == "https://back.test" ? 1 : 0;
  }
  EXPECT_GE(back, 49);
  EXPECT_LE(back, 51);
}

// Tasks the clock says took no time still count: a site sending many cheap
// messages doesn't go first forever.
TEST(MoltNetDomStorageFairQueueTest, EveryTaskCounts) {
  FairQueue queue;
  for (int i = 0; i < 100; ++i) {
    queue.Push("https://free.test", i);
  }
  for (int i = 0; i < 10; ++i) {
    queue.Push("https://dear.test", i);
  }
  // The free site's tasks take no time by the clock, the dear site's 5
  // microseconds.
  int free_runs = 0;
  int dear_runs = 0;
  while (dear_runs < 3) {
    std::string site;
    int task = 0;
    ASSERT_TRUE(queue.Pop(&site, &task));
    const bool dear = site == "https://dear.test";
    queue.Charge(site, dear ? 5 : 0);
    ++(dear ? dear_runs : free_runs);
  }
  // Each free task counts a microsecond: five of them for each dear one.
  EXPECT_GE(free_runs, 10);
  EXPECT_LE(free_runs, 12);
}

// Nothing is kept for a site once its next task would start now anyway:
// an idle site is remembered only until the virtual time passes when its
// last task finished.
TEST(MoltNetDomStorageFairQueueTest, IdleSitesAreForgotten) {
  FairQueue queue;
  for (int s = 0; s < 100; ++s) {
    queue.Push("https://s" + std::to_string(s) + ".test", s);
  }
  while (!RunNext(queue, 5).empty()) {
  }
  // They all ran at the same virtual time and finished after it.
  EXPECT_EQ(queue.SiteCountForTesting(), 100u);
  queue.Push("https://late.test", 0);
  queue.Push("https://late.test", 1);
  RunNext(queue, 5);
  RunNext(queue, 5);
  EXPECT_EQ(queue.SiteCountForTesting(), 1u);
  EXPECT_FALSE(queue.HasReadyTasks());
}

}  // namespace
}  // namespace storage
