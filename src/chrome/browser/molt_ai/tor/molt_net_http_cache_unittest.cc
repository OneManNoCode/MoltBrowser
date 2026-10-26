// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's per-site HTTP cache limits
// (net/disk_cache/memory/molt_mem_backend_budget_policy.h) on Chromium's
// real in-memory backend and HttpCache key format.

#include <memory>
#include <optional>
#include <string>
#include <vector>

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
