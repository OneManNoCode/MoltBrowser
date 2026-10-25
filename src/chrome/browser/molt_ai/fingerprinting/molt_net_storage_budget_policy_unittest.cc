// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's storage budget (storage/browser/quota/
// molt_storage_budget_policy.h, std-only) with MoltNet's values
// (molt_net_storage_values.h).

#include "storage/browser/quota/molt_storage_budget_policy.h"

#include <cstdint>
#include <limits>
#include <optional>

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr int64_t kMiB = kMoltNetStorageMiB;
constexpr int64_t kSite = kMoltNetQuotaPerSiteBytes;
constexpr int64_t kFloor = kMoltNetQuotaPoolBytes;
constexpr int64_t kReserve = kMoltNetStorageMemoryReserveBytes;

int64_t Quota(int64_t usage,
              int64_t site_usage,
              int64_t total_usage,
              std::optional<int64_t> available_memory = std::nullopt) {
  return storage::MoltStorageQuota(kMoltNetQuotaPerStorageKeyBytes, usage,
                                   site_usage, total_usage, kSite, kFloor,
                                   available_memory, kReserve);
}

bool NeedsMemory(int64_t site_usage, int64_t total_usage) {
  return storage::MoltNeedsMemoryCheck(kSite, site_usage, total_usage, kFloor);
}

TEST(MoltNetStorageBudgetPolicyTest, OtherProfilesKeepChromiumsQuota) {
  EXPECT_FALSE(storage::MoltUsesStorageBudget(0));
  EXPECT_TRUE(storage::MoltMayEvictForThePool(0));
  // Without a budget the quota is Chromium's, whatever others use, and the
  // machine is never asked.
  EXPECT_EQ(storage::MoltStorageQuota(200 * kMiB, 10, 500 * kMiB,
                                      10000 * kMiB, 0, 600 * kMiB,
                                      std::nullopt, kReserve),
            200 * kMiB);
  EXPECT_FALSE(storage::MoltNeedsMemoryCheck(0, 0, 10000 * kMiB, kFloor));
}

TEST(MoltNetStorageBudgetPolicyTest, MoltNetNeverEvictsForThePool) {
  EXPECT_TRUE(storage::MoltUsesStorageBudget(kSite));
  EXPECT_FALSE(storage::MoltMayEvictForThePool(kSite));
}

// A site's own share: its storage keys together.
TEST(MoltNetStorageBudgetPolicyTest, SiteShare) {
  // An empty site on an empty identity: its whole share.
  EXPECT_EQ(Quota(0, 0, 0), kSite);
  // Another key of the same site uses 50 MiB: 10 MiB left for this one.
  EXPECT_EQ(Quota(0, 50 * kMiB, 50 * kMiB), 10 * kMiB);
  // This key's own usage is part of the site's: quota is usage + room.
  EXPECT_EQ(Quota(20 * kMiB, 50 * kMiB, 50 * kMiB), 30 * kMiB);
  // Over its share (written before, or racing): no room, never negative.
  EXPECT_EQ(Quota(20 * kMiB, 90 * kMiB, 90 * kMiB), 20 * kMiB);
  EXPECT_EQ(Quota(0, 90 * kMiB, 90 * kMiB), 0);
}

// What other sites stored never changes what a site may store while the
// machine has room, however many sites stored their whole share.
TEST(MoltNetStorageBudgetPolicyTest, OthersNeverFillAShare) {
  const int64_t plenty = 8 * 1024 * kMiB;
  for (int64_t others :
       {int64_t{0}, 100 * kMiB, 9 * kSite, 10 * kSite, 20 * kSite,
        100 * kSite}) {
    EXPECT_EQ(Quota(0, 0, others, plenty), kSite) << others;
    EXPECT_EQ(Quota(5 * kMiB, 5 * kMiB, others + 5 * kMiB, plenty), kSite)
        << others;
    EXPECT_EQ(Quota(kSite - 1, kSite - 1, others + kSite - 1, plenty), kSite)
        << others;
  }
}

// Below the floor the machine is never asked, and its answer would change
// nothing there: all sites may always fill it together.
TEST(MoltNetStorageBudgetPolicyTest, TheFloorNeedsNoMachine) {
  EXPECT_FALSE(NeedsMemory(0, 0));
  EXPECT_FALSE(NeedsMemory(0, kFloor - kSite));
  EXPECT_TRUE(NeedsMemory(0, kFloor - kSite + 1));
  EXPECT_FALSE(NeedsMemory(kSite - 1, kFloor - 1));
  EXPECT_TRUE(NeedsMemory(kSite - 1, kFloor));
  // A site at its share can't write: nothing to ask.
  EXPECT_FALSE(NeedsMemory(kSite, 100 * kSite));
  // With no answer (unknown), up to the floor still.
  EXPECT_EQ(Quota(0, 0, kFloor - kSite), kSite);
  EXPECT_EQ(Quota(0, 0, kFloor - 10 * kMiB), 10 * kMiB);
  EXPECT_EQ(Quota(0, 0, kFloor - 10 * kMiB, 0), 10 * kMiB);
}

// Beyond the floor, what the machine has available above the reserve; a
// write it has no room for fails for the writer.
TEST(MoltNetStorageBudgetPolicyTest, BeyondTheFloorTheMachineDecides) {
  // Room for 30 MiB more above the reserve.
  EXPECT_EQ(Quota(0, 0, kFloor, kReserve + 30 * kMiB), 30 * kMiB);
  EXPECT_EQ(Quota(10 * kMiB, 10 * kMiB, kFloor + 200 * kMiB,
                  kReserve + 30 * kMiB),
            40 * kMiB);
  // The site's own share still bounds it.
  EXPECT_EQ(Quota(0, 0, kFloor, kReserve + 300 * kMiB), kSite);
  // Machine at or below its reserve: nothing more.
  EXPECT_EQ(Quota(7 * kMiB, 7 * kMiB, kFloor + 10 * kMiB, kReserve), 7 * kMiB);
  EXPECT_EQ(Quota(7 * kMiB, 7 * kMiB, kFloor, kReserve - 1), 7 * kMiB);
  // Unknown: nothing beyond the floor.
  EXPECT_EQ(Quota(0, 0, kFloor), 0);
  // The floor's room or the machine's, whichever is more.
  EXPECT_EQ(Quota(0, 0, kFloor - 20 * kMiB, kReserve + 5 * kMiB), 20 * kMiB);
  EXPECT_EQ(Quota(0, 0, kFloor - 20 * kMiB, kReserve + 50 * kMiB), 50 * kMiB);
}

TEST(MoltNetStorageBudgetPolicyTest, NoOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax, 0, 0, kSite, kFloor,
                                      std::nullopt, kReserve),
            kMax);
  EXPECT_EQ(storage::MoltStorageQuota(kMax, kMax - 1, 0, 0, kSite, kFloor,
                                      kMax, kReserve),
            kMax);
  // A negative usage (an unknown one) counts as none.
  EXPECT_EQ(storage::MoltStorageQuota(kSite, -1, 0, 0, kSite, kFloor,
                                      std::nullopt, kReserve),
            kSite);
  EXPECT_EQ(storage::MoltStorageQuota(kSite, 0, -5, -5, kSite, kFloor,
                                      std::nullopt, kReserve),
            kSite);
  static_assert(storage::MoltStorageQuota(10, 0, 0, 0, 5, 100, std::nullopt,
                                          0) == 5);
  static_assert(storage::MoltMachineRoom(std::nullopt, 0) == 0);
  static_assert(storage::MoltMachineRoom(100, 30) == 70);
  static_assert(storage::MoltMachineRoom(10, 30) == 0);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
