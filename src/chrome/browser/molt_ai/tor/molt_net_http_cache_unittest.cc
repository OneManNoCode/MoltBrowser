// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's per-site HTTP cache limits
// (net/disk_cache/memory/molt_mem_backend_budget_policy.h) on Chromium's
// real in-memory backend and HttpCache key format, and the per-site
// least-recently-used lists its evictions walk
// (net/disk_cache/memory/molt_mem_backend_site_lru.h).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/tor/molt_net_network_context_policy.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/network_isolation_key.h"
#include "net/base/request_priority.h"
#include "net/base/schemeful_site.h"
#include "net/disk_cache/disk_cache.h"
#include "net/disk_cache/memory/mem_backend_impl.h"
#include "net/disk_cache/memory/molt_mem_backend_budget_policy.h"
#include "net/disk_cache/memory/molt_mem_backend_site_lru.h"
#include "net/http/http_cache.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace molt_ai::tor {
namespace {

std::string Key(const std::string& site, const std::string& path) {
  return "1/0/_dk_" + site + " " + site + " " + site + path;
}

class MoltNetHttpCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    clock_.SetNow(base::Time::Now());
    backend_ = disk_cache::MemBackendImpl::CreateBackend(0, nullptr);
    ASSERT_TRUE(backend_);
    backend_->SetClockForTesting(&clock_);
  }

  void UseBudget(const disk_cache::MoltCacheSiteBudget& budget) {
    backend_->MoltSetPerSiteBudget(budget);
  }

  // Writes `bytes` into a new entry of `key`; the write's result.
  int Write(const std::string& key, int bytes) {
    disk_cache::EntryResult result = backend_->CreateEntry(
        key, net::HIGHEST, disk_cache::EntryResultCallback());
    if (result.net_error() != net::OK) {
      return result.net_error();
    }
    disk_cache::Entry* entry = result.ReleaseEntry();
    auto buffer = base::MakeRefCounted<net::IOBufferWithSize>(bytes);
    const int rv = entry->WriteData(1, 0, buffer.get(), bytes,
                                    net::CompletionOnceCallback(), false);
    entry->Close();
    return rv;
  }

  // Moves time on: the wall clock entries keep their times by, and the
  // monotonic one MoltNet's expiry reads.
  void Advance(base::TimeDelta delta) {
    clock_.Advance(delta);
    task_environment_.AdvanceClock(delta);
  }

  bool Has(const std::string& key) {
    disk_cache::EntryResult result = backend_->OpenEntry(
        key, net::HIGHEST, disk_cache::EntryResultCallback());
    if (result.net_error() != net::OK) {
      return false;
    }
    result.ReleaseEntry()->Close();
    return true;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::SimpleTestClock clock_;
  std::unique_ptr<disk_cache::MemBackendImpl> backend_;
};

TEST_F(MoltNetHttpCacheTest, OnlyMoltNetsContextsKeepLimitsPerSite) {
  EXPECT_TRUE(MoltNetNetworkContextSettingsFor(true).per_site_http_cache);
  EXPECT_FALSE(MoltNetNetworkContextSettingsFor(false).per_site_http_cache);
}

// The key net::HttpCache makes for a request is filed under its top-level
// site.
TEST_F(MoltNetHttpCacheTest, HttpCacheKeysAreFiledByTopLevelSite) {
  const net::SchemefulSite top(GURL("https://a.test"));
  const net::SchemefulSite frame(GURL("https://cdn.test"));
  const std::optional<std::string> key = net::HttpCache::GenerateCacheKey(
      GURL("https://cdn.test/lib.js"), /*load_flags=*/0,
      net::NetworkIsolationKey(top, frame), std::nullopt,
      /*is_subframe_document_resource=*/false,
      /*is_mainframe_navigation=*/false, /*is_shared_resource=*/false,
      std::nullopt, /*include_url=*/true);
  ASSERT_TRUE(key);
  EXPECT_EQ(disk_cache::MoltHttpCacheSiteOfKey(*key), "https://a.test");
}

// One greedy site against a polite one: the greedy site's writes evict only
// its own oldest entries; every entry of the polite site stays.
TEST_F(MoltNetHttpCacheTest, GreedySiteEvictsOnlyItsOwnEntries) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  const int kChunk = 1024 * 1024;
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(Write(Key("https://polite.test", "/" + std::to_string(i)),
                    kChunk),
              kChunk);
  }
  for (int i = 0; i < 100; ++i) {
    Advance(base::Seconds(1));
    ASSERT_EQ(Write(Key("https://greedy.test", "/" + std::to_string(i)),
                    kChunk),
              kChunk);
  }
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(Has(Key("https://polite.test", "/" + std::to_string(i))));
  }
  EXPECT_LE(backend_->MoltSiteSizeForTesting("https://greedy.test"),
            disk_cache::kMoltHttpCachePerSiteBytes);
  // Its newest entry stayed, its oldest went.
  EXPECT_TRUE(Has(Key("https://greedy.test", "/99")));
  EXPECT_FALSE(Has(Key("https://greedy.test", "/0")));
}

// Twelve sites at their full share fill the total: a new site's write isn't
// stored, and nothing anyone stored is evicted for it.
TEST_F(MoltNetHttpCacheTest, AFullTotalEvictsNobody) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  const int kChunk = 1024 * 1024 - 256;
  for (int s = 0; s < disk_cache::kMoltHttpCacheShares; ++s) {
    for (int i = 0; i < 10; ++i) {
      ASSERT_EQ(Write(Key("https://s" + std::to_string(s) + ".test",
                          "/" + std::to_string(i)),
                      kChunk),
                kChunk);
    }
  }
  EXPECT_EQ(Write(Key("https://late.test", "/x"), kChunk),
            net::ERR_INSUFFICIENT_RESOURCES);
  for (int s = 0; s < disk_cache::kMoltHttpCacheShares; ++s) {
    EXPECT_TRUE(Has(Key("https://s" + std::to_string(s) + ".test", "/0")));
  }
}

// Entries a site holds open (a request's, from before it is sent until its
// response is stored) with long keys take neither it past its share nor
// the cache past its total: another site's writes are still stored.
TEST_F(MoltNetHttpCacheTest, KeysOfEntriesHeldOpenCountAgainstTheSite) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  const std::string long_path = "/?" + std::string(1024 * 1024, 'k');
  std::vector<disk_cache::Entry*> held;
  int made = 0;
  for (int i = 0; i < 200; ++i) {
    disk_cache::EntryResult result = backend_->CreateEntry(
        Key("https://greedy.test", long_path + std::to_string(i)),
        net::HIGHEST, disk_cache::EntryResultCallback());
    if (result.net_error() == net::OK) {
      held.push_back(result.ReleaseEntry());
      ++made;
    } else {
      EXPECT_EQ(result.net_error(), net::ERR_INSUFFICIENT_RESOURCES);
    }
  }
  EXPECT_GT(made, 0);
  EXPECT_LT(made, 11);
  EXPECT_LE(backend_->MoltSiteSizeForTesting("https://greedy.test"),
            disk_cache::kMoltHttpCachePerSiteBytes);
  EXPECT_EQ(Write(Key("https://polite.test", "/x"), 1024 * 1024),
            1024 * 1024);
  for (disk_cache::Entry* entry : held) {
    entry->Close();
  }
}

// A site's new entry makes room from its own unused entries, as a write
// does.
TEST_F(MoltNetHttpCacheTest, ANewEntryMakesRoomFromItsSitesOwnEntries) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  const int kChunk = 1024 * 1024 - 256;
  for (int i = 0; i < 10; ++i) {
    Advance(base::Seconds(1));
    ASSERT_EQ(Write(Key("https://a.test", "/" + std::to_string(i)), kChunk),
              kChunk);
  }
  ASSERT_EQ(Write(Key("https://b.test", "/0"), kChunk), kChunk);
  const std::string long_path = "/?" + std::string(2 * 1024 * 1024, 'k');
  disk_cache::EntryResult result = backend_->CreateEntry(
      Key("https://a.test", long_path), net::HIGHEST,
      disk_cache::EntryResultCallback());
  ASSERT_EQ(result.net_error(), net::OK);
  result.ReleaseEntry()->Close();
  EXPECT_FALSE(Has(Key("https://a.test", "/0")));
  EXPECT_TRUE(Has(Key("https://b.test", "/0")));
  EXPECT_LE(backend_->MoltSiteSizeForTesting("https://a.test"),
            disk_cache::kMoltHttpCachePerSiteBytes);
}

// An entry unused for the idle expiry goes, on its next use and in the
// sweep; one used meanwhile stays.
TEST_F(MoltNetHttpCacheTest, UnusedEntriesExpire) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  ASSERT_EQ(Write(Key("https://a.test", "/old"), 100), 100);
  ASSERT_EQ(Write(Key("https://a.test", "/used"), 100), 100);
  Advance(base::Minutes(20));
  EXPECT_TRUE(Has(Key("https://a.test", "/used")));
  ASSERT_EQ(Write(Key("https://a.test", "/used2"), 100), 100);
  Advance(base::Minutes(15));
  EXPECT_FALSE(Has(Key("https://a.test", "/old")));
  EXPECT_TRUE(Has(Key("https://a.test", "/used2")));
  // The sweep frees them without a use.
  ASSERT_EQ(Write(Key("https://b.test", "/x"), 100), 100);
  Advance(base::Minutes(31));
  task_environment_.FastForwardBy(base::Seconds(
      disk_cache::kMoltHttpCacheSweepIntervalSeconds));
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://b.test"), 0);
}

// The sweep passes an expired entry that is in use (it stays) and drops the
// expired ones after it; it stops at the first entry that hasn't expired.
TEST_F(MoltNetHttpCacheTest, TheSweepPassesEntriesInUse) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  disk_cache::EntryResult held = backend_->CreateEntry(
      Key("https://a.test", "/held"), net::HIGHEST,
      disk_cache::EntryResultCallback());
  ASSERT_EQ(held.net_error(), net::OK);
  disk_cache::Entry* held_entry = held.ReleaseEntry();
  ASSERT_EQ(Write(Key("https://b.test", "/old"), 100), 100);
  Advance(base::Minutes(29));
  ASSERT_EQ(Write(Key("https://c.test", "/new"), 100), 100);
  Advance(base::Minutes(2));
  task_environment_.FastForwardBy(
      base::Seconds(disk_cache::kMoltHttpCacheSweepIntervalSeconds));
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://b.test"), 0);
  EXPECT_GT(backend_->MoltSiteSizeForTesting("https://a.test"), 0);
  EXPECT_GT(backend_->MoltSiteSizeForTesting("https://c.test"), 0);
  held_entry->Close();
  task_environment_.FastForwardBy(
      base::Seconds(disk_cache::kMoltHttpCacheSweepIntervalSeconds));
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://a.test"), 0);
  EXPECT_GT(backend_->MoltSiteSizeForTesting("https://c.test"), 0);
}

// Sparse data (child entries) counts against its entry's site and is
// evicted with the site's own entries, never another site's.
TEST_F(MoltNetHttpCacheTest, SparseDataCountsAgainstItsSite) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  ASSERT_EQ(Write(Key("https://polite.test", "/0"), 4096), 4096);
  const int kChunk = 1024 * 1024;
  for (int i = 0; i < 12; ++i) {
    Advance(base::Seconds(1));
    disk_cache::EntryResult result = backend_->CreateEntry(
        Key("https://greedy.test", "/media" + std::to_string(i)),
        net::HIGHEST, disk_cache::EntryResultCallback());
    ASSERT_EQ(result.net_error(), net::OK);
    disk_cache::Entry* entry = result.ReleaseEntry();
    auto buffer = base::MakeRefCounted<net::IOBufferWithSize>(kChunk);
    EXPECT_EQ(entry->WriteSparseData(0, buffer.get(), kChunk,
                                     net::CompletionOnceCallback()),
              kChunk);
    entry->Close();
  }
  EXPECT_LE(backend_->MoltSiteSizeForTesting("https://greedy.test"),
            disk_cache::kMoltHttpCachePerSiteBytes);
  EXPECT_FALSE(Has(Key("https://greedy.test", "/media0")));
  EXPECT_TRUE(Has(Key("https://greedy.test", "/media11")));
  EXPECT_TRUE(Has(Key("https://polite.test", "/0")));
}

// Entries made before the limits were set are in their sites' lists, in
// the order they were used.
TEST_F(MoltNetHttpCacheTest, EntriesMadeBeforeTheLimitsAreInTheirSitesLists) {
  const int kChunk = 1024 * 1024 - 256;
  for (int i = 0; i < 9; ++i) {
    Advance(base::Seconds(1));
    ASSERT_EQ(Write(Key("https://a.test", "/" + std::to_string(i)), kChunk),
              kChunk);
  }
  ASSERT_EQ(Write(Key("https://b.test", "/0"), kChunk), kChunk);
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://b.test"),
            kChunk + static_cast<int64_t>(
                         Key("https://b.test", "/0").size()));
  for (int i = 9; i < 12; ++i) {
    Advance(base::Seconds(1));
    ASSERT_EQ(Write(Key("https://a.test", "/" + std::to_string(i)), kChunk),
              kChunk);
  }
  EXPECT_FALSE(Has(Key("https://a.test", "/0")));
  EXPECT_TRUE(Has(Key("https://a.test", "/11")));
  EXPECT_TRUE(Has(Key("https://b.test", "/0")));
}

// The per-site lists on their own, with a stand-in entry: a site's eviction
// looks at its own entries only, least recently used first, steps past the
// children that follow an entry it evicts, and a site with nothing left is
// forgotten.
class FakeEntry {
 public:
  explicit FakeEntry(const FakeEntry* parent_entry = nullptr)
      : parent_(parent_entry) {}
  const FakeEntry* parent() const { return parent_; }
  disk_cache::MoltCacheSiteLink<FakeEntry>& link() { return link_; }

  bool in_use = false;

 private:
  const raw_ptr<const FakeEntry> parent_;
  disk_cache::MoltCacheSiteLink<FakeEntry> link_{this};
};

TEST(MoltNetHttpCacheSiteLruTest, AnEvictionLooksOnlyAtItsSitesEntries) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  std::vector<std::unique_ptr<FakeEntry>> others, own;
  for (int i = 0; i < 1000; ++i) {
    others.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://other.test", others.back()->link(), ++now_us);
    lru.AddSize("https://other.test", 10);
  }
  for (int i = 0; i < 10; ++i) {
    own.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://a.test", own.back()->link(), ++now_us);
    lru.AddSize("https://a.test", 10);
  }
  own[0]->in_use = true;
  lru.Touch("https://a.test", own[5]->link(), ++now_us);  // used again: last
  std::vector<const FakeEntry*> seen;
  std::vector<FakeEntry*> evicted;
  lru.EvictTill(
      "https://a.test", 70,
      [&](const FakeEntry* entry) {
        seen.push_back(entry);
        return !entry->in_use;
      },
      [&](FakeEntry* entry) {
        evicted.push_back(entry);
        lru.Remove("https://a.test", entry->link());
        lru.AddSize("https://a.test", -10);
      });
  // Its own entries from the least recently used, the one in use passed.
  EXPECT_EQ(seen, (std::vector<const FakeEntry*>{own[0].get(), own[1].get(),
                                                 own[2].get(), own[3].get()}));
  EXPECT_EQ(evicted, (std::vector<FakeEntry*>{own[1].get(), own[2].get(),
                                              own[3].get()}));
  EXPECT_EQ(lru.SizeOf("https://a.test"), 70);
  EXPECT_EQ(lru.SizeOf("https://other.test"), 10000);
  std::vector<const FakeEntry*> order;
  lru.ForEachForTesting("https://a.test", [&](const FakeEntry* entry) {
    order.push_back(entry);
  });
  EXPECT_EQ(order, (std::vector<const FakeEntry*>{
                       own[0].get(), own[4].get(), own[6].get(), own[7].get(),
                       own[8].get(), own[9].get(), own[5].get()}));
}

TEST(MoltNetHttpCacheSiteLruTest, AnEvictedEntrysChildrenArePassed) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  auto parent = std::make_unique<FakeEntry>();
  std::vector<std::unique_ptr<FakeEntry>> children;
  lru.Touch("https://a.test", parent->link(), ++now_us);
  for (int i = 0; i < 3; ++i) {
    children.push_back(std::make_unique<FakeEntry>(parent.get()));
    lru.Touch("https://a.test", children.back()->link(), ++now_us);
  }
  auto next = std::make_unique<FakeEntry>();
  lru.Touch("https://a.test", next->link(), ++now_us);
  lru.AddSize("https://a.test", 50);
  std::vector<const FakeEntry*> seen;
  lru.EvictTill(
      "https://a.test", 0,
      [&](const FakeEntry* entry) {
        seen.push_back(entry);
        return entry == parent.get();
      },
      [&](FakeEntry* entry) {
        // Dooming a parent takes its children out of the list too.
        lru.Remove("https://a.test", entry->link());
        for (auto& child : children) {
          lru.Remove("https://a.test", child->link());
        }
        lru.AddSize("https://a.test", -40);
      });
  EXPECT_EQ(seen, (std::vector<const FakeEntry*>{parent.get(), next.get()}));
}

TEST(MoltNetHttpCacheSiteLruTest, ASiteWithNothingLeftIsForgotten) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  FakeEntry a, b;
  lru.Touch("https://a.test", a.link(), ++now_us);
  lru.AddSize("https://a.test", 10);
  lru.Touch("https://b.test", b.link(), ++now_us);
  lru.AddSize("https://b.test", 10);
  EXPECT_EQ(lru.SiteCountForTesting(), 2u);
  lru.Remove("https://a.test", a.link());
  EXPECT_EQ(lru.SiteCountForTesting(), 2u);  // still counted
  lru.AddSize("https://a.test", -10);
  EXPECT_EQ(lru.SiteCountForTesting(), 1u);
  EXPECT_EQ(lru.SizeOf("https://a.test"), 0);
  lru.Clear();
  EXPECT_EQ(lru.SiteCountForTesting(), 0u);
  EXPECT_FALSE(b.link().linked());
}

// The sweep goes site by site, a few entries a step: a step looks at one
// site's entries only, at most kMoltCacheSweepStepEntries of them, from its
// least recently used to its first that hasn't expired.
TEST(MoltNetHttpCacheSiteLruTest, TheSweepGoesSiteBySiteInSteps) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  std::vector<std::unique_ptr<FakeEntry>> a, b;
  for (int i = 0; i < 150; ++i) {
    a.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://a.test", a.back()->link(), ++now_us);
    lru.AddSize("https://a.test", 1);
  }
  b.push_back(std::make_unique<FakeEntry>());
  lru.Touch("https://b.test", b.back()->link(), ++now_us);
  lru.AddSize("https://b.test", 1);
  b.push_back(std::make_unique<FakeEntry>());
  lru.Touch("https://b.test", b.back()->link(), 1000);  // not expired
  lru.AddSize("https://b.test", 1);
  const auto expired = [](int64_t last_used_us) { return last_used_us < 500; };
  std::vector<const FakeEntry*> looked;
  std::vector<std::pair<std::string, FakeEntry*>> dropped;
  auto drop_from = [&](const std::string& site) {
    return [&, site](FakeEntry* entry) {
      dropped.emplace_back(site, entry);
      lru.Remove(site, entry->link());
      lru.AddSize(site, -1);
    };
  };
  EXPECT_FALSE(lru.sweeping());
  lru.StartSweep();
  ASSERT_TRUE(lru.sweeping());
  // Three steps for a's 150 entries (64, 64, 22), then one for b's.
  EXPECT_TRUE(lru.SweepStep(
      disk_cache::kMoltCacheSweepStepEntries, expired,
      [&](const FakeEntry* entry) {
        looked.push_back(entry);
        return true;
      },
      drop_from("https://a.test")));
  EXPECT_EQ(looked.size(), disk_cache::kMoltCacheSweepStepEntries);
  EXPECT_EQ(dropped.size(), disk_cache::kMoltCacheSweepStepEntries);
  EXPECT_EQ(dropped.front().second, a[0].get());
  EXPECT_TRUE(lru.SweepStep(disk_cache::kMoltCacheSweepStepEntries, expired,
                            [](const FakeEntry*) { return true; },
                            drop_from("https://a.test")));
  EXPECT_EQ(dropped.size(), 2 * disk_cache::kMoltCacheSweepStepEntries);
  // The last of a's: a is then forgotten, and the round goes on to b.
  EXPECT_TRUE(lru.SweepStep(disk_cache::kMoltCacheSweepStepEntries, expired,
                            [](const FakeEntry*) { return true; },
                            drop_from("https://a.test")));
  EXPECT_EQ(dropped.size(), 150u);
  EXPECT_EQ(lru.SiteCountForTesting(), 1u);
  // b's first entry has expired, its second hasn't: the round ends there.
  EXPECT_FALSE(lru.SweepStep(disk_cache::kMoltCacheSweepStepEntries, expired,
                             [](const FakeEntry*) { return true; },
                             drop_from("https://b.test")));
  EXPECT_EQ(dropped.size(), 151u);
  EXPECT_EQ(dropped.back().second, b[0].get());
  EXPECT_FALSE(lru.sweeping());
  EXPECT_EQ(lru.SizeOf("https://b.test"), 1);
}

// A step that drops nothing (the entries it looks at are in use) ends its
// site's turn, so a site holding many entries open can't keep the sweep on
// it.
TEST(MoltNetHttpCacheSiteLruTest, ASweepStepThatDropsNothingMovesOn) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  std::vector<std::unique_ptr<FakeEntry>> a;
  for (size_t i = 0; i < disk_cache::kMoltCacheSweepStepEntries + 1; ++i) {
    a.push_back(std::make_unique<FakeEntry>());
    a.back()->in_use = true;
    lru.Touch("https://a.test", a.back()->link(), ++now_us);
  }
  FakeEntry b;
  lru.Touch("https://b.test", b.link(), ++now_us);
  lru.StartSweep();
  int dropped = 0;
  EXPECT_TRUE(lru.SweepStep(
      disk_cache::kMoltCacheSweepStepEntries,
      [](int64_t) { return true; },
      [](const FakeEntry* entry) { return !entry->in_use; },
      [&](FakeEntry*) { ++dropped; }));
  EXPECT_EQ(dropped, 0);
  EXPECT_FALSE(lru.SweepStep(
      disk_cache::kMoltCacheSweepStepEntries,
      [](int64_t) { return true; },
      [](const FakeEntry* entry) { return !entry->in_use; },
      [&](FakeEntry* entry) {
        ++dropped;
        lru.Remove("https://b.test", entry->link());
      }));
  EXPECT_EQ(dropped, 1);
}

// A step stops at its site's first entry that hasn't expired: the entries
// used after it aren't looked at.
TEST(MoltNetHttpCacheSiteLruTest, ASweepStepStopsAtTheFirstFreshEntry) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  FakeEntry old_entry;
  lru.Touch("https://a.test", old_entry.link(), ++now_us);
  std::vector<std::unique_ptr<FakeEntry>> fresh;
  for (int i = 0; i < 100; ++i) {
    fresh.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://a.test", fresh.back()->link(), 1000 + i);
  }
  int asked = 0;
  lru.StartSweep();
  EXPECT_FALSE(lru.SweepStep(
      disk_cache::kMoltCacheSweepStepEntries,
      [&](int64_t last_used_us) {
        ++asked;
        return last_used_us < 500;
      },
      [](const FakeEntry*) { return true; },
      [&](FakeEntry* entry) { lru.Remove("https://a.test", entry->link()); }));
  EXPECT_EQ(asked, 2);
}

// The site a round is at may go between two steps (its entries doomed by
// other means): the round goes on with the next site.
TEST(MoltNetHttpCacheSiteLruTest, TheSweepsSiteMayGoBetweenSteps) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  int64_t now_us = 0;
  std::vector<std::unique_ptr<FakeEntry>> a;
  for (size_t i = 0; i < 2 * disk_cache::kMoltCacheSweepStepEntries; ++i) {
    a.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://a.test", a.back()->link(), ++now_us);
  }
  FakeEntry b;
  lru.Touch("https://b.test", b.link(), ++now_us);
  lru.StartSweep();
  std::vector<FakeEntry*> dropped;
  auto drop = [&](FakeEntry* entry) {
    dropped.push_back(entry);
    lru.Remove(entry == &b ? "https://b.test" : "https://a.test",
               entry->link());
  };
  ASSERT_TRUE(lru.SweepStep(disk_cache::kMoltCacheSweepStepEntries,
                            [](int64_t) { return true; },
                            [](const FakeEntry*) { return true; }, drop));
  // Every entry of a's left goes another way; a is forgotten.
  for (auto& entry : a) {
    lru.Remove("https://a.test", entry->link());
  }
  ASSERT_EQ(lru.SiteCountForTesting(), 1u);
  EXPECT_FALSE(lru.SweepStep(disk_cache::kMoltCacheSweepStepEntries,
                             [](int64_t) { return true; },
                             [](const FakeEntry*) { return true; }, drop));
  EXPECT_EQ(dropped.back(), &b);
  EXPECT_FALSE(lru.sweeping());
}

// The table of sites is made with room for the cap: adding sites up to it
// never rehashes it, and a site past it gets no room.
TEST(MoltNetHttpCacheSiteLruTest, TheTableOfSitesHasRoomForTheCap) {
  disk_cache::MoltCacheSiteLru<FakeEntry> lru;
  lru.Reserve(disk_cache::kMoltCacheMaxSites, /*seed=*/12345);
  const size_t buckets = lru.BucketCountForTesting();
  EXPECT_GE(buckets, disk_cache::kMoltCacheMaxSites);
  for (size_t i = 0; i < disk_cache::kMoltCacheMaxSites; ++i) {
    const std::string site = "https://s" + std::to_string(i) + ".test";
    ASSERT_TRUE(lru.HasRoomFor(site));
    lru.AddSize(site, 1);
  }
  EXPECT_EQ(lru.BucketCountForTesting(), buckets);
  EXPECT_FALSE(lru.HasRoomFor("https://late.test"));
  EXPECT_TRUE(lru.HasRoomFor("https://s0.test"));
  lru.AddSize("https://s0.test", -1);
  EXPECT_TRUE(lru.HasRoomFor("https://late.test"));
}

// The cache's expiry reads a monotonic clock: entries expire 30 minutes of
// it after their last use, also when the wall clock goes back meanwhile,
// and the sweep finds them however the wall clock moved.
TEST_F(MoltNetHttpCacheTest, ExpiryFollowsAMonotonicClock) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  ASSERT_EQ(Write(Key("https://a.test", "/old"), 100), 100);
  clock_.Advance(-base::Hours(2));  // the wall clock goes back
  ASSERT_EQ(Write(Key("https://b.test", "/new"), 100), 100);
  task_environment_.FastForwardBy(base::Minutes(31));
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://a.test"), 0);
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://b.test"), 0);
  // And forward: nothing used within 30 minutes of the monotonic clock
  // expires.
  ASSERT_EQ(Write(Key("https://c.test", "/x"), 100), 100);
  clock_.Advance(base::Hours(5));
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_TRUE(Has(Key("https://c.test", "/x")));
}

// The sweep drops a site's many expired entries in steps, each a task of
// its own, and other sites' entries in later steps.
TEST_F(MoltNetHttpCacheTest, TheSweepDropsEntriesInStepsOfItsOwn) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  for (int i = 0; i < 200; ++i) {
    ASSERT_EQ(Write(Key("https://many.test", "/" + std::to_string(i)), 10),
              10);
  }
  ASSERT_EQ(Write(Key("https://one.test", "/x"), 10), 10);
  Advance(base::Minutes(31));
  // The sweep's timer fires: its first step runs in the timer's task, the
  // others in tasks of their own.
  task_environment_.FastForwardBy(
      base::Seconds(disk_cache::kMoltHttpCacheSweepIntervalSeconds));
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://many.test"), 0);
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://one.test"), 0);
}

// A site past the cap on sites gets no entry, as a write that doesn't fit;
// once a site's entries are gone there is room again.
TEST_F(MoltNetHttpCacheTest, ASitePastTheCapIsNotCached) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  for (size_t i = 0; i < disk_cache::kMoltCacheMaxSites; ++i) {
    ASSERT_EQ(Write(Key("https://s" + std::to_string(i) + ".test", "/"), 1),
              1);
  }
  EXPECT_EQ(Write(Key("https://late.test", "/"), 1),
            net::ERR_INSUFFICIENT_RESOURCES);
  EXPECT_EQ(Write(Key("https://s1.test", "/more"), 1), 1);
  backend_->DoomAllEntries(net::CompletionOnceCallback());
  EXPECT_EQ(Write(Key("https://late.test", "/"), 1), 1);
}

// Fixed limits: an entry's limit is the budget's, and memory-pressure
// settings don't change them.
TEST_F(MoltNetHttpCacheTest, LimitsAreFixed) {
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  EXPECT_EQ(backend_->MaxFileSize(), disk_cache::kMoltHttpCacheMaxEntryBytes);
  backend_->SetMaxBytes(base::ByteSize(1024));
  EXPECT_EQ(backend_->GetMaxBytesForTesting().InBytes(),
            static_cast<uint64_t>(disk_cache::kMoltHttpCacheTotalBytes));
}

// Without a budget, Chromium's single least-recently-used cache.
TEST_F(MoltNetHttpCacheTest, OtherContextsKeepChromiumsCache) {
  backend_->SetMaxBytes(base::ByteSize(10 * 1024 * 1024));
  ASSERT_EQ(Write(Key("https://polite.test", "/0"), 1024 * 1024),
            1024 * 1024);
  for (int i = 0; i < 20; ++i) {
    Advance(base::Seconds(1));
    Write(Key("https://greedy.test", "/" + std::to_string(i)), 1024 * 1024);
  }
  EXPECT_FALSE(Has(Key("https://polite.test", "/0")));
}

}  // namespace
}  // namespace molt_ai::tor
