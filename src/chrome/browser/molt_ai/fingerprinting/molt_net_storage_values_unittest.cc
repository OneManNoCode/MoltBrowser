// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"

#include <cstdint>

#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr int64_t kGiB = 1024 * kMoltNetStorageMiB;

// Chromium's off-the-record pool for a machine with `memory` bytes at the
// low and high end of its random ratio (storage/browser/quota/
// quota_settings.cc: 0.15 to 0.2), and its per-site share (a third).
constexpr int64_t IncognitoPoolLow(int64_t memory) {
  return memory * 15 / 100;
}
constexpr int64_t IncognitoPoolHigh(int64_t memory) {
  return memory * 20 / 100;
}

TEST(MoltNetStorageValuesTest, QuotaIsChromiumsOnTheReferenceMachine) {
  EXPECT_EQ(kMoltNetStorageReferenceMemory, 4 * kGiB);
  EXPECT_EQ(kMoltNetQuotaPoolBytes, 600 * kMoltNetStorageMiB);
  EXPECT_LE(kMoltNetQuotaPoolBytes,
            IncognitoPoolLow(kMoltNetStorageReferenceMemory));
  EXPECT_GT(kMoltNetQuotaPoolBytes,
            IncognitoPoolLow(kMoltNetStorageReferenceMemory) -
                100 * kMoltNetStorageMiB);
}

// Each site gets a tenth of what all can always store together; a storage
// key may use what its site may.
TEST(MoltNetStorageValuesTest, EachSiteATenthOfThePool) {
  EXPECT_EQ(kMoltNetQuotaPerSiteBytes, 60 * kMoltNetStorageMiB);
  EXPECT_EQ(kMoltNetQuotaPerSiteBytes * 10, kMoltNetQuotaPoolBytes);
  EXPECT_EQ(kMoltNetQuotaPerStorageKeyBytes, kMoltNetQuotaPerSiteBytes);
}

// Beyond the pool a write must leave the machine 1 GiB available: on the
// reference machine the pool, the blob memory and the reserve still leave
// half of it to the system, the browser and other applications.
TEST(MoltNetStorageValuesTest, MemoryReserve) {
  EXPECT_EQ(kMoltNetStorageMemoryReserveBytes, kGiB);
  EXPECT_LE(kMoltNetQuotaPoolBytes +
                static_cast<int64_t>(kMoltNetBlobMemoryBytes) +
                kMoltNetStorageMemoryReserveBytes,
            kMoltNetStorageReferenceMemory * 3 / 5);
}

TEST(MoltNetStorageValuesTest, NoMoreThanIncognitoOnBiggerMachines) {
  // The device memory MoltNet reports, and more: MoltNet never lets a site
  // hold more than Incognito would there.
  for (int64_t memory : {4 * kGiB, 8 * kGiB, 16 * kGiB, 64 * kGiB}) {
    EXPECT_LE(kMoltNetQuotaPoolBytes, IncognitoPoolLow(memory));
    EXPECT_LE(kMoltNetQuotaPerSiteBytes, IncognitoPoolLow(memory) / 3);
    EXPECT_LE(static_cast<int64_t>(kMoltNetBlobMemoryBytes), memory / 5);
  }
  // Only machines below 4 GB get more in MoltNet than in Incognito (on 2
  // GB, Incognito's pool is at most 410 MiB).
  EXPECT_GT(kMoltNetQuotaPoolBytes, IncognitoPoolHigh(2 * kGiB));
}

TEST(MoltNetStorageValuesTest, BlobMemory) {
  EXPECT_EQ(kMoltNetBlobMemoryBytes, uint64_t{800} * 1024 * 1024);
  // Below Chromium's 64-bit desktop value, above its paging granularity
  // (storage::kDefaultMinPageFileSize, 5 MiB).
  EXPECT_LT(kMoltNetBlobMemoryBytes, uint64_t{2} * 1024 * 1024 * 1024);
  EXPECT_GT(kMoltNetBlobMemoryBytes, uint64_t{5} * 1024 * 1024);
  // Blobs and storage together stay within half the reference machine.
  EXPECT_LE(static_cast<int64_t>(kMoltNetBlobMemoryBytes) +
                kMoltNetQuotaPoolBytes,
            kMoltNetStorageReferenceMemory / 2);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
