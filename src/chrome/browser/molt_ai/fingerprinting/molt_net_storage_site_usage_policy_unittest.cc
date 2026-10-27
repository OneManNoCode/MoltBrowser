// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What MoltNet's storage budget counts per site
// (storage/browser/quota/molt_storage_usage_counts.h, std-only templates):
// cached usage per site and for all sites, and the storage keys to ask
// again. Sites and keys are strings here; the quota system uses
// MoltBudgetSite (molt_storage_budget.h) and blink::StorageKey.

#include "storage/browser/quota/molt_storage_usage_counts.h"

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

// A site that is gone (a page's nonce) leaves no stale keys behind; others
// keep theirs.
TEST(MoltNetStorageSiteUsageTest, AGoneSitesStaleKeysGo) {
  Stale stale;
  stale.Mark("nonce-a", "https://t.test^nonce-a");
  stale.Mark("https://b.test", "https://b.test");
  stale.Forget("nonce-a");
  stale.Forget("unknown");
  EXPECT_FALSE(stale.HasAny("nonce-a"));
  EXPECT_TRUE(stale.HasAny("https://b.test"));
  EXPECT_EQ(stale.site_count(), 1u);
}

// One refresh of a site's stale keys at a time: a check arriving while one
// runs waits for it and one more (for what was marked meanwhile); the site
// has none running once a refresh ends with nobody waiting. Other sites'
// refreshes run on their own.
TEST(MoltNetStorageSiteUsageTest, OneRefreshAtATimePerSite) {
  MoltSiteRefreshes<std::string, int> refreshes;
  EXPECT_FALSE(refreshes.IsRunning("a"));
  refreshes.Start("a");
  refreshes.Start("b");
  EXPECT_TRUE(refreshes.IsRunning("a"));
  refreshes.Wait("a", 2);
  refreshes.Wait("a", 3);
  // The first refresh ends: the two that waited get the next one.
  EXPECT_EQ(refreshes.Finished("a"), (std::vector<int>{2, 3}));
  EXPECT_TRUE(refreshes.IsRunning("a"));
  refreshes.Wait("a", 4);
  EXPECT_EQ(refreshes.Finished("a"), (std::vector<int>{4}));
  EXPECT_TRUE(refreshes.Finished("a").empty());
  EXPECT_FALSE(refreshes.IsRunning("a"));
  EXPECT_TRUE(refreshes.IsRunning("b"));
  EXPECT_TRUE(refreshes.Finished("b").empty());
  EXPECT_TRUE(refreshes.Finished("unknown").empty());
  EXPECT_EQ(refreshes.site_count(), 0u);
}

// The race the serialization closes: a site at 50 counted, one stale key
// whose real usage is 5 more. Two checks at once: the first takes the key
// and asks for it; the second, reading at once, saw 50 and so more room than
// the site had. Waiting for the refresh, it reads 55.
TEST(MoltNetStorageSiteUsageTest, ASecondCheckReadsAfterTheRefresh) {
  Totals totals;
  Stale stale;
  MoltSiteRefreshes<std::string, int> refreshes;
  totals.Changed("s", 0, 50);
  stale.Mark("s", "k");
  // Check 1.
  ASSERT_FALSE(refreshes.IsRunning("s"));
  refreshes.Start("s");
  const std::vector<std::string> asked = stale.Take("s");
  ASSERT_EQ(asked.size(), 1u);
  // Check 2 arrives before the answer for k.
  ASSERT_TRUE(refreshes.IsRunning("s"));
  refreshes.Wait("s", 2);
  // (Chromium-shaped, without the wait: it would read now.)
  EXPECT_EQ(totals.Of("s"), 50);
  // k's usage comes back and is counted; check 1 reads; then check 2's
  // refresh (nothing new) and its read.
  totals.Changed("s", 0, 5);
  EXPECT_EQ(totals.Of("s"), 55);
  const std::vector<int> waiting = refreshes.Finished("s");
  ASSERT_EQ(waiting, (std::vector<int>{2}));
  EXPECT_TRUE(stale.Take("s").empty());
  EXPECT_EQ(totals.Of("s"), 55);
  EXPECT_TRUE(refreshes.Finished("s").empty());
}

}  // namespace
}  // namespace storage
