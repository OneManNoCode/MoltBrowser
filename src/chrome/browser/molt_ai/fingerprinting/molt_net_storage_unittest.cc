// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage.h"

#include <optional>

#include "base/time/time.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_storage_values.h"
#include "storage/browser/blob/blob_storage_constants.h"
#include "storage/browser/quota/quota_settings.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::fingerprinting {
namespace {

TEST(MoltNetStorageTest, OtherProfilesKeepChromiumsLimits) {
  for (const RendererMode mode : {RendererMode::kOff, RendererMode::kFarble}) {
    EXPECT_FALSE(MoltNetQuotaSettings(mode).has_value());
    EXPECT_FALSE(MoltNetBlobStorageLimits(mode).has_value());
  }
}

TEST(MoltNetStorageTest, QuotaSettings) {
  const std::optional<storage::QuotaSettings> settings =
      MoltNetQuotaSettings(RendererMode::kUniform);
  ASSERT_TRUE(settings.has_value());
  EXPECT_EQ(settings->pool_size, kMoltNetQuotaPoolBytes);
  EXPECT_EQ(settings->per_storage_key_quota, kMoltNetQuotaPerStorageKeyBytes);
  EXPECT_EQ(settings->session_only_per_storage_key_quota,
            kMoltNetQuotaPerStorageKeyBytes);
  EXPECT_LE(settings->per_storage_key_quota, settings->pool_size);
  EXPECT_EQ(settings->should_remain_available, 0);
  EXPECT_EQ(settings->must_remain_available, 0);
  EXPECT_TRUE(settings->refresh_interval.is_max());
}

TEST(MoltNetStorageTest, BlobLimits) {
  const std::optional<storage::BlobStorageLimits> limits =
      MoltNetBlobStorageLimits(RendererMode::kUniform);
  ASSERT_TRUE(limits.has_value());
  EXPECT_TRUE(limits->IsValid());
  EXPECT_EQ(limits->max_blob_in_memory_space, kMoltNetBlobMemoryBytes);
  EXPECT_EQ(limits->desired_max_disk_space, 0u);
  EXPECT_EQ(limits->effective_max_disk_space, 0u);
  // Everything else is Chromium's (the same on every machine).
  const storage::BlobStorageLimits defaults;
  EXPECT_EQ(limits->max_ipc_memory_size, defaults.max_ipc_memory_size);
  EXPECT_EQ(limits->max_shared_memory_size, defaults.max_shared_memory_size);
  EXPECT_EQ(limits->max_bytes_data_item_size,
            defaults.max_bytes_data_item_size);
  EXPECT_EQ(limits->min_page_file_size, defaults.min_page_file_size);
  EXPECT_EQ(limits->max_file_size, defaults.max_file_size);
  EXPECT_EQ(limits->max_blob_in_memory_space_under_pressure_ratio,
            defaults.max_blob_in_memory_space_under_pressure_ratio);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
