// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/shared_worker_key.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/synchronization/waitable_event.h"
#include "base/unguessable_token.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/storage_key/ancestor_chain_bit.mojom-blink.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/inspector/worker_devtools_params.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/core/workers/global_scope_creation_params.h"
#include "third_party/blink/renderer/core/workers/worker_backing_thread_startup_data.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/core/workers/worker_thread_test_helper.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace blink::molt_fingerprinting {
namespace {

std::array<uint8_t, kSessionSeedSize> TestSeed() {
  std::array<uint8_t, kSessionSeedSize> seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(5 + 11 * i);
  }
  return seed;
}

// What a farbler does, reduced to a value that can cross threads: two
// farblers are the same iff their PRF agrees on these inputs.
using Fingerprint = std::array<uint64_t, 4>;

Fingerprint FingerprintOf(const Farbler& farbler) {
  Fingerprint out;
  for (uint64_t i = 0; i < out.size(); ++i) {
    out[i] = farbler.Prf(i, i * 77);
  }
  return out;
}

Fingerprint Derived(const char* site, const std::string& partition = "") {
  return FingerprintOf(Farbler(DeriveSiteKey(TestSeed(), site, partition)));
}

scoped_refptr<const SecurityOrigin> Origin(const char* url) {
  return SecurityOrigin::Create(KURL(url));
}

// The storage key of a frame showing `frame_url` in a page whose top-level
// site is `top_url`'s: what the browser sends as `constructor_key` for a
// worker that frame constructs.
BlinkStorageKey ThirdPartyKey(const char* frame_url, const char* top_url) {
  return BlinkStorageKey::Create(Origin(frame_url),
                                 BlinkSchemefulSite(Origin(top_url)),
                                 mojom::blink::AncestorChainBit::kCrossSite);
}

// Runs on the worker thread.
void ReadKeyOnWorkerThread(WorkerThread* thread,
                           std::optional<Fingerprint>* result,
                           base::WaitableEvent* done) {
  EXPECT_TRUE(thread->IsCurrentThread());
  if (const std::optional<Farbler> farbler =
          FarblerFor(thread->GlobalScope())) {
    *result = FingerprintOf(*farbler);
  }
  done->Signal();
}

// A worker global scope on its own thread, set up the way
// WebSharedWorkerImpl::StartWorkerContext sets up a shared worker: no
// top-level frame origin, and the constructor's storage key and the
// browser's Shields decision handed over through its WorkerClients before the
// thread starts.
class TestSharedWorker {
 public:
  explicit TestSharedWorker(const BlinkStorageKey& constructor_key,
                            bool shields_down = false)
      : thread_(std::make_unique<WorkerThreadForTest>(reporting_proxy_)) {
    std::unique_ptr<GlobalScopeCreationParams> params =
        GlobalScopeCreationParams::CreateForWorkerForTesting(
            Origin("https://worker.example/").get(),
            KURL("https://worker.example/worker.js"));
    ProvideToSharedWorker(static_cast<StorageKey>(constructor_key),
                          shields_down, params->worker_clients.Get());
    thread_->Start(std::move(params),
                   WorkerBackingThreadStartupData::CreateDefault(),
                   std::make_unique<WorkerDevToolsParams>());
    thread_->WaitForInit();
  }

  ~TestSharedWorker() {
    thread_->Terminate();
    thread_->WaitForShutdownForTesting();
  }

  // The key the worker farbles with (asked on its thread), or nullopt when
  // it isn't farbled.
  std::optional<Fingerprint> Key() {
    std::optional<Fingerprint> result;
    base::WaitableEvent done;
    WorkerThread* thread = thread_.get();
    PostCrossThreadTask(
        *thread->GetTaskRunner(TaskType::kInternalTest), FROM_HERE,
        CrossThreadBindOnce(
            &ReadKeyOnWorkerThread, CrossThreadUnretained(thread),
            CrossThreadUnretained(&result), CrossThreadUnretained(&done)));
    done.Wait();
    return result;
  }

 private:
  MockWorkerReportingProxy reporting_proxy_;
  std::unique_ptr<WorkerThreadForTest> thread_;
};

// These tests never configure kUniform (see molt_fingerprinting_test.cc).
class MoltSharedWorkerKeyTest : public testing::Test {
 protected:
  void SetUp() override { ResetForTesting(); }
  void TearDown() override { ResetForTesting(); }

  void Start() {
    Configure(Mode::kFarble);
    SetSessionSeed(TestSeed());
  }

  // The key of a document whose storage key is `storage_key`.
  std::optional<Fingerprint> DocumentKey(const BlinkStorageKey& storage_key) {
    holders_.push_back(
        DummyPageHolder::CreateAndCommitNavigation(KURL("https://doc.test/")));
    LocalDOMWindow& window = *holders_.back()->GetFrame().DomWindow();
    window.SetStorageKey(storage_key);
    if (const std::optional<Farbler> farbler = FarblerFor(&window)) {
      return FingerprintOf(*farbler);
    }
    return std::nullopt;
  }

  test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<DummyPageHolder>> holders_;
};

TEST_F(MoltSharedWorkerKeyTest, FirstPartyWorkerUsesItsSitesKey) {
  Start();
  const BlinkStorageKey key =
      BlinkStorageKey::CreateFirstParty(Origin("https://www.a.example/"));
  TestSharedWorker worker(key);
  const std::optional<Fingerprint> worker_key = worker.Key();
  ASSERT_TRUE(worker_key.has_value());
  EXPECT_EQ(*worker_key, Derived("https://a.example"));
  EXPECT_EQ(worker_key, DocumentKey(key));
  EXPECT_EQ(worker.Key(), worker_key);
}

TEST_F(MoltSharedWorkerKeyTest, ThirdPartyWorkerUsesItsTopLevelSitesKey) {
  if (!StorageKey::IsThirdPartyStoragePartitioningEnabled()) {
    GTEST_SKIP() << "third-party storage partitioning is off";
  }
  Start();
  const BlinkStorageKey under_a =
      ThirdPartyKey("https://tracker.example/", "https://a.example/");
  TestSharedWorker worker(under_a);
  const std::optional<Fingerprint> worker_key = worker.Key();
  ASSERT_TRUE(worker_key.has_value());
  EXPECT_EQ(*worker_key, Derived("https://a.example"));
  // The same key as the iframe that made it and as a.example's own pages.
  EXPECT_EQ(worker_key, DocumentKey(under_a));
  EXPECT_EQ(worker_key, DocumentKey(BlinkStorageKey::CreateFirstParty(
                            Origin("https://a.example/"))));

  // A restarted instance gets it again.
  TestSharedWorker restarted(under_a);
  EXPECT_EQ(restarted.Key(), worker_key);

  // The same script under b.example (a separate worker, since the storage
  // key partitions it) gets b.example's key.
  TestSharedWorker under_b(
      ThirdPartyKey("https://tracker.example/", "https://b.example/"));
  const std::optional<Fingerprint> under_b_key = under_b.Key();
  ASSERT_TRUE(under_b_key.has_value());
  EXPECT_EQ(*under_b_key, Derived("https://b.example"));
  EXPECT_NE(under_b_key, worker_key);
}

TEST_F(MoltSharedWorkerKeyTest, NonceIsThePartition) {
  Start();
  const base::UnguessableToken nonce = base::UnguessableToken::Create();
  const BlinkStorageKey key =
      BlinkStorageKey::CreateWithNonce(Origin("https://f.example/"), nonce);
  TestSharedWorker worker(key);
  const std::optional<Fingerprint> worker_key = worker.Key();
  ASSERT_TRUE(worker_key.has_value());
  EXPECT_EQ(*worker_key, Derived("https://f.example", nonce.ToString()));
  EXPECT_NE(*worker_key, Derived("https://f.example"));
  EXPECT_EQ(worker_key, DocumentKey(key));
}

TEST_F(MoltSharedWorkerKeyTest, OpaqueTopLevelSiteGetsARandomKey) {
  if (!StorageKey::IsThirdPartyStoragePartitioningEnabled()) {
    GTEST_SKIP() << "third-party storage partitioning is off";
  }
  Start();
  const BlinkStorageKey key = BlinkStorageKey::Create(
      Origin("https://w.example/"), BlinkSchemefulSite(),
      mojom::blink::AncestorChainBit::kCrossSite);
  TestSharedWorker one(key);
  TestSharedWorker two(key);
  const std::optional<Fingerprint> one_key = one.Key();
  ASSERT_TRUE(one_key.has_value());
  EXPECT_EQ(one.Key(), one_key);  // Stable for the worker's life.
  EXPECT_NE(two.Key(), one_key);
  EXPECT_NE(*one_key, Derived("https://w.example"));
}

TEST_F(MoltSharedWorkerKeyTest, KeyNeededBeforeTheSeedStaysRandom) {
  Configure(Mode::kFarble);
  const BlinkStorageKey key =
      BlinkStorageKey::CreateFirstParty(Origin("https://a.example/"));
  TestSharedWorker early(key);
  const std::optional<Fingerprint> first = early.Key();
  // Fail closed, with a random key.
  ASSERT_TRUE(first.has_value());
  EXPECT_NE(*first, Derived("https://a.example"));
  SetSessionSeed(TestSeed());
  EXPECT_EQ(early.Key(), first);  // Kept for the worker's whole life.
  TestSharedWorker late(key);
  EXPECT_EQ(late.Key(), Derived("https://a.example"));
}

TEST_F(MoltSharedWorkerKeyTest, ShieldsOffComesWithTheStart) {
  if (!StorageKey::IsThirdPartyStoragePartitioningEnabled()) {
    GTEST_SKIP() << "third-party storage partitioning is off";
  }
  Start();
  const BlinkStorageKey under_a =
      ThirdPartyKey("https://tracker.example/", "https://a.example/");
  // The browser decided, when it started the worker, that MoltShield is off
  // for a.example (the key's top-level site).
  TestSharedWorker down(under_a, /*shields_down=*/true);
  EXPECT_FALSE(down.Key().has_value());
  // Without that decision the worker is farbled, with a.example's key.
  TestSharedWorker up(under_a);
  EXPECT_EQ(up.Key(), Derived("https://a.example"));
}

TEST_F(MoltSharedWorkerKeyTest, NothingIsHandedOverWhenOff) {
  const StorageKey key = static_cast<StorageKey>(
      BlinkStorageKey::CreateFirstParty(Origin("https://a.example/")));
  WorkerClients* off = MakeGarbageCollected<WorkerClients>();
  ProvideToSharedWorker(key, /*shields_down=*/false, off);
  EXPECT_EQ(SharedWorkerStorageKey(off), nullptr);
  ProvideToSharedWorker(key, /*shields_down=*/false, nullptr);
  EXPECT_EQ(SharedWorkerStorageKey(nullptr), nullptr);

  Start();
  WorkerClients* on = MakeGarbageCollected<WorkerClients>();
  ProvideToSharedWorker(key, /*shields_down=*/false, on);
  const StorageKey* handed_over = SharedWorkerStorageKey(on);
  ASSERT_NE(handed_over, nullptr);
  EXPECT_EQ(*handed_over, key);
}

}  // namespace
}  // namespace blink::molt_fingerprinting
