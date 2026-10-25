// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The decisions of net/disk_cache/memory/molt_mem_backend_budget_policy.h
// (std-only).

#include "net/disk_cache/memory/molt_mem_backend_budget_policy.h"

#include <cstdint>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace disk_cache {
namespace {

// Keys in the form net::HttpCache::GenerateCacheKey() makes.
TEST(MoltNetHttpCacheBudgetPolicyTest, TheSiteOfAKeyIsItsTopLevelSite) {
  EXPECT_EQ(MoltHttpCacheSiteOfKey(
                "1/0/_dk_https://a.test https://a.test https://a.test/x.js"),
            "https://a.test");
  // A third party embedded on the site counts against the site.
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_https://a.test https://cdn.test "
                                   "https://cdn.test/lib.js"),
            "https://a.test");
  // Subframe documents and cross-site main-frame navigations.
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_s_https://a.test https://b.test "
                                   "https://b.test/frame.html"),
            "https://a.test");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("0/0/_dk_cn_https://b.test https://b.test "
                                   "https://b.test/"),
            "https://b.test");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_s_cn_https://a.test "
                                   "https://a.test https://a.test/"),
            "https://a.test");
  // An upload's identifier, and a partition after the frame site.
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/12345/_dk_https://a.test https://a.test "
                                   "fenced https://a.test/post"),
            "https://a.test");
  // The site's own string, never more of the key.
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_http://a.test:8080 http://a.test "
                                   "http://a.test/"),
            "http://a.test:8080");
}

TEST(MoltNetHttpCacheBudgetPolicyTest, KeysWithoutTheDoubleKeyHaveNoSite) {
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/https://a.test/x.js"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey(""), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_ https://a.test/"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/0/_dk_https://a.test"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("_dk_https://a.test https://a.test x"), "");
  EXPECT_EQ(MoltHttpCacheSiteOfKey("1/_dk_https://a.test https://a.test x"),
            "");
}

TEST(MoltNetHttpCacheBudgetPolicyTest, MoltNetsNumbers) {
  const MoltCacheSiteBudget budget = MoltNetHttpCacheBudget();
  EXPECT_TRUE(MoltUsesCacheSiteBudget(budget));
  EXPECT_EQ(budget.per_site_bytes, 10 * 1024 * 1024);
  EXPECT_EQ(budget.total_bytes, 12 * budget.per_site_bytes);
  EXPECT_EQ(budget.max_entry_bytes, budget.per_site_bytes / 2);
  EXPECT_EQ(budget.idle_expiry_seconds, 30 * 60);
  EXPECT_GT(budget.sweep_interval_seconds, 0);
  EXPECT_LT(budget.sweep_interval_seconds, budget.idle_expiry_seconds);
  // No budget: Chromium's cache.
  EXPECT_FALSE(MoltUsesCacheSiteBudget(MoltCacheSiteBudget()));
}

TEST(MoltNetHttpCacheBudgetPolicyTest, ASiteMakesRoomFromItsOwnEntries) {
  const MoltCacheSiteBudget budget = MoltNetHttpCacheBudget();
  EXPECT_FALSE(MoltCacheSiteOverShare(budget.per_site_bytes, budget));
  EXPECT_TRUE(MoltCacheSiteOverShare(budget.per_site_bytes + 1, budget));
  EXPECT_EQ(MoltCacheSiteEvictionTarget(budget),
            budget.per_site_bytes - budget.per_site_bytes / 10);
}

// A write fails only for its own site's share or for the total; the total
// is never a reason to evict.
TEST(MoltNetHttpCacheBudgetPolicyTest, WritesPastTheShareOrTheTotalFail) {
  const MoltCacheSiteBudget budget = MoltNetHttpCacheBudget();
  EXPECT_FALSE(MoltCacheWriteRefused(0, 0, budget));
  EXPECT_FALSE(MoltCacheWriteRefused(budget.per_site_bytes,
                                     budget.total_bytes, budget));
  EXPECT_TRUE(MoltCacheWriteRefused(budget.per_site_bytes + 1, 0, budget));
  EXPECT_TRUE(MoltCacheWriteRefused(1, budget.total_bytes + 1, budget));
}

TEST(MoltNetHttpCacheBudgetPolicyTest, UnusedEntriesExpire) {
  const MoltCacheSiteBudget budget = MoltNetHttpCacheBudget();
  EXPECT_FALSE(MoltCacheEntryExpired(0, budget));
  EXPECT_FALSE(MoltCacheEntryExpired(budget.idle_expiry_seconds - 1, budget));
  EXPECT_TRUE(MoltCacheEntryExpired(budget.idle_expiry_seconds, budget));
  // A clock that went back doesn't expire anything.
  EXPECT_FALSE(MoltCacheEntryExpired(-100, budget));
  // No expiry without a budget's.
  EXPECT_FALSE(MoltCacheEntryExpired(1000000, MoltCacheSiteBudget()));
}

TEST(MoltNetHttpCacheBudgetPolicyTest, IsAConstantExpression) {
  static_assert(MoltHttpCacheSiteOfKey("1/0/_dk_https://a.test https://a.test"
                                       " https://a.test/") == "https://a.test");
  static_assert(MoltUsesCacheSiteBudget(MoltNetHttpCacheBudget()));
  static_assert(!MoltCacheEntryExpired(0, MoltNetHttpCacheBudget()));
}

}  // namespace
}  // namespace disk_cache
