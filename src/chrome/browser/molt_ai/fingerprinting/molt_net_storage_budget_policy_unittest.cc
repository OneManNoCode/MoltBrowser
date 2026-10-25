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
constexpr int64_t kFloor = kMoltNetQuotaPoolBytes;

int64_t Quota(int64_t usage, int64_t site_usage) {
  return storage::MoltStorageQuota(kMoltNetQuotaPerStorageKeyBytes, usage,
                                   site_usage, kSite);
}

TEST(MoltNetStorageBudgetPolicyTest, OtherProfilesKeepChromiumsQuota) {
  EXPECT_FALSE(storage::MoltUsesStorageBudget(0));
  EXPECT_TRUE(storage::MoltMayEvictForThePool(0));
  // Without a budget the quota is Chromium's, whatever the site uses, and
  // nobody is told of the total.
  EXPECT_EQ(storage::MoltStorageQuota(200 * kMiB, 10, 500 * kMiB, 0),
            200 * kMiB);
  EXPECT_FALSE(storage::MoltStorageBeyondFloor(0, 10000 * kMiB, kFloor));
}

TEST(MoltNetStorageBudgetPolicyTest, MoltNetNeverEvictsForThePool) {
  EXPECT_TRUE(storage::MoltUsesStorageBudget(kSite));
  EXPECT_FALSE(storage::MoltMayEvictForThePool(kSite));
}

// A site's own share: its storage keys together.
TEST(MoltNetStorageBudgetPolicyTest, SiteShare) {
  // An empty site: its whole share.
  EXPECT_EQ(Quota(0, 0), kSite);
  // Another key of the same site uses 50 MiB: 10 MiB left for this one.
  EXPECT_EQ(Quota(0, 50 * kMiB), 10 * kMiB);
  // This key's own usage is part of the site's: quota is usage + room.
  EXPECT_EQ(Quota(20 * kMiB, 50 * kMiB), 30 * kMiB);
  // Over its share (written before, or racing): no room, never negative.
  EXPECT_EQ(Quota(20 * kMiB, 90 * kMiB), 20 * kMiB);
  EXPECT_EQ(Quota(0, 90 * kMiB), 0);
  // The per-key quota still bounds a key.
  EXPECT_EQ(storage::MoltStorageQuota(5 * kMiB, 0, 0, kSite), 5 * kMiB);
}

// Nothing but the site's own usage goes into its quota: the function has no
// input for other sites or the machine, so however many sites stored their
// whole share, and however short the machine is, a site's quota is the
// same. (The quota manager's call site is checked word for word by the
// round-25 storage harness.)
TEST(MoltNetStorageBudgetPolicyTest, OthersNeverFillAShare) {
  for (int64_t site_usage : {int64_t{0}, 5 * kMiB, kSite - 1}) {
    EXPECT_EQ(Quota(site_usage, site_usage), kSite) << site_usage;
  }
}

// Past the floor (all sites together), the embedder is told; at or below
// it, never. A write never depends on it.
TEST(MoltNetStorageBudgetPolicyTest, BeyondTheFloorTheEmbedderHears) {
  EXPECT_FALSE(storage::MoltStorageBeyondFloor(kSite, 0, kFloor));
  EXPECT_FALSE(storage::MoltStorageBeyondFloor(kSite, kFloor, kFloor));
  EXPECT_TRUE(storage::MoltStorageBeyondFloor(kSite, kFloor + 1, kFloor));
  EXPECT_TRUE(storage::MoltStorageBeyondFloor(kSite, 100 * kFloor, kFloor));
}

TEST(MoltNetStorageBudgetPolicyTest, NoOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax, 0, kSite), kMax);
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax - 1, 0, kSite), kMax);
  // A negative usage (an unknown one) counts as none.
  EXPECT_EQ(storage::MoltStorageQuota(kSite, -1, 0, kSite), kSite);
  EXPECT_EQ(storage::MoltStorageQuota(kSite, 0, -5, kSite), kSite);
  static_assert(storage::MoltStorageQuota(10, 0, 0, 5) == 5);
  static_assert(storage::MoltSiteRoom(3, 5) == 2);
  static_assert(storage::MoltSiteRoom(-3, 5) == 5);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
