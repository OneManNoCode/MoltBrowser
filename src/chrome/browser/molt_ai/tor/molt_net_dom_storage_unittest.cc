// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's localStorage caches and totals per site, with the real storage
// service implementation (LocalStorageImpl, in memory as for an
// off-the-record partition): what one site opens never purges another
// site's areas, and a site's areas together hold one area's limit; the
// partition keeps no database, so an area holding items is never purged;
// a site's writes past its write rate fail, and another site's don't; a
// site flooding its areas with messages doesn't make another site's read
// wait for them. And sessionStorage's (SessionStorageImpl): a copy made for
// a tab counts, no database, the write rate, the flood. And, in every
// profile, a write the area's quota refuses leaves the old value in place
// (StorageAreaImpl::Put()).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "components/services/storage/dom_storage/db_status.h"
#include "components/services/storage/dom_storage/dom_storage_database.h"
#include "components/services/storage/dom_storage/local_storage_impl.h"
#include "components/services/storage/dom_storage/molt_dom_storage_own_thread.h"
#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "components/services/storage/dom_storage/session_storage_impl.h"
#include "components/services/storage/dom_storage/session_storage_namespace_impl.h"
#include "components/services/storage/dom_storage/storage_area_impl.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/session_storage_control.mojom.h"
#include "components/services/storage/public/mojom/storage_usage_info.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::tor {
namespace {

blink::StorageKey Key(const std::string& origin) {
  return blink::StorageKey::CreateFromStringForTesting(origin);
}

// A flood a page can send its storage areas: 1500 writes of a growing value
// to `writes` (past its site's write allowance, the last 500 are refused)
// and as many clear()s of `clears`. Then, in a task queued after the first
// of them was read, a read of `polite`; once it is answered, `handled()`
// tells how many of the writes the storage service had handled.
size_t FloodThenRead(blink::mojom::StorageArea* writes,
                     blink::mojom::StorageArea* clears,
                     blink::mojom::StorageArea* polite,
                     base::RepeatingCallback<size_t()> handled) {
  for (size_t i = 1; i <= 1500; ++i) {
    writes->Put({'k'}, std::vector<uint8_t>(i, 'v'), std::nullopt,
                /*source=*/nullptr, base::DoNothing());
    clears->DeleteAll(/*source=*/nullptr, mojo::NullRemote(),
                      base::DoNothing());
  }
  size_t handled_at_read = 0;
  base::RunLoop loop;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&] {
        polite->GetAll(
            mojo::NullRemote(),
            base::BindLambdaForTesting(
                [&](std::vector<blink::mojom::KeyValuePtr>) {
                  handled_at_read = handled.Run();
                  loop.Quit();
                }));
      }));
  loop.Run();
  return handled_at_read;
}

// An area's observer (a renderer process with the area open), recording
// what it hears.
class RecordingObserver : public blink::mojom::StorageAreaObserver {
 public:
  mojo::PendingRemote<blink::mojom::StorageAreaObserver> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  const std::vector<std::string>& heard() const { return heard_; }

  // blink::mojom::StorageAreaObserver:
  void KeyChanged(const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& new_value,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override {
    heard_.push_back(base::StringPrintf(
        "changed %zu", new_value.size()));
  }
  void KeyChangeFailed(const std::vector<uint8_t>& key,
                       blink::mojom::StorageAreaSourcePtr source) override {
    heard_.push_back("failed");
  }
  void KeyDeleted(const std::vector<uint8_t>& key,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override {
    heard_.push_back(base::StringPrintf(
        "deleted %zu", old_value ? old_value->size() : 0));
  }
  void AllDeleted(bool was_nonempty,
                  blink::mojom::StorageAreaSourcePtr source) override {
    heard_.push_back(was_nonempty ? "cleared" : "cleared empty");
  }
  void ShouldSendOldValueOnMutations(bool value) override {}

 private:
  std::vector<std::string> heard_;
  mojo::Receiver<blink::mojom::StorageAreaObserver> receiver_{this};
};

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

  // A partition on disk instead, which keeps a database also per site.
  void UseDiskPartition() {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    storage_ = std::make_unique<storage::LocalStorageImpl>(
        temp_dir_.GetPath(), base::NullCallback(), mojo::NullReceiver());
  }

  // Opens `key`'s area and leaves it unused, holding nothing.
  void OpenAndLeave(const blink::StorageKey& key) {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
    area.FlushForTesting();
    area.reset();
    task_environment_.RunUntilIdle();
  }

  // Writes `bytes` under the key `name` of an open area; whether it was
  // stored.
  static bool Put(blink::mojom::StorageArea* area,
                  uint8_t name,
                  size_t bytes) {
    base::test::TestFuture<bool> success;
    area->Put({name}, std::vector<uint8_t>(bytes, 'v'), std::nullopt,
              /*source=*/nullptr, success.GetCallback());
    return success.Take();
  }

  // What `key`'s area holds.
  std::vector<blink::mojom::KeyValuePtr> ReadAll(const blink::StorageKey& key) {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
    base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
    area->GetAll(mojo::NullRemote(), all.GetCallback());
    return all.Take();
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

  // How many writes of a flood (FloodThenRead()) the storage service had
  // handled when another site's read was answered.
  size_t FloodedWritesBeforeAPoliteRead() {
    const blink::StorageKey flooded = Key("https://flood.test");
    mojo::Remote<blink::mojom::StorageArea> writes, clears, polite;
    storage_->BindStorageArea(flooded, writes.BindNewPipeAndPassReceiver());
    storage_->BindStorageArea(Key("https://x.flood.test"),
                              clears.BindNewPipeAndPassReceiver());
    storage_->BindStorageArea(Key("https://polite.test"),
                              polite.BindNewPipeAndPassReceiver());
    writes.FlushForTesting();
    clears.FlushForTesting();
    polite.FlushForTesting();
    return FloodThenRead(
        writes.get(), clears.get(), polite.get(),
        base::BindLambdaForTesting([&]() -> size_t {
          // The last value handled is as long as its write's number.
          const size_t used =
              storage_->GetStorageAreaForTesting(flooded)->storage_used();
          return used > 0 ? used - 1 : 0;
        }));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_dir_;
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
// unused area stays cached (one holding nothing goes as it is left, whatever
// other sites do: WithoutADatabaseAnUnusedEmptyAreaGoesAtOnce).
TEST_F(MoltNetDomStorageTest, OtherSitesNeverPurgeASitesAreas) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey polite = Key("https://polite.test");
  ASSERT_TRUE(Write(polite, 10));
  for (int i = 0; i < 100; ++i) {
    OpenAndLeave(Key(base::StringPrintf("https://s%d.greedy.test", i)));
    OpenAndLeave(Key(base::StringPrintf("https://site%d.test", i)));
  }
  EXPECT_TRUE(Cached(polite));
  // Nor does a purge for memory.
  storage_->PurgeMemory();
  EXPECT_TRUE(Cached(polite));
}

// With a database (on disk), a site past its own limits purges its own
// unused areas, and only those.
TEST_F(MoltNetDomStorageTest, ASitePurgesItsOwn) {
  UseDiskPartition();
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

// With a database (on disk), an unused area leaves the cache after 30
// minutes unused, whatever other sites do; one a page has open never does.
TEST_F(MoltNetDomStorageTest, IdleAreasExpire) {
  UseDiskPartition();
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

// MoltNet's partition keeps no database: opening an area waits on nothing
// another site queued. So an area holding items (also one of zero bytes)
// stays in the cache, past its site's limits and however long unused, and
// keeps them; one holding nothing still goes. What the partition stores is
// what its areas hold.
TEST_F(MoltNetDomStorageTest, WithoutADatabaseAreasHoldingItemsStay) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey kept = Key("https://kept.a.test");
  const blink::StorageKey zero = Key("https://zero.a.test");
  const blink::StorageKey empty = Key("https://empty.a.test");
  ASSERT_TRUE(Write(kept, 10));
  EXPECT_EQ(storage_->GetDatabaseForTesting(), nullptr);
  {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(zero, area.BindNewPipeAndPassReceiver());
    base::test::TestFuture<bool> success;
    area->Put({}, {}, std::nullopt, /*source=*/nullptr, success.GetCallback());
    ASSERT_TRUE(success.Take());
  }
  OpenAndLeave(empty);
  for (size_t i = 0; i <= storage::kMoltDomStorageAreasPerSite + 1; ++i) {
    OpenAndLeave(Key(base::StringPrintf("https://s%zu.a.test", i)));
  }
  EXPECT_FALSE(Cached(empty));
  task_environment_.FastForwardBy(base::Hours(3));
  EXPECT_TRUE(Cached(kept));
  EXPECT_TRUE(Cached(zero));
  const std::vector<blink::mojom::KeyValuePtr> data = ReadAll(kept);
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value, std::vector<uint8_t>(10, 'v'));
  EXPECT_EQ(ReadAll(zero).size(), 1u);

  base::test::TestFuture<std::vector<storage::mojom::StorageUsageInfoPtr>>
      usage;
  storage_->GetUsage(usage.GetCallback());
  const std::vector<storage::mojom::StorageUsageInfoPtr> infos = usage.Take();
  ASSERT_EQ(infos.size(), 2u);
  for (const storage::mojom::StorageUsageInfoPtr& info : infos) {
    EXPECT_EQ(info->total_size_bytes, info->storage_key == kept ? 11 : 0);
  }
}

// Without a database (MoltNet's partition), an area left unused that holds
// nothing goes at once, in its site's turn: no purge when another area
// opens, no sweep. One a page has open, or one holding an item, stays; one
// holding items goes once they are deleted and it is unused.
TEST_F(MoltNetDomStorageTest, WithoutADatabaseAnUnusedEmptyAreaGoesAtOnce) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey left = Key("https://left.test");
  OpenAndLeave(left);
  EXPECT_FALSE(Cached(left));
  const blink::StorageKey open = Key("https://open.test");
  mojo::Remote<blink::mojom::StorageArea> kept_open;
  storage_->BindStorageArea(open, kept_open.BindNewPipeAndPassReceiver());
  kept_open.FlushForTesting();
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(Cached(open));
  const blink::StorageKey cleared = Key("https://cleared.test");
  {
    mojo::Remote<blink::mojom::StorageArea> area;
    storage_->BindStorageArea(cleared, area.BindNewPipeAndPassReceiver());
    ASSERT_TRUE(Put(area.get(), 'k', 10));
    base::test::TestFuture<void> done;
    area->DeleteAll(/*source=*/nullptr, mojo::NullRemote(),
                    done.GetCallback());
    ASSERT_TRUE(done.Wait());
  }
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(Cached(cleared));
  const blink::StorageKey held = Key("https://held.test");
  ASSERT_TRUE(Write(held, 10));
  task_environment_.FastForwardBy(base::Hours(3));
  EXPECT_TRUE(Cached(held));
  base::test::TestFuture<void> deleted;
  storage_->DeleteStorage(held, deleted.GetCallback());
  ASSERT_TRUE(deleted.Wait());
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(Cached(held));
}

// The same for sessionStorage: an area left unbound that holds nothing goes
// from its namespace at once; one holding an item stays.
TEST_F(MoltNetDomStorageTest, SessionStorageWithoutADatabaseDropsEmptyAreas) {
  storage::SessionStorageImpl session(
      base::FilePath(), storage::SessionStorageImpl::BackingMode::kNoDisk,
      base::DoNothing(), mojo::NullReceiver());
  session.MoltKeepCachesPerSite();
  const std::string ns = base::Uuid::GenerateRandomV4().AsLowercaseString();
  session.CreateNamespace(ns);
  const blink::StorageKey empty = Key("https://empty.test");
  const blink::StorageKey held = Key("https://held.test");
  {
    mojo::Remote<blink::mojom::StorageArea> a, b;
    session.BindStorageArea(empty, ns, a.BindNewPipeAndPassReceiver());
    session.BindStorageArea(held, ns, b.BindNewPipeAndPassReceiver());
    a.FlushForTesting();
    ASSERT_TRUE(Put(b.get(), 'k', 10));
  }
  task_environment_.RunUntilIdle();
  storage::SessionStorageNamespaceImpl* namespace_impl =
      session.GetNamespaceForTesting(ns);
  ASSERT_TRUE(namespace_impl);
  EXPECT_FALSE(namespace_impl->HasAreaForStorageKeyForTesting(empty));
  EXPECT_TRUE(namespace_impl->HasAreaForStorageKeyForTesting(held));
}

// A partition kept per site on a thread of its own
// (molt_dom_storage_own_thread.h): its control pipe takes a turn of its own,
// and each BindStorageArea() it carries is handled in its site's turn. A
// page making the browser bind areas in a loop (here 1000 subdomains of
// one site) takes its own site's turns: another site's read, sent after
// them, waits for a few of them, not for all.
TEST_F(MoltNetDomStorageTest, BindingAreasInALoopDoesntDelayAnotherSite) {
  mojo::Remote<storage::mojom::LocalStorageControl> control;
  storage::MoltPerSiteLocalStorage host(control.BindNewPipeAndPassReceiver(),
                                        base::DoNothing());
  const blink::StorageKey polite = Key("https://polite.test");
  mojo::Remote<blink::mojom::StorageArea> polite_area;
  control->BindStorageArea(polite, polite_area.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(Put(polite_area.get(), 'k', 10));
  std::vector<mojo::Remote<blink::mojom::StorageArea>> greedy(1000);
  for (size_t i = 0; i < greedy.size(); ++i) {
    control->BindStorageArea(
        Key(base::StringPrintf("https://s%zu.greedy.test", i)),
        greedy[i].BindNewPipeAndPassReceiver());
  }
  size_t bound_at_read = 0;
  base::RunLoop loop;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&] {
        polite_area->GetAll(
            mojo::NullRemote(),
            base::BindLambdaForTesting(
                [&](std::vector<blink::mojom::KeyValuePtr> all) {
                  EXPECT_EQ(all.size(), 1u);
                  for (size_t i = 0; i < greedy.size(); ++i) {
                    if (host.storage_for_testing()->GetStorageAreaForTesting(
                            Key(base::StringPrintf("https://s%zu.greedy.test",
                                                   i)))) {
                      ++bound_at_read;
                    }
                  }
                  loop.Quit();
                }));
      }));
  loop.Run();
  EXPECT_LT(bound_at_read, 20u);
  // They are all bound in the end.
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(host.storage_for_testing()->GetStorageAreaForTesting(
      Key("https://s999.greedy.test")));
}

// The storage service's per-site partitions: each on a thread of its own,
// answering through its control pipe as a partition does, and gone with
// its pipe.
TEST_F(MoltNetDomStorageTest, PerSitePartitionsRunOnThreadsOfTheirOwn) {
  storage::MoltDomStorageOwnThreads threads;
  mojo::Remote<storage::mojom::LocalStorageControl> local;
  mojo::Remote<storage::mojom::SessionStorageControl> session;
  threads.BindLocalStorageControl(local.BindNewPipeAndPassReceiver());
  threads.BindSessionStorageControl(session.BindNewPipeAndPassReceiver());
  EXPECT_EQ(threads.CountForTesting(), 2u);

  const blink::StorageKey key = Key("https://a.test");
  mojo::Remote<blink::mojom::StorageArea> area;
  local->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(Put(area.get(), 'k', 10));
  base::test::TestFuture<std::vector<storage::mojom::StorageUsageInfoPtr>>
      usage;
  local->GetUsage(usage.GetCallback());
  EXPECT_EQ(usage.Take().size(), 1u);

  const std::string ns = base::Uuid::GenerateRandomV4().AsLowercaseString();
  session->CreateNamespace(ns);
  mojo::Remote<blink::mojom::StorageArea> session_area;
  session->BindStorageArea(key, ns, session_area.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(Put(session_area.get(), 'k', 10));

  area.reset();
  session_area.reset();
  local.reset();
  session.reset();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(threads.CountForTesting(), 0u);
}

// Kept per site, an area's observers each hear of a change in a task of
// their own, in the order of the changes, and an observer gone meanwhile
// hears nothing (molt_dom_storage_split_work.h). Chromium's order and
// content, unchanged.
TEST_F(MoltNetDomStorageTest, ObserversHearOfChangesInTasksOfTheirOwn) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey key = Key("https://a.test");
  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
  std::vector<std::unique_ptr<RecordingObserver>> observers;
  for (int i = 0; i < 3; ++i) {
    observers.push_back(std::make_unique<RecordingObserver>());
    area->AddObserver(observers.back()->Bind());
  }
  area.FlushForTesting();
  ASSERT_TRUE(Put(area.get(), 'k', 10));
  ASSERT_TRUE(Put(area.get(), 'k', 20));
  {
    base::test::TestFuture<void> deleted;
    area->Delete({'k'}, std::nullopt, /*source=*/nullptr,
                 deleted.GetCallback());
    ASSERT_TRUE(deleted.Wait());
  }
  ASSERT_TRUE(Put(area.get(), 'j', 5));
  {
    base::test::TestFuture<void> cleared;
    area->DeleteAll(/*source=*/nullptr, mojo::NullRemote(),
                    cleared.GetCallback());
    ASSERT_TRUE(cleared.Wait());
  }
  task_environment_.RunUntilIdle();
  const std::vector<std::string> expected = {
      "changed 10", "changed 20", "deleted 20", "changed 5", "cleared"};
  for (const auto& observer : observers) {
    EXPECT_EQ(observer->heard(), expected);
  }
  // An observer gone before its notification hears nothing more.
  observers.pop_back();
  ASSERT_TRUE(Put(area.get(), 'k', 7));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(observers[0]->heard().back(), "changed 7");
}

// A clear() of an area holding many items frees them a chunk a task; the
// area is empty at once, and its site's total with it.
TEST_F(MoltNetDomStorageTest, AClearOfManyItemsIsFreedInChunks) {
  storage_->MoltKeepCachesPerSite();
  const blink::StorageKey key = Key("https://a.test");
  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(key, area.BindNewPipeAndPassReceiver());
  for (int i = 0; i < 900; ++i) {
    base::test::TestFuture<bool> stored;
    area->Put({static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8)},
              std::vector<uint8_t>(2048, 'v'), std::nullopt,
              /*source=*/nullptr, stored.GetCallback());
    ASSERT_TRUE(stored.Take());
  }
  base::test::TestFuture<void> cleared;
  area->DeleteAll(/*source=*/nullptr, mojo::NullRemote(),
                  cleared.GetCallback());
  ASSERT_TRUE(cleared.Wait());
  EXPECT_EQ(storage_->GetStorageAreaForTesting(key)->storage_used(), 0u);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(ReadAll(key).empty());
  // The site's whole total is free again (once its write allowance has
  // refilled).
  task_environment_.FastForwardBy(base::Seconds(20));
  EXPECT_TRUE(Write(key, 10 * 1024 * 1024));
}

// Chromium 156: an in-memory partition keeps its areas' items in a database
// too.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteThereIsADatabase) {
  ASSERT_TRUE(Write(Key("https://a.test"), 10));
  EXPECT_NE(storage_->GetDatabaseForTesting(), nullptr);
}

// A site's writes come out of its write allowance: past it they fail, as a
// write past the area's quota does, while another site's writes at the
// same time all go; the allowance refills with time.
TEST_F(MoltNetDomStorageTest, AGreedySitesWritesDontDelayAnothers) {
  storage_->MoltKeepCachesPerSite();
  mojo::Remote<blink::mojom::StorageArea> greedy;
  storage_->BindStorageArea(Key("https://greedy.test"),
                            greedy.BindNewPipeAndPassReceiver());
  mojo::Remote<blink::mojom::StorageArea> polite;
  storage_->BindStorageArea(Key("https://polite.test"),
                            polite.BindNewPipeAndPassReceiver());
  for (int64_t i = 0; i < storage::kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(Put(greedy.get(), static_cast<uint8_t>(i), 1));
  }
  EXPECT_FALSE(Put(greedy.get(), 'x', 1));
  // A subdomain is the same site.
  mojo::Remote<blink::mojom::StorageArea> sub;
  storage_->BindStorageArea(Key("https://sub.greedy.test"),
                            sub.BindNewPipeAndPassReceiver());
  EXPECT_FALSE(Put(sub.get(), 'x', 1));
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(Put(polite.get(), static_cast<uint8_t>(i), 1));
  }
  task_environment_.FastForwardBy(base::Seconds(1));
  for (int64_t i = 0; i < storage::kMoltDomStorageSiteWritesPerSecond; ++i) {
    EXPECT_TRUE(Put(greedy.get(), 'y', static_cast<size_t>(i % 7)));
  }
  EXPECT_FALSE(Put(greedy.get(), 'z', 1));
}

// The write allowance follows the same groups as the totals: a
// credentialless iframe's writes come out of its page's nonce's allowance,
// not out of its own site's, nor the other way round.
TEST_F(MoltNetDomStorageTest, CredentiallessFramesHaveTheirOwnAllowance) {
  storage_->MoltKeepCachesPerSite();
  const url::Origin tracker = url::Origin::Create(GURL("https://t.test"));
  mojo::Remote<blink::mojom::StorageArea> own;
  storage_->BindStorageArea(Key("https://t.test"),
                            own.BindNewPipeAndPassReceiver());
  for (int64_t i = 0; i < storage::kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(Put(own.get(), static_cast<uint8_t>(i), 1));
  }
  EXPECT_FALSE(Put(own.get(), 'x', 1));
  mojo::Remote<blink::mojom::StorageArea> framed;
  storage_->BindStorageArea(
      blink::StorageKey::CreateWithNonce(tracker,
                                         base::UnguessableToken::Create()),
      framed.BindNewPipeAndPassReceiver());
  for (int64_t i = 0; i < storage::kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(Put(framed.get(), static_cast<uint8_t>(i), 1));
  }
  EXPECT_FALSE(Put(framed.get(), 'x', 1));
}

// The storage service's sequence is taken in turn per site: however many
// messages a site has sent its areas (writes, also refused ones, and
// clear()s, to two of its areas), another site's read waits for at most
// one of them.
TEST_F(MoltNetDomStorageTest, AFloodingSiteDoesntDelayAnothersRead) {
  storage_->MoltKeepCachesPerSite();
  EXPECT_LE(FloodedWritesBeforeAPoliteRead(), 2u);
}

// Chromium 156: the read waits for every message sent before it.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteAFloodDelaysEveryRead) {
  EXPECT_EQ(FloodedWritesBeforeAPoliteRead(), 1500u);
}

// The bytes a site writes come out of its allowance too, also a value
// written over and over at the same size.
TEST_F(MoltNetDomStorageTest, ASitesWriteBytesAreLimited) {
  storage_->MoltKeepCachesPerSite();
  constexpr size_t kMiB = 1024 * 1024;
  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(Key("https://a.test"),
                            area.BindNewPipeAndPassReceiver());
  EXPECT_TRUE(Put(area.get(), 'k', 4 * kMiB));
  EXPECT_TRUE(Put(area.get(), 'k', 4 * kMiB));
  EXPECT_FALSE(Put(area.get(), 'k', 4 * kMiB));
  // What it holds is unchanged.
  const std::vector<blink::mojom::KeyValuePtr> data =
      ReadAll(Key("https://a.test"));
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value.size(), 4 * kMiB);
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_TRUE(Put(area.get(), 'k', 4 * kMiB));
}

// Chromium 156: as many writes as a page sends.
TEST_F(MoltNetDomStorageTest, WithoutCachesPerSiteWritesAreNotLimited) {
  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(Key("https://a.test"),
                            area.BindNewPipeAndPassReceiver());
  for (int64_t i = 0; i < 2 * storage::kMoltDomStorageSiteWritesBurst; ++i) {
    ASSERT_TRUE(Put(area.get(), static_cast<uint8_t>(i), 1));
  }
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

// The site's total counts what each item and area takes, not only the
// items' bytes: a site with a full area has room for a few hundred tiny
// items in another, not tens of thousands.
TEST_F(MoltNetDomStorageTest, TinyItemsCountWhatTheyTake) {
  storage_->MoltKeepCachesPerSite();
  constexpr size_t kQuota = 10 * 1024 * 1024;
  ASSERT_TRUE(Write(Key("https://a.test"), kQuota));
  const size_t room =
      storage::kMoltDomStorageBytesPerSite -
      storage::MoltDomStorageCountedBytes(/*item_bytes=*/1 + kQuota, 1);
  // Items with a key of 4 bytes and an empty value.
  const size_t fit =
      (room - storage::kMoltDomStorageAreaOverheadBytes) /
      (4 + storage::kMoltDomStorageItemOverheadBytes);
  mojo::Remote<blink::mojom::StorageArea> area;
  storage_->BindStorageArea(Key("https://x.a.test"),
                            area.BindNewPipeAndPassReceiver());
  size_t stored = 0;
  for (uint32_t i = 0; i < 2 * fit; ++i) {
    base::test::TestFuture<bool> success;
    area->Put({static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 'k',
               'k'},
              {}, std::nullopt, /*source=*/nullptr, success.GetCallback());
    if (!success.Take()) {
      break;
    }
    ++stored;
  }
  EXPECT_EQ(stored, fit);
  EXPECT_LT(fit, 1000u);
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

// sessionStorage in MoltNet's partition: no database, and a map holding
// items stays when its site's unused areas are purged.
TEST_F(MoltNetDomStorageTest, SessionStorageWithoutADatabaseKeepsItsItems) {
  storage::SessionStorageImpl session(
      base::FilePath(), storage::SessionStorageImpl::BackingMode::kNoDisk,
      base::DoNothing(), mojo::NullReceiver());
  session.MoltKeepCachesPerSite();
  const std::string ns = base::Uuid::GenerateRandomV4().AsLowercaseString();
  session.CreateNamespace(ns);
  const blink::StorageKey kept = Key("https://kept.a.test");
  {
    mojo::Remote<blink::mojom::StorageArea> area;
    session.BindStorageArea(kept, ns, area.BindNewPipeAndPassReceiver());
    ASSERT_TRUE(Put(area.get(), 'k', 10));
  }
  task_environment_.RunUntilIdle();
  EXPECT_EQ(session.GetDatabaseForTesting(), nullptr);
  for (size_t i = 0; i <= storage::kMoltDomStorageAreasPerSite + 1; ++i) {
    mojo::Remote<blink::mojom::StorageArea> area;
    session.BindStorageArea(Key(base::StringPrintf("https://s%zu.a.test", i)),
                            ns, area.BindNewPipeAndPassReceiver());
    area.FlushForTesting();
    area.reset();
    task_environment_.RunUntilIdle();
  }
  mojo::Remote<blink::mojom::StorageArea> again;
  session.BindStorageArea(kept, ns, again.BindNewPipeAndPassReceiver());
  base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
  again->GetAll(mojo::NullRemote(), all.GetCallback());
  const std::vector<blink::mojom::KeyValuePtr> data = all.Take();
  ASSERT_EQ(data.size(), 1u);
  EXPECT_EQ(data[0]->value, std::vector<uint8_t>(10, 'v'));
  again.reset();
  task_environment_.RunUntilIdle();
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

// A credentialless iframe's areas (a storage key with a nonce, whose
// top-level site is its own origin's site wherever it is embedded) count
// with its nonce: not with its site's own pages, nor with the same site's
// credentialless frames under another page's nonce.
TEST_F(MoltNetDomStorageTest, CredentiallessFramesCountWithTheirNonce) {
  storage_->MoltKeepCachesPerSite();
  constexpr size_t kMiB = 1024 * 1024;
  const url::Origin tracker = url::Origin::Create(GURL("https://t.test"));
  const base::UnguessableToken page_a = base::UnguessableToken::Create();
  const base::UnguessableToken page_b = base::UnguessableToken::Create();
  ASSERT_TRUE(Write(Key("https://t.test"), 10 * kMiB));
  // The site's own total is full; its frames under page A's nonce have
  // theirs.
  EXPECT_TRUE(Write(blink::StorageKey::CreateWithNonce(tracker, page_a),
                    6 * kMiB));
  EXPECT_FALSE(Write(Key("https://x.t.test"), kMiB));
  // Under page B's nonce, a total of its own again.
  EXPECT_TRUE(Write(blink::StorageKey::CreateWithNonce(tracker, page_b),
                    10 * kMiB));
  // Two origins under one page's nonce share it.
  EXPECT_FALSE(Write(blink::StorageKey::CreateWithNonce(
                         url::Origin::Create(GURL("https://x.t.test")), page_a),
                     5 * kMiB));
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

// sessionStorage's areas take the sequence in turn per site too.
TEST_F(MoltNetSessionStorageTest, AFloodingSiteDoesntDelayAnothersRead) {
  storage_->MoltKeepCachesPerSite();
  const std::string tab = NewNamespace();
  const std::string other_tab = NewNamespace();
  const blink::StorageKey flooded = Key("https://flood.test");
  mojo::Remote<blink::mojom::StorageArea> writes = Area(tab, flooded);
  mojo::Remote<blink::mojom::StorageArea> clears =
      Area(tab, Key("https://x.flood.test"));
  mojo::Remote<blink::mojom::StorageArea> polite =
      Area(other_tab, Key("https://polite.test"));
  writes.FlushForTesting();
  clears.FlushForTesting();
  polite.FlushForTesting();
  EXPECT_LE(FloodThenRead(writes.get(), clears.get(), polite.get(),
                          base::BindLambdaForTesting([&]() -> size_t {
                            storage::StorageAreaImpl* area =
                                storage_->GetNamespaceForTesting(tab)
                                    ->GetStorageAreaForTesting(flooded);
                            return area && area->storage_used() > 0
                                       ? area->storage_used() - 1
                                       : 0;
                          })),
            2u);
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
