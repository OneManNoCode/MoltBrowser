// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage / sessionStorage caches per site
// (components/services/storage/dom_storage/molt_dom_storage_site_policy.h,
// std-only).

#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace storage {
namespace {

constexpr size_t kMiB = 1024 * 1024;

// A site's unused areas are purged when its own areas pass either per-site
// limit, and only if it has unused areas.
TEST(MoltNetDomStorageSitePolicyTest, PurgesBySiteLimits) {
  EXPECT_FALSE(MoltDomStoragePurgesSite(0, 0, 0));
  EXPECT_FALSE(MoltDomStoragePurgesSite(kMoltDomStorageAreasPerSite, 3,
                                        kMoltDomStorageCachePerSiteBytes));
  EXPECT_TRUE(MoltDomStoragePurgesSite(kMoltDomStorageAreasPerSite + 1, 1, 0));
  EXPECT_TRUE(MoltDomStoragePurgesSite(1, 1,
                                       kMoltDomStorageCachePerSiteBytes + 1));
  // Nothing unused: nothing to purge, however full.
  EXPECT_FALSE(MoltDomStoragePurgesSite(100, 0, 100 * kMiB));
}

// A quarter of Chromium's desktop limits for a whole partition.
TEST(MoltNetDomStorageSitePolicyTest, Limits) {
  EXPECT_EQ(kMoltDomStorageAreasPerSite * 4, 48u);
  EXPECT_EQ(kMoltDomStorageCachePerSiteBytes * 4, 20 * kMiB);
}

// Unused areas leave the cache after 30 minutes unused, checked every
// minute.
TEST(MoltNetDomStorageSitePolicyTest, IdleExpiry) {
  EXPECT_FALSE(MoltDomStorageAreaExpired(0));
  EXPECT_FALSE(MoltDomStorageAreaExpired(kMoltDomStorageIdleExpirySeconds - 1));
  EXPECT_TRUE(MoltDomStorageAreaExpired(kMoltDomStorageIdleExpirySeconds));
  EXPECT_EQ(kMoltDomStorageIdleExpirySeconds, 1800);
  EXPECT_EQ(kMoltDomStorageSweepIntervalSeconds, 60);
}

}  // namespace
}  // namespace storage
