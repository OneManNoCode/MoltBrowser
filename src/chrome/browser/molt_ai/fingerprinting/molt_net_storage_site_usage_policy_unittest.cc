// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltNet's storage budget counts per site
// (storage/browser/quota/molt_storage_budget.h, std-only templates): the
// cached usage per site and for all sites, and the storage keys to ask
// again. Sites and keys are strings here; the quota system uses
// net::SchemefulSite and blink::StorageKey.

#include "storage/browser/quota/molt_storage_budget.h"

#include <cstdint>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace storage {
namespace {

using Totals = MoltSiteUsageTotals<std::string>;
using Stale = MoltStaleStorageKeys<std::string, std::string>;

// A site's usage is the sum of its cached buckets' usage, moved by each
// change; another site's changes don't touch it.
TEST(MoltNetStorageSiteUsageTest, TotalsFollowChanges) {
  Totals totals;
  EXPECT_EQ(totals.Of("https://a.test"), 0);
  totals.Changed("https://a.test", 0, 100);  // a bucket cached
  totals.Changed("https://a.test", 0, 50);   // another
  totals.Changed("https://b.test", 0, 7);
  EXPECT_EQ(totals.Of("https://a.test"), 150);
  EXPECT_EQ(totals.Of("https://b.test"), 7);
  EXPECT_EQ(totals.total(), 157);
  totals.Changed("https://a.test", 100, 130);  // grew
  totals.Changed("https://a.test", 50, 0);     // no longer cached
  EXPECT_EQ(totals.Of("https://a.test"), 130);
  EXPECT_EQ(totals.total(), 137);
  // A site at zero isn't kept.
  totals.Changed("https://b.test", 7, 0);
  EXPECT_EQ(totals.Of("https://b.test"), 0);
  EXPECT_EQ(totals.site_count(), 1u);
  totals.Clear();
  EXPECT_EQ(totals.total(), 0);
  EXPECT_EQ(totals.site_count(), 0u);
}

// Stale keys are kept per site, once each, and taking a site's leaves the
// others'.
TEST(MoltNetStorageSiteUsageTest, StaleKeysPerSite) {
  Stale stale;
  EXPECT_TRUE(stale.Take("https://a.test").empty());
  stale.Mark("https://a.test", "https://x.a.test");
  stale.Mark("https://a.test", "https://x.a.test");
  stale.Mark("https://a.test", "https://a.test");
  stale.Mark("https://b.test", "https://b.test");
  EXPECT_TRUE(stale.HasAny("https://a.test"));
  const std::vector<std::string> keys = stale.Take("https://a.test");
  EXPECT_EQ(keys,
            (std::vector<std::string>{"https://a.test", "https://x.a.test"}));
  EXPECT_FALSE(stale.HasAny("https://a.test"));
  EXPECT_TRUE(stale.Take("https://a.test").empty());
  // A change after the take marks the key again.
  stale.Mark("https://a.test", "https://a.test");
  EXPECT_EQ(stale.Take("https://a.test").size(), 1u);
  EXPECT_EQ(stale.Take("https://b.test").size(), 1u);
}

// The cost of reading a polite site's usage, and of the keys asked again
// for it, doesn't grow with what a greedy site stores: a model of a quota
// check under the budget, counting the buckets and keys it touches.
TEST(MoltNetStorageSiteUsageTest, AGreedySiteDoesntSlowAnothersChecks) {
  Totals totals;
  Stale stale;
  totals.Changed("https://polite.test", 0, 1000);
  for (int i = 0; i < 100000; ++i) {
    const std::string key = "https://s" + std::to_string(i) + ".greedy.test";
    totals.Changed("https://greedy.test", 0, 10);
    stale.Mark("https://greedy.test", key);
  }
  // The polite site's check: no key of its own to ask again, one lookup.
  EXPECT_TRUE(stale.Take("https://polite.test").empty());
  EXPECT_EQ(totals.Of("https://polite.test"), 1000);
  // The greedy site's check asks again for its own keys only.
  EXPECT_EQ(stale.Take("https://greedy.test").size(), 100000u);
  EXPECT_EQ(totals.Of("https://greedy.test"), 1000000);
  EXPECT_EQ(totals.total(), 1001000);
}

}  // namespace
}  // namespace storage
