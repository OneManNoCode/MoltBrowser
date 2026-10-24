// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's storage budget (storage/browser/quota/
// molt_storage_budget_policy.h, std-only) with MoltNet's values
// (molt_net_storage_values.h).

#include "storage/browser/quota/molt_storage_budget_policy.h"

#include <cstdint>
#include <limits>

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr int64_t kMiB = kMoltNetStorageMiB;
constexpr int64_t kSite = kMoltNetQuotaPerSiteBytes;
constexpr int64_t kTotal = kMoltNetQuotaPoolBytes;

int64_t Quota(int64_t usage, int64_t site_usage, int64_t total_usage) {
  return storage::MoltStorageQuota(kMoltNetQuotaPerStorageKeyBytes, usage,
                                   site_usage, total_usage, kSite, kTotal);
}

TEST(MoltNetStorageBudgetPolicyTest, OtherProfilesKeepChromiumsQuota) {
  EXPECT_FALSE(storage::MoltUsesStorageBudget(0));
  EXPECT_TRUE(storage::MoltMayEvictForThePool(0));
  // Without a budget the quota is Chromium's, whatever others use.
  EXPECT_EQ(storage::MoltStorageQuota(200 * kMiB, 10, 500 * kMiB,
                                      10000 * kMiB, 0, 600 * kMiB),
            200 * kMiB);
}

TEST(MoltNetStorageBudgetPolicyTest, MoltNetNeverEvictsForThePool) {
  EXPECT_TRUE(storage::MoltUsesStorageBudget(kSite));
  EXPECT_FALSE(storage::MoltMayEvictForThePool(kSite));
}

// A site's own budget: its storage keys together.
TEST(MoltNetStorageBudgetPolicyTest, SiteBudget) {
  // An empty site on an empty identity: its whole budget.
  EXPECT_EQ(Quota(0, 0, 0), kSite);
  // Another key of the same site uses 50 MiB: 10 MiB left for this one.
  EXPECT_EQ(Quota(0, 50 * kMiB, 50 * kMiB), 10 * kMiB);
  // This key's own usage is part of the site's: quota is usage + room.
  EXPECT_EQ(Quota(20 * kMiB, 50 * kMiB, 50 * kMiB), 30 * kMiB);
  // Over budget (written before, or racing): no room, never negative.
  EXPECT_EQ(Quota(20 * kMiB, 90 * kMiB, 90 * kMiB), 20 * kMiB);
  EXPECT_EQ(Quota(0, 90 * kMiB, 90 * kMiB), 0);
}

// What other sites use changes nothing, until the total is nearly full.
TEST(MoltNetStorageBudgetPolicyTest, OthersDontMatterBelowTheTotal) {
  for (int64_t others : {int64_t{0}, 100 * kMiB, 9 * kSite}) {
    EXPECT_EQ(Quota(0, 0, others), kSite) << others;
    EXPECT_EQ(Quota(5 * kMiB, 5 * kMiB, others + 5 * kMiB), kSite) << others;
  }
  // Nine other sites full and this one one byte short of its budget: it may
  // still write that byte.
  EXPECT_EQ(Quota(kSite - 1, kSite - 1, 9 * kSite + kSite - 1), kSite);
  // Ten others full: the total is.
  EXPECT_EQ(Quota(0, 0, 10 * kSite), 0);
  EXPECT_EQ(storage::MoltSitesToFillStorage(kTotal, kSite), 10);
  EXPECT_EQ(storage::MoltSitesToFillStorage(100, 10), 10);
  EXPECT_EQ(storage::MoltSitesToFillStorage(105, 10), 10);
  EXPECT_EQ(storage::MoltSitesToFillStorage(110, 10), 11);
}

TEST(MoltNetStorageBudgetPolicyTest, NoOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax, 0, 0, kSite, kTotal), kMax);
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax - 1, 0, 0, kSite, kTotal),
            kMax);
  // A negative usage (an unknown one) counts as none.
  EXPECT_EQ(storage::MoltStorageQuota(kSite, -1, 0, 0, kSite, kTotal), kSite);
  static_assert(storage::MoltStorageQuota(10, 0, 0, 0, 5, 100) == 5);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
