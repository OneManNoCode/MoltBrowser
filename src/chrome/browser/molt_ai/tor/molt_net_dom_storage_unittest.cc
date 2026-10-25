// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage caches and totals per site, with the real storage
// service implementation (LocalStorageImpl, in memory as for an
// off-the-record partition): what one site opens never purges another
// site's areas, and a site's areas together hold one area's limit.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/stringprintf.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"

namespace molt_ai::tor {
namespace {

blink::StorageKey Key(const std::string& origin) {
  return blink::StorageKey::CreateFromStringForTesting(origin);
}

class MoltNetDomStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    // An empty path: an in-memory database, as for MoltNet's partition.
    storage_ = std::make_unique<storage::LocalStorageImpl>(
        base::FilePath(), base::NullCallback(), mojo::NullReceiver());
  }
  void TearDown() override {
    storage_.reset();
    task_environment_.RunUntilIdle();
  }

  // Opens `key`'s area, writes to it, and leaves it unused.
  void OpenAndLeave(const blink::StorageKey& key) {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
    base::test::TestFuture<bool> success;
    area->Put({'k'}, {'v'}, std::nullopt, /*source=*/nullptr,
              success.GetCallback());
    EXPECT_TRUE(success.Take());
    area.reset();
    task_environment_.RunUntilIdle();
  }

  // Writes `bytes` under one key of `key`'s area; whether it was stored.
  bool Write(const blink::StorageKey& key, size_t bytes) {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
    base::test::TestFuture<bool> success;
    area->Put({'k'}, std::vector<uint8_t>(bytes, 'v'), std::nullopt,
              /*source=*/nullptr, success.GetCallback());
    const bool stored = success.Take();
    area.reset();
    task_environment_.RunUntilIdle();
    return stored;
  }

  bool Cached(const blink::StorageKey& key) {
    return storage_->GetStorageAreaForTesting(key) != nullptr;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  std::unique_ptr<storage::LocalStorageImpl> storage_;
};

// Chromium 156: a site that opens many areas purges every other site's
// unused ones.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteOneSitePurgesOthers) {
  const blink::StorageKey polite = Key("https://polite.test");
  OpenAndLeave(polite);
  ASSERT_TRUE(Cached(polite));
  for (int i = 0; i < 60; ++i) {
    OpenAndLeave(Key(base::StringPrintf("https://s%d.greedy.test", i)));
  }
  EXPECT_FALSE(Cached(polite));
}

// With caches per site, however many areas another site opens, a site's
// unused area stays cached.
TEST_F(MoltNetDomStorageTest, OtherSitesNeverPurgeASitesAreas) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey polite = Key("https://polite.test");
  OpenAndLeave(polite);
  for (int i = 0; i < 100; ++i) {
    OpenAndLeave(Key(base::StringPrintf("https://s%d.greedy.test", i)));
    OpenAndLeave(Key(base::StringPrintf("https://site%d.test", i)));
  }
  EXPECT_TRUE(Cached(polite));
  // Nor does a purge for memory.
  storage_->PurgeMemory();
  EXPECT_TRUE(Cached(polite));
}

// A site past its own limits purges its own unused areas, and only those.
TEST_F(MoltNetDomStorageTest, ASitePurgesItsOwn) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey other = Key("https://other.test");
  OpenAndLeave(other);
  const blink::StorageKey first = Key("https://s0.greedy.test");
  for (size_t i = 0; i <= storage::kMoltDomStorageAreasPerSite; ++i) {
    OpenAndLeave(Key(base::StringPrintf("https://s%zu.greedy.test", i)));
  }
  // At the limit plus one, nothing purged yet: the next open purges.
  EXPECT_TRUE(Cached(first));
  OpenAndLeave(Key("https://next.greedy.test"));
  EXPECT_FALSE(Cached(first));
  EXPECT_TRUE(Cached(Key("https://next.greedy.test")));
  EXPECT_TRUE(Cached(other));
}

// An unused area leaves the cache after 30 minutes unused, whatever other
// sites do; one a page has open never does.
TEST_F(MoltNetDomStorageTest, IdleAreasExpire) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey idle = Key("https://idle.test");
  const blink::StorageKey open = Key("https://open.test");
  mojo::Remote<blink::mojom::StorageArea> kept;
  storage_->BindStorageArea(open, kept.BindNewPipeAndPassReceiver());
  OpenAndLeave(idle);
  task_environment_.FastForwardBy(
      base::Seconds(storage::kMoltDomStorageIdleExpirySeconds) -
      base::Minutes(1));
  EXPECT_TRUE(Cached(idle));
  task_environment_.FastForwardBy(base::Minutes(2));
  EXPECT_FALSE(Cached(idle));
  task_environment_.FastForwardBy(base::Hours(3));
  EXPECT_TRUE(Cached(open));
}

// With caches per site, a site's subdomains share one area's limit; another
// site keeps all of its own, and deleting an area gives its bytes back.
TEST_F(MoltNetDomStorageTest, ASitesAreasShareOneTotal) {
  storage_->MoltKeepCachesPerSite();
  constexpr size_t kPart = 3 * 1024 * 1024;
  int stored = 0;
  for (int i = 0; i < 10; ++i) {
    stored += Write(Key(base::StringPrintf("https://s%d.greedy.test", i)),
                    kPart)
                  ? 1
                  : 0;
  }
  EXPECT_EQ(stored, 3);
  EXPECT_TRUE(Write(Key("https://other.test"), 10 * 1024 * 1024));

  base::test::TestFuture<void> deleted;
  storage_->DeleteStorage(Key("https://s0.greedy.test"), deleted.GetCallback());
  ASSERT_TRUE(deleted.Wait());
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(Write(Key("https://s9.greedy.test"), kPart));
}

// Chromium 156: every subdomain has its own 10 MiB.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteEachAreaHasItsOwn) {
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(Write(Key(base::StringPrintf("https://s%d.greedy.test", i)),
                      9 * 1024 * 1024));
  }
}

}  // namespace
}  // namespace molt_ai::tor
