// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The per-site shares of MoltNet's fixed-size network caches
// (net/base/molt_site_share_policy.h, std-only): a site evicts only its own
// entries, least recently used first; past the total a new entry of a site
// that holds none isn't kept; records go with a site's last entry. And the
// containers that keep them (net/base/molt_site_lru_cache.h, over
// base::LRUCache and base::LRUCacheSet): without shares the base's own
// behaviour; with them, the container evicts nothing itself and the record
// follows every way an entry comes and goes.

#include "net/base/molt_site_share_policy.h"

#include <optional>
#include <string>
#include <utility>

#include "base/containers/lru_cache.h"
#include "net/base/molt_site_lru_cache.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {

using Shares = MoltSiteShares<std::string, std::string>;

// Adds an entry of `site` to `shares` as a cache does: room first, then the
// entry, if kept. Returns what was evicted, "" if nothing, "x" if not kept.
std::string Put(Shares& shares, const std::string& site,
                const std::string& key) {
  std::string evicted;
  while (std::optional<std::string> victim = shares.NextToEvictFor(site)) {
    evicted += (evicted.empty() ? "" : ",") + *victim;
    shares.Removed(*victim);
  }
  if (!shares.HasRoomFor(site)) {
    return "x";
  }
  shares.Added(site, key);
  return evicted;
}

TEST(MoltNetSiteSharesTest, TheLimits) {
  EXPECT_EQ(kMoltSiteShareSites, 16u);
  EXPECT_EQ(kMoltTlsSessionShares, (MoltSiteShareLimits{256, 4096}));
  EXPECT_EQ(kMoltHttpAuthShares, (MoltSiteShareLimits{20, 320}));
  EXPECT_EQ(kMoltServerInfoShares, (MoltSiteShareLimits{500, 8000}));
  EXPECT_EQ(kMoltHttp11RequiredShares, (MoltSiteShareLimits{100, 1600}));
  EXPECT_EQ(kMoltPreflightShares, (MoltSiteShareLimits{128, 2048}));
  EXPECT_EQ(kMoltDictionaryShares, (MoltSiteShareLimits{100, 1600}));
  EXPECT_EQ(kMoltDictionaryStorageShares, (MoltSiteShareLimits{10, 160}));
  EXPECT_EQ(kMoltDictionaryBytesPerSite, 4u * 1024 * 1024);
  EXPECT_EQ(kMoltDictionaryBytesTotal, 64u * 1024 * 1024);
  // A site gets at least what Chromium gives all sites for the small caches.
  EXPECT_GE(kMoltHttpAuthShares.per_site, 20u);
  EXPECT_GE(kMoltServerInfoShares.per_site, 500u);
  EXPECT_GE(kMoltHttp11RequiredShares.per_site, 100u);
  EXPECT_GE(kMoltDictionaryStorageShares.per_site, 10u);
}

TEST(MoltNetSiteSharesTest, TheDecisions) {
  const MoltSiteShareLimits limits{3, 6};
  // Room: keep, evict nothing.
  EXPECT_FALSE(MoltSiteShareEvictsOwn(limits, 0, 0));
  EXPECT_TRUE(MoltSiteShareHasRoom(limits, 0, 0));
  EXPECT_FALSE(MoltSiteShareEvictsOwn(limits, 2, 5));
  EXPECT_TRUE(MoltSiteShareHasRoom(limits, 2, 5));
  // At its share: its own goes first.
  EXPECT_TRUE(MoltSiteShareEvictsOwn(limits, 3, 3));
  EXPECT_FALSE(MoltSiteShareHasRoom(limits, 3, 3));
  // At the total, with entries of its own: its own goes first.
  EXPECT_TRUE(MoltSiteShareEvictsOwn(limits, 1, 6));
  // At the total, with none: not kept, and nothing evicted.
  EXPECT_FALSE(MoltSiteShareEvictsOwn(limits, 0, 6));
  EXPECT_FALSE(MoltSiteShareHasRoom(limits, 0, 6));
  // A share of nothing keeps nothing.
  EXPECT_FALSE(MoltSiteShareHasRoom({0, 6}, 0, 0));
}

// A site at its share evicts its own least recently used entry; a use makes
// an entry the most recent.
TEST(MoltNetSiteSharesTest, ASiteEvictsItsOwnLeastRecentlyUsed) {
  Shares shares({3, 100});
  EXPECT_EQ(Put(shares, "a", "a1"), "");
  EXPECT_EQ(Put(shares, "a", "a2"), "");
  EXPECT_EQ(Put(shares, "a", "a3"), "");
  shares.Used("a1");
  EXPECT_EQ(Put(shares, "a", "a4"), "a2");
  EXPECT_EQ(Put(shares, "a", "a5"), "a3");
  EXPECT_EQ(Put(shares, "a", "a6"), "a1");
  EXPECT_EQ(shares.HeldBy("a"), 3u);
  EXPECT_EQ(shares.total(), 3u);
}

// However many entries other sites make, a polite site's entries stay.
TEST(MoltNetSiteSharesTest, AGreedySiteNeverEvictsAnother) {
  Shares shares({4, 16});
  ASSERT_EQ(Put(shares, "polite", "p1"), "");
  ASSERT_EQ(Put(shares, "polite", "p2"), "");
  for (int i = 0; i < 1000; ++i) {
    const std::string evicted = Put(shares, "greedy", "g" + std::to_string(i));
    EXPECT_EQ(evicted.find('p'), std::string::npos) << evicted;
  }
  EXPECT_TRUE(shares.Holds("p1"));
  EXPECT_TRUE(shares.Holds("p2"));
  EXPECT_EQ(shares.HeldBy("greedy"), 4u);
  // A group of greedy sites fills the total; the polite site's entries
  // still stay, and a new one of its own takes its own oldest's place.
  for (int s = 0; s < 10; ++s) {
    for (int i = 0; i < 10; ++i) {
      Put(shares, "g" + std::to_string(s),
          "g" + std::to_string(s) + "-" + std::to_string(i));
    }
  }
  EXPECT_EQ(shares.total(), 16u);
  EXPECT_TRUE(shares.Holds("p1"));
  EXPECT_TRUE(shares.Holds("p2"));
  EXPECT_EQ(Put(shares, "polite", "p3"), "p1");
  EXPECT_TRUE(shares.Holds("p2"));
  EXPECT_TRUE(shares.Holds("p3"));
  // And a site holding none gets none while the total is full.
  EXPECT_EQ(Put(shares, "newcomer", "n1"), "x");
  EXPECT_EQ(shares.total(), 16u);
}

// A key already held is only used; removals and Clear() keep the counts.
TEST(MoltNetSiteSharesTest, TheRecord) {
  Shares shares({4, 16});
  shares.Added("a", "k");
  shares.Added("a", "k");
  EXPECT_EQ(shares.total(), 1u);
  shares.Removed("k");
  shares.Removed("k");
  shares.Removed("unknown");
  EXPECT_EQ(shares.total(), 0u);
  EXPECT_EQ(shares.sites(), 0u);
  shares.Added("a", "k1");
  shares.Added("b", "k2");
  shares.Clear();
  EXPECT_EQ(shares.total(), 0u);
  EXPECT_EQ(shares.sites(), 0u);
  EXPECT_FALSE(shares.Holds("k1"));
}

// A site's record goes with its last entry, so sites that come once leave
// none.
TEST(MoltNetSiteSharesTest, RecordsGoWithTheLastEntry) {
  Shares shares(kMoltTlsSessionShares);
  for (int s = 0; s < 1000; ++s) {
    const std::string site = "s" + std::to_string(s);
    Put(shares, site, site + "-1");
    Put(shares, site, site + "-2");
    shares.Removed(site + "-1");
    shares.Removed(site + "-2");
  }
  EXPECT_EQ(shares.sites(), 0u);
  EXPECT_EQ(shares.total(), 0u);
}

// The site of a test key: what comes before its '/'.
struct SiteOfKey {
  std::string operator()(const std::string& key) const {
    return key.substr(0, key.find('/'));
  }
};
using Cache = MoltSiteLRU<base::LRUCache<std::string, int>, SiteOfKey>;
using CacheSet = MoltSiteLRU<base::LRUCacheSet<std::string>, SiteOfKey>;

// Puts `key` as a cache with shares does: room first.
bool PutInto(Cache& cache, const std::string& key, int value = 0) {
  if (!cache.MoltMakeRoomFor(key)) {
    return false;
  }
  cache.Put(key, value);
  return true;
}

bool Holds(Cache& cache, const std::string& key) {
  return cache.Peek(key) != cache.end();
}

// Without shares (every context but MoltNet's), the base's own behaviour.
TEST(MoltNetSiteSharesTest, WithoutSharesTheContainerIsItsBase) {
  Cache cache(3);
  EXPECT_FALSE(cache.MoltKeepsPerSiteShares());
  EXPECT_EQ(cache.molt_shares(), nullptr);
  EXPECT_TRUE(cache.MoltMakeRoomFor("a/1"));
  for (const char* key : {"a/1", "a/2", "b/1", "b/2"}) {
    cache.Put(key, 0);
  }
  EXPECT_EQ(cache.size(), 3u);
  EXPECT_EQ(cache.max_size(), 3u);
  EXPECT_FALSE(Holds(cache, "a/1"));  // any site's least recently used
  cache.Get("a/2");
  cache.ShrinkToSize(1);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_TRUE(Holds(cache, "a/2"));
  cache.UpdateMaxSize(5);
  EXPECT_EQ(cache.max_size(), 5u);
}

// With shares, a site's new entry takes its own least recently used one's
// place, never another site's; a Get() makes an entry the most recent.
TEST(MoltNetSiteSharesTest, TheContainerKeepsEachSitesShare) {
  Cache cache(1024);
  cache.MoltKeepPerSiteShares({2, 4});
  EXPECT_TRUE(cache.MoltKeepsPerSiteShares());
  EXPECT_EQ(cache.max_size(), 4u);
  ASSERT_TRUE(PutInto(cache, "polite/1"));
  ASSERT_TRUE(PutInto(cache, "polite/2"));
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(PutInto(cache, "greedy/" + std::to_string(i)));
  }
  EXPECT_TRUE(Holds(cache, "polite/1"));
  EXPECT_TRUE(Holds(cache, "polite/2"));
  EXPECT_TRUE(Holds(cache, "greedy/98"));
  EXPECT_TRUE(Holds(cache, "greedy/99"));
  EXPECT_EQ(cache.size(), 4u);
  // At the total: a site holding none keeps nothing...
  EXPECT_FALSE(PutInto(cache, "late/1"));
  EXPECT_EQ(cache.size(), 4u);
  // ...and one holding some makes room from its own, least recently used
  // first.
  cache.Get("polite/1");
  ASSERT_TRUE(PutInto(cache, "polite/3"));
  EXPECT_TRUE(Holds(cache, "polite/1"));
  EXPECT_FALSE(Holds(cache, "polite/2"));
  // A key already held is replaced in place.
  EXPECT_TRUE(cache.MoltMakeRoomFor("polite/3"));
  cache.Put("polite/3", 7);
  EXPECT_EQ(cache.size(), 4u);
  EXPECT_EQ(cache.molt_shares()->total(), 4u);
}

// With shares the container evicts nothing itself: no size of its own, and
// its own shrinking does nothing (a memory limit can't make it evict).
TEST(MoltNetSiteSharesTest, TheContainerEvictsNothingItself) {
  Cache cache(2);
  cache.MoltKeepPerSiteShares({3, 48});
  for (int s = 0; s < 4; ++s) {
    for (int i = 0; i < 3; ++i) {
      ASSERT_TRUE(
          PutInto(cache, "s" + std::to_string(s) + "/" + std::to_string(i)));
    }
  }
  EXPECT_EQ(cache.size(), 12u);  // past the base's 2
  cache.ShrinkToSize(0);
  cache.UpdateMaxSize(0);
  EXPECT_EQ(cache.size(), 12u);
  EXPECT_EQ(cache.max_size(), 48u);
}

// The record follows Erase() (both iterators), Clear() and Put() over an
// existing key; entries held before the shares count, in their order.
TEST(MoltNetSiteSharesTest, TheRecordFollowsTheContainer) {
  Cache cache(1024);
  cache.Put("a/1", 0);
  cache.Put("a/2", 0);
  cache.Put("b/1", 0);
  cache.MoltKeepPerSiteShares({2, 32});
  EXPECT_EQ(cache.molt_shares()->HeldBy("a"), 2u);
  EXPECT_EQ(cache.molt_shares()->HeldBy("b"), 1u);
  // a/1 is a's least recently used, as before the shares.
  ASSERT_TRUE(PutInto(cache, "a/3"));
  EXPECT_FALSE(Holds(cache, "a/1"));
  cache.Erase(cache.Peek("a/2"));
  cache.Erase(cache.rbegin());  // b/1, the least recently used
  EXPECT_EQ(cache.molt_shares()->total(), 1u);
  EXPECT_EQ(cache.molt_shares()->HeldBy("b"), 0u);
  EXPECT_EQ(cache.molt_shares()->sites(), 1u);
  cache.Put("a/3", 1);
  EXPECT_EQ(cache.molt_shares()->total(), 1u);
  cache.Clear();
  EXPECT_EQ(cache.molt_shares()->total(), 0u);
  EXPECT_EQ(cache.size(), 0u);
}

// Swap() with shares takes the other container's entries in as Put() does,
// and hands this one's over.
TEST(MoltNetSiteSharesTest, SwapTakesTheOtherEntriesIn) {
  Cache cache(1024);
  cache.MoltKeepPerSiteShares({2, 32});
  ASSERT_TRUE(PutInto(cache, "a/1"));
  base::LRUCache<std::string, int> other(10);
  other.Put("b/1", 1);
  other.Put("b/2", 2);
  cache.Swap(other);
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.molt_shares()->total(), 2u);
  EXPECT_EQ(cache.molt_shares()->HeldBy("b"), 2u);
  EXPECT_EQ(other.size(), 1u);
  EXPECT_NE(other.Peek("a/1"), other.end());
}

// The set variant (servers that required HTTP/1.1).
TEST(MoltNetSiteSharesTest, TheSetKeepsEachSitesShare) {
  CacheSet set(100);
  set.MoltKeepPerSiteShares({2, 32});
  for (const char* key : {"a/1", "b/1", "b/2", "b/3"}) {
    std::string k(key);
    ASSERT_TRUE(set.MoltMakeRoomFor(k));
    set.Put(std::move(k));
  }
  EXPECT_NE(set.Peek("a/1"), set.end());
  EXPECT_EQ(set.Peek("b/1"), set.end());
  EXPECT_EQ(set.molt_shares()->HeldBy("b"), 2u);
  EXPECT_NE(set.Get("b/2"), set.end());
}

// Compression dictionaries are counted after they are added.
TEST(MoltNetSiteSharesTest, Dictionaries) {
  EXPECT_FALSE(MoltSiteDictionariesOver(100, kMoltDictionaryBytesPerSite,
                                        1600, kMoltDictionaryBytesTotal));
  EXPECT_TRUE(MoltSiteDictionariesOver(101, 0, 101, 0));
  EXPECT_TRUE(MoltSiteDictionariesOver(1, kMoltDictionaryBytesPerSite + 1, 1,
                                       kMoltDictionaryBytesPerSite + 1));
  EXPECT_TRUE(MoltSiteDictionariesOver(1, 1, 1601, 1));
  EXPECT_TRUE(MoltSiteDictionariesOver(1, 1, 2, kMoltDictionaryBytesTotal + 1));
}

}  // namespace
}  // namespace net
