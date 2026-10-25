// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What the browser holds for a site outside any blob (a Background Fetch's
// response while the in-memory download service downloads it) counts
// against that site's blob share and toward the total the memory valve
// watches (storage/browser/blob/molt_blob_budget_policy.h:
// MoltBlobHoldFor()), on the real BlobStorageContext. The download itself:
// components/download/internal/background_service/
// in_memory_download_unittest.cc (InMemoryDownloadTest.MoltNet*).

#include <memory>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "storage/browser/blob/blob_storage_constants.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/blob/molt_blob_budget_policy.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace storage {
namespace {

constexpr size_t kKiB = 1024;
constexpr uint64_t kSiteA = kMoltBlobSiteKeyBit | 1;
constexpr uint64_t kSiteB = kMoltBlobSiteKeyBit | 2;
constexpr uint64_t kSiteC = kMoltBlobSiteKeyBit | 3;

class MoltNetBackgroundFetchBufferTest : public testing::Test {
 protected:
  // MoltNet's shape, scaled down: a share of 64 KiB per key, a floor of
  // 128 KiB for all.
  void UseShares() {
    BlobStorageLimits limits;
    limits.max_ipc_memory_size = 16 * kKiB;
    limits.max_shared_memory_size = 16 * kKiB;
    limits.max_bytes_data_item_size = 16 * kKiB;
    limits.max_blob_in_memory_space = 128 * kKiB;
    limits.desired_max_disk_space = 0;
    limits.effective_max_disk_space = 0;
    limits.min_page_file_size = 4 * kKiB;
    limits.max_file_size = 16 * kKiB;
    limits.molt_per_key_memory = 64 * kKiB;
    limits.molt_on_growth_beyond_floor =
        base::BindRepeating([](int* told) { ++*told; }, &valve_told_);
    context_.set_limits_for_testing(limits);
  }

  size_t Used(uint64_t key) {
    return context_.memory_controller().MoltMemoryUsage(key);
  }
  size_t Total() { return context_.memory_controller().memory_usage(); }

  base::test::TaskEnvironment task_environment_;
  BlobStorageContext context_;
  int valve_told_ = 0;
};

// Off MoltNet nothing is counted, as in Chromium.
TEST_F(MoltNetBackgroundFetchBufferTest, NotCountedWithoutShares) {
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 40 * kKiB),
            MoltBlobHold::kNotCounted);
  EXPECT_EQ(Total(), 0u);
}

// A buffer counts against its site's share, and only its own: past the
// share a part is refused, and nothing of it is counted; another site keeps
// all of its own.
TEST_F(MoltNetBackgroundFetchBufferTest, CountsAgainstTheSitesShare) {
  UseShares();
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 40 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 20 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(Used(kSiteA), 60 * kKiB);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 8 * kKiB), MoltBlobHold::kRefused);
  EXPECT_EQ(Used(kSiteA), 60 * kKiB);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteB, 64 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(Total(), 124 * kKiB);

  context_.MoltReleaseHeldMemory(kSiteA, 60 * kKiB);
  EXPECT_EQ(Used(kSiteA), 0u);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 64 * kKiB), MoltBlobHold::kCounted);
  context_.MoltReleaseHeldMemory(kSiteA, 64 * kKiB);
  context_.MoltReleaseHeldMemory(kSiteB, 64 * kKiB);
  EXPECT_EQ(Total(), 0u);
  // Releasing more than is held takes nothing from anyone else.
  EXPECT_EQ(context_.MoltHoldMemory(kSiteB, 4 * kKiB), MoltBlobHold::kCounted);
  context_.MoltReleaseHeldMemory(kSiteA, 64 * kKiB);
  EXPECT_EQ(Used(kSiteB), 4 * kKiB);
}

// Past the floor of all keys together the valve hears of buffers, as of
// blobs.
TEST_F(MoltNetBackgroundFetchBufferTest, TheValveSeesBuffers) {
  UseShares();
  EXPECT_EQ(context_.MoltHoldMemory(kSiteA, 64 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteB, 64 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(valve_told_, 0);
  EXPECT_EQ(context_.MoltHoldMemory(kSiteC, 4 * kKiB), MoltBlobHold::kCounted);
  EXPECT_EQ(valve_told_, 1);
}

}  // namespace
}  // namespace storage
