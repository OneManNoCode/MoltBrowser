// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's blob budget (storage/browser/blob/molt_blob_budget_policy.h,
// std-only) with MoltNet's values (molt_net_storage_values.h).

#include "storage/browser/blob/molt_blob_budget_policy.h"

#include <cstdint>
#include <set>
#include <string>

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr uint64_t kMiB = static_cast<uint64_t>(kMoltNetStorageMiB);
constexpr uint64_t kShare = kMoltNetBlobMemoryPerKeyBytes;
constexpr uint64_t kFloor = kMoltNetBlobMemoryBytes;

TEST(MoltNetBlobBudgetPolicyTest, OtherProfilesKeepOneLimit) {
  EXPECT_FALSE(storage::MoltUsesBlobBudget(0));
  EXPECT_TRUE(storage::MoltUsesBlobBudget(kShare));
  EXPECT_FALSE(storage::MoltBlobBeyondFloor(0, 10 * kFloor, kFloor));
}

// A key's room is what is left of its own share: nothing else counts.
TEST(MoltNetBlobBudgetPolicyTest, OnlyTheKeysOwnShare) {
  EXPECT_EQ(storage::MoltBlobRoom(kShare, 0), kShare);
  EXPECT_EQ(storage::MoltBlobRoom(kShare, kShare - kMiB), kMiB);
  EXPECT_EQ(storage::MoltBlobRoom(kShare, kShare), 0u);
  EXPECT_EQ(storage::MoltBlobRoom(kShare, kShare + kMiB), 0u);
}

// Past the floor the embedder hears of each blob stored; below it, never.
TEST(MoltNetBlobBudgetPolicyTest, BeyondTheFloorTheEmbedderHears) {
  EXPECT_FALSE(storage::MoltBlobBeyondFloor(kShare, 0, kFloor));
  EXPECT_FALSE(storage::MoltBlobBeyondFloor(kShare, kFloor, kFloor));
  EXPECT_TRUE(storage::MoltBlobBeyondFloor(kShare, kFloor + 1, kFloor));
}

// What the browser holds for a site before it becomes a blob (a Background
// Fetch's response as it downloads) counts like a blob: within the key's
// room, refused past it; off MoltNet not counted.
TEST(MoltNetBlobBudgetPolicyTest, HeldMemoryCountsLikeABlob) {
  using storage::MoltBlobHold;
  using storage::MoltBlobHoldFor;
  EXPECT_EQ(MoltBlobHoldFor(0, 0, kShare), MoltBlobHold::kNotCounted);
  EXPECT_EQ(MoltBlobHoldFor(0, 10 * kShare, kShare), MoltBlobHold::kNotCounted);
  EXPECT_EQ(MoltBlobHoldFor(kShare, 0, kShare), MoltBlobHold::kCounted);
  EXPECT_EQ(MoltBlobHoldFor(kShare, kShare - kMiB, kMiB),
            MoltBlobHold::kCounted);
  EXPECT_EQ(MoltBlobHoldFor(kShare, kShare - kMiB, kMiB + 1),
            MoltBlobHold::kRefused);
  EXPECT_EQ(MoltBlobHoldFor(kShare, kShare, 1), MoltBlobHold::kRefused);
  EXPECT_EQ(MoltBlobHoldFor(kShare, 0, kShare + 1), MoltBlobHold::kRefused);
  // Nothing more is always fine.
  EXPECT_EQ(MoltBlobHoldFor(kShare, kShare, 0), MoltBlobHold::kCounted);
}

// The blobs the browser makes for a site count to that site's own key,
// derived from a keyed digest of the site, with nothing kept per site: apart
// from every renderer process (positive 31-bit ids) and the browser's own
// key, by construction; apart from other sites unless their digests' 63 bits
// meet, which a page can't arrange (it never sees the secret or a key).
TEST(MoltNetBlobBudgetPolicyTest, SiteKeys) {
  using storage::MoltBlobSiteKeyFromDigest;
  for (uint64_t digest :
       {uint64_t{0}, uint64_t{1}, uint64_t{INT32_MAX}, ~uint64_t{0},
        uint64_t{0x8000000000000000}, uint64_t{0x123456789abcdef0}}) {
    const uint64_t key = MoltBlobSiteKeyFromDigest(digest);
    EXPECT_NE(key & storage::kMoltBlobSiteKeyBit, 0u);
    EXPECT_NE(key, storage::kMoltBlobBrowserKey);
    EXPECT_GT(key, static_cast<uint64_t>(INT32_MAX));
    // The digest's other 63 bits are the key's.
    EXPECT_EQ(key & ~storage::kMoltBlobSiteKeyBit,
              digest & ~storage::kMoltBlobSiteKeyBit);
  }
  // Distinct digests (but for the top bit) give distinct keys.
  std::set<uint64_t> keys;
  for (uint64_t i = 0; i < 10000; ++i) {
    keys.insert(MoltBlobSiteKeyFromDigest(i * 0x9e3779b97f4a7c15ULL));
  }
  EXPECT_EQ(keys.size(), 10000u);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
