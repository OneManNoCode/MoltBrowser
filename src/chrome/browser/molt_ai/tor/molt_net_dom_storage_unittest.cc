// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage caches and totals per site, with the real storage
// service implementation (LocalStorageImpl, in memory as for an
// off-the-record partition): what one site opens never purges another
// site's areas, and a site's areas together hold one area's limit. And
// sessionStorage's totals (SessionStorageImpl): a copy made for a tab
// counts. And, in every profile, a write the area's quota refuses leaves
// the old value in place (StorageAreaImpl::Put()).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/stringprintf.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "components/services/storage/dom_storage/db_status.h"
#include "components/services/storage/dom_storage/dom_storage_database.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "components/services/storage/dom_storage/session_storage_impl.h"
#include "components/services/storage/dom_storage/storage_area_impl.h"
#include "components/services/storage/public/mojom/session_storage_control.mojom.h"
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

// A write its site's total refuses changes nothing: the area still holds
// the value it overwrote.
TEST_F(MoltNetDomStorageTest, ARefusedWriteChangesNothing) {
  storage_->MoltKeepCachesPerSite();
  constexpr size_t kMiB = 1024 * 1024;
  const blink::StorageKey key = Key("https://x.a.test");
  ASSERT_TRUE(Write(key, 6 * kMiB));
  ASSERT_TRUE(Write(Key("https://y.a.test"), 4 * kMiB));
  EXPECT_FALSE(Write(key, 7 * kMiB));

  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
  area->GetAll(mojo::NullRemote(), all.GetCallback());
  const std::vector<blink::mojom::KeyValuePtr> data = all.Take();
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value.size(), 6 * kMiB);
}

// Every profile: a write the area's own quota refuses changes nothing; the
// value it would have replaced stays readable. (Chromium 156 moved the old
// value out of the area's map before the quota check and left the key
// mapped to an empty value.)
TEST_F(MoltNetDomStorageTest, AQuotaRefusedWriteKeepsTheOldValue) {
  constexpr size_t kMiB = 1024 * 1024;
  const blink::StorageKey key = Key("https://a.test");
  ASSERT_TRUE(Write(key, 6 * kMiB));
  // Past the area's 10 MiB and 100 KiB, whatever the renderer checked.
  EXPECT_FALSE(Write(key, 11 * kMiB));

  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
  area->GetAll(mojo::NullRemote(), all.GetCallback());
  const std::vector<blink::mojom::KeyValuePtr> data = all.Take();
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value, std::vector<uint8_t>(6 * kMiB, 'v'));
}

// The same for a storage area on its own (as sessionStorage's maps use
// it), with a small quota.
class NoCommitsDelegate : public storage::StorageAreaImpl::Delegate {
 public:
  void OnNoBindings() override {}
  void DidCommit(storage::DbStatus) override {}
};

TEST(MoltStorageAreaPutTest, AQuotaRefusedPutKeepsTheOldValue) {
  base::test::TaskEnvironment task_environment;
  NoCommitsDelegate delegate;
  storage::StorageAreaImpl::Options options;
  options.max_size = 100;
  storage::StorageAreaImpl area(
      /*database=*/nullptr,
      base::MakeRefCounted<storage::DomStorageDatabase::SharedMapLocator>(
          storage::DomStorageDatabase::MapLocator(Key("https://a.test"))),
      &delegate, options);
  area.InitializeAsEmpty();
  mojo::Remote<blink::mojom::StorageArea> remote;
  area.Bind(remote.BindNewPipeAndPassReceiver());

  const std::vector<uint8_t> old_value(40, 'a');
  base::test::TestFuture<bool> stored;
  remote->Put({'k'}, old_value, std::nullopt, /*source=*/nullptr,
              stored.GetCallback());
  ASSERT_TRUE(stored.Take());
  remote->Put({'k'}, std::vector<uint8_t>(120, 'b'), old_value,
              /*source=*/nullptr, stored.GetCallback());
  EXPECT_FALSE(stored.Take());

  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
  remote->GetAll(mojo::NullRemote(), all.GetCallback());
  const std::vector<blink::mojom::KeyValuePtr> data = all.Take();
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value, old_value);
  EXPECT_EQ(area.storage_used(), 1u + 40u);
}

// Chromium 156: every subdomain has its own 10 MiB.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteEachAreaHasItsOwn) {
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(Write(Key(base::StringPrintf("https://s%d.greedy.test", i)),
                      9 * 1024 * 1024));
  }
}

class MoltNetSessionStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    storage_ = std::make_unique<storage::SessionStorageImpl>(
        base::FilePath(), storage::SessionStorageImpl::BackingMode::kNoDisk,
        base::DoNothing(), mojo::NullReceiver());
  }
  void TearDown() override {
    storage_.reset();
    task_environment_.RunUntilIdle();
  }

  std::string NewNamespace() {
    std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    storage_->CreateNamespace(id);
    return id;
  }
  std::string CloneOf(const std::string& from) {
    std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
    storage_->CloneNamespace(
        from, id, storage::mojom::SessionStorageCloneType::kImmediate);
    return id;
  }
  mojo::Remote<blink::mojom::StorageArea> Area(const std::string& ns,
                                               const blink::StorageKey& key) {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(key, ns, area.BindNewPipeAndPassReceiver());
    return area;
  }
  static bool Put(blink::mojom::StorageArea* area, uint8_t key, size_t bytes) {
    base::test::TestFuture<bool> success;
    area->Put({key}, std::vector<uint8_t>(bytes, 'v'), std::nullopt,
              /*source=*/nullptr, success.GetCallback());
    return success.Take();
  }
  static void Delete(blink::mojom::StorageArea* area, uint8_t key) {
    base::test::TestFuture<void> done;
    area->Delete({key}, std::nullopt, /*source=*/nullptr, done.GetCallback());
    EXPECT_TRUE(done.Wait());
  }

  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<storage::SessionStorageImpl> storage_;
};

// A tab's sessionStorage, copied into a tab it opened, counts again once
// the copy is changed, also by a change that doesn't grow it.
TEST_F(MoltNetSessionStorageTest, ACopyCountsOnceChanged) {
  constexpr size_t kMiB = 1024 * 1024;
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey site = Key("https://a.test");
  const blink::StorageKey sub = Key("https://x.a.test");
  const std::string first = NewNamespace();
  mojo::Remote<blink::mojom::StorageArea> area = Area(first, site);
  ASSERT_TRUE(Put(area.get(), 'k', 4 * kMiB));
  ASSERT_TRUE(Put(area.get(), 'j', 1));

  mojo::Remote<blink::mojom::StorageArea> copy = Area(CloneOf(first), site);
  Delete(copy.get(), 'j');
  task_environment_.RunUntilIdle();

  // 4 MiB in each tab's copy: a third area of the site has 2 MiB left.
  mojo::Remote<blink::mojom::StorageArea> other = Area(first, sub);
  EXPECT_FALSE(Put(other.get(), 'k', 3 * kMiB));
  EXPECT_TRUE(Put(other.get(), 'k', 2 * kMiB));
}

// Chromium 156: the copy has its own 10 MiB, like every area.
TEST_F(MoltNetSessionStorageTest, WithoutCachesPerSiteACopyIsNotCounted) {
  constexpr size_t kMiB = 1024 * 1024;
  const blink::StorageKey site = Key("https://a.test");
  const std::string first = NewNamespace();
  mojo::Remote<blink::mojom::StorageArea> area = Area(first, site);
  ASSERT_TRUE(Put(area.get(), 'k', 9 * kMiB));
  mojo::Remote<blink::mojom::StorageArea> copy = Area(CloneOf(first), site);
  EXPECT_TRUE(Put(copy.get(), 'j', kMiB));
}

}  // namespace
}  // namespace molt_ai::tor
