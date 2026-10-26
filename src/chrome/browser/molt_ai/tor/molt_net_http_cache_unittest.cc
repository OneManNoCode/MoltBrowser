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
    clock_.Advance(base::Seconds(1));
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
    clock_.Advance(base::Seconds(1));
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
  clock_.Advance(base::Minutes(20));
  EXPECT_TRUE(Has(Key("https://a.test", "/used")));
  ASSERT_EQ(Write(Key("https://a.test", "/used2"), 100), 100);
  clock_.Advance(base::Minutes(15));
  EXPECT_FALSE(Has(Key("https://a.test", "/old")));
  EXPECT_TRUE(Has(Key("https://a.test", "/used2")));
  // The sweep frees them without a use.
  ASSERT_EQ(Write(Key("https://b.test", "/x"), 100), 100);
  clock_.Advance(base::Minutes(31));
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
  clock_.Advance(base::Minutes(29));
  ASSERT_EQ(Write(Key("https://c.test", "/new"), 100), 100);
  clock_.Advance(base::Minutes(2));
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
    clock_.Advance(base::Seconds(1));
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
    clock_.Advance(base::Seconds(1));
    ASSERT_EQ(Write(Key("https://a.test", "/" + std::to_string(i)), kChunk),
              kChunk);
  }
  ASSERT_EQ(Write(Key("https://b.test", "/0"), kChunk), kChunk);
  UseBudget(disk_cache::MoltNetHttpCacheBudget());
  EXPECT_EQ(backend_->MoltSiteSizeForTesting("https://b.test"),
            kChunk + static_cast<int64_t>(
                         Key("https://b.test", "/0").size()));
  for (int i = 9; i < 12; ++i) {
    clock_.Advance(base::Seconds(1));
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
  std::vector<std::unique_ptr<FakeEntry>> others, own;
  for (int i = 0; i < 1000; ++i) {
    others.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://other.test", others.back()->link());
    lru.AddSize("https://other.test", 10);
  }
  for (int i = 0; i < 10; ++i) {
    own.push_back(std::make_unique<FakeEntry>());
    lru.Touch("https://a.test", own.back()->link());
    lru.AddSize("https://a.test", 10);
  }
  own[0]->in_use = true;
  lru.Touch("https://a.test", own[5]->link());  // used again: last
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
  auto parent = std::make_unique<FakeEntry>();
  std::vector<std::unique_ptr<FakeEntry>> children;
  lru.Touch("https://a.test", parent->link());
  for (int i = 0; i < 3; ++i) {
    children.push_back(std::make_unique<FakeEntry>(parent.get()));
    lru.Touch("https://a.test", children.back()->link());
  }
  auto next = std::make_unique<FakeEntry>();
  lru.Touch("https://a.test", next->link());
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
  FakeEntry a, b;
  lru.Touch("https://a.test", a.link());
  lru.AddSize("https://a.test", 10);
  lru.Touch("https://b.test", b.link());
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
    clock_.Advance(base::Seconds(1));
    Write(Key("https://greedy.test", "/" + std::to_string(i)), 1024 * 1024);
  }
  EXPECT_FALSE(Has(Key("https://polite.test", "/0")));
}

}  // namespace
}  // namespace molt_ai::tor
