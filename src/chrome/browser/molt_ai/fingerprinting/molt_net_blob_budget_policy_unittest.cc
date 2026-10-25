// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's blob budget (storage/browser/blob/molt_blob_budget_policy.h,
// std-only) with MoltNet's values (molt_net_storage_values.h).

#include "storage/browser/blob/molt_blob_budget_policy.h"

#include <cstdint>
#include <optional>

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr uint64_t kMiB = static_cast<uint64_t>(kMoltNetStorageMiB);
constexpr uint64_t kShare = kMoltNetBlobMemoryPerKeyBytes;
constexpr uint64_t kFloor = kMoltNetBlobMemoryBytes;
constexpr uint64_t kReserve =
    static_cast<uint64_t>(kMoltNetStorageMemoryReserveBytes);

uint64_t Room(uint64_t key_used,
              uint64_t total,
              std::optional<uint64_t> available = std::nullopt) {
  return storage::MoltBlobRoom(kShare, key_used, total, kFloor, available,
                               kReserve);
}

TEST(MoltNetBlobBudgetPolicyTest, OtherProfilesKeepOneLimit) {
  EXPECT_FALSE(storage::MoltUsesBlobBudget(0));
  EXPECT_TRUE(storage::MoltUsesBlobBudget(kShare));
}

// Below the floor a key gets its whole share whatever the others hold, and
// the machine isn't asked.
TEST(MoltNetBlobBudgetPolicyTest, AShareBelowTheFloor) {
  EXPECT_EQ(Room(0, 0), kShare);
  EXPECT_EQ(Room(0, kFloor - kShare), kShare);
  EXPECT_FALSE(storage::MoltBlobNeedsMemoryCheck(kShare, 0, kFloor - kShare,
                                                 kFloor));
  EXPECT_EQ(Room(50 * kMiB, 300 * kMiB), kShare - 50 * kMiB);
  // Its own share is all it gets.
  EXPECT_EQ(Room(kShare, kShare), 0u);
  EXPECT_EQ(Room(kShare + kMiB, kShare + kMiB), 0u);
  EXPECT_FALSE(storage::MoltBlobNeedsMemoryCheck(kShare, kShare, kFloor,
                                                 kFloor));
}

// Three greedy keys at their share: a fourth still has all of its share.
// One more greedy key takes the total to the floor; from then on the
// machine decides, for everyone alike.
TEST(MoltNetBlobBudgetPolicyTest, GreedyKeysAgainstAPoliteOne) {
  EXPECT_EQ(Room(0, 3 * kShare), kShare);
  EXPECT_TRUE(storage::MoltBlobNeedsMemoryCheck(kShare, 0, 4 * kShare, kFloor));
  // Plenty available: the polite key gets its share.
  EXPECT_EQ(Room(0, 4 * kShare, 8ull * 1024 * kMiB), kShare);
  // Only the reserve left: nothing beyond the floor, for anyone.
  EXPECT_EQ(Room(0, 4 * kShare, kReserve), 0u);
  EXPECT_EQ(Room(0, 4 * kShare, kReserve + 30 * kMiB), 30 * kMiB);
  // The machine unread or failing: nothing beyond the floor.
  EXPECT_EQ(Room(0, 4 * kShare, std::nullopt), 0u);
  EXPECT_EQ(Room(0, 4 * kShare, 0), 0u);
}

// The machine only adds room beyond the floor; below it, what's left of the
// floor counts even if the machine is short.
TEST(MoltNetBlobBudgetPolicyTest, TheFloorIsAlwaysThere) {
  EXPECT_EQ(Room(0, kFloor - 10 * kMiB, 0), 10 * kMiB);
  EXPECT_TRUE(storage::MoltBlobNeedsMemoryCheck(kShare, 0, kFloor - 10 * kMiB,
                                                kFloor));
  EXPECT_EQ(Room(0, kFloor - 10 * kMiB, kReserve + 100 * kMiB), 100 * kMiB);
}

TEST(MoltNetBlobBudgetPolicyTest, MachineRoom) {
  EXPECT_EQ(storage::MoltBlobMachineRoom(std::nullopt, kReserve), 0u);
  EXPECT_EQ(storage::MoltBlobMachineRoom(kReserve, kReserve), 0u);
  EXPECT_EQ(storage::MoltBlobMachineRoom(kReserve - 1, kReserve), 0u);
  EXPECT_EQ(storage::MoltBlobMachineRoom(kReserve + 1, kReserve), 1u);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
