// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's storage budget with the real quota manager (QuotaManagerImpl,
// off the record, with a mock quota client): a site's quota follows its own
// storage keys' usage, counted per site as the usage cache changes
// (storage/browser/quota/molt_storage_budget.h), and a quota check asks a
// client again only for its own site's stale storage keys, whatever other
// sites store.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/stringprintf.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/unguessable_token.h"
#include "components/services/storage/public/cpp/buckets/bucket_info.h"
#include "components/services/storage/public/cpp/buckets/bucket_init_params.h"
#include "components/services/storage/public/cpp/buckets/bucket_locator.h"
#include "components/services/storage/public/cpp/quota_error_or.h"
#include "components/services/storage/public/mojom/quota_client.mojom.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "storage/browser/quota/quota_client_type.h"
#include "storage/browser/quota/quota_manager_impl.h"
#include "storage/browser/quota/quota_manager_proxy.h"
#include "storage/browser/quota/quota_settings.h"
#include "storage/browser/test/mock_quota_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/quota/quota_types.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr int64_t kKiB = 1024;
constexpr int64_t kMiB = 1024 * kKiB;
constexpr int64_t kPerSite = 60 * kMiB;

blink::StorageKey Key(const std::string& origin) {
  return blink::StorageKey::CreateFromStringForTesting(origin);
}

class MoltNetStorageSiteUsageTest : public testing::Test {
 protected:
  void SetUp() override {
    quota_manager_ = base::MakeRefCounted<storage::QuotaManagerImpl>(
        /*is_incognito=*/true, base::FilePath(),
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        /*special_storage_policy=*/nullptr, storage::GetQuotaSettingsFunc());
    storage::QuotaSettings settings;
    settings.pool_size = 100 * kMiB;  // the floor
    settings.per_storage_key_quota = kPerSite;
    settings.session_only_per_storage_key_quota = kPerSite;
    settings.refresh_interval = base::TimeDelta::Max();
    settings.molt_per_site_quota = kPerSite;
    settings.molt_on_growth_beyond_floor =
        base::BindLambdaForTesting([this] { ++past_floor_; });
    quota_manager_->SetQuotaSettings(settings);

    auto client = std::make_unique<storage::MockQuotaClient>(
        base::WrapRefCounted(quota_manager_->proxy()),
        storage::QuotaClientType::kIndexedDatabase);
    client_ = client.get();
    mojo::PendingRemote<storage::mojom::QuotaClient> remote;
    mojo::MakeSelfOwnedReceiver(std::move(client),
                                remote.InitWithNewPipeAndPassReceiver());
    quota_manager_->proxy()->RegisterClient(
        std::move(remote), storage::QuotaClientType::kIndexedDatabase);
    task_environment_.RunUntilIdle();
  }

  void TearDown() override {
    client_ = nullptr;
    quota_manager_.reset();
    task_environment_.RunUntilIdle();
  }

  // `key`'s default bucket, holding `usage` bytes in the client.
  storage::BucketLocator Store(const blink::StorageKey& key, int64_t usage) {
    base::test::TestFuture<storage::QuotaErrorOr<storage::BucketInfo>> bucket;
    quota_manager_->UpdateOrCreateBucket(
        storage::BucketInitParams::ForDefaultBucket(key), bucket.GetCallback());
    storage::QuotaErrorOr<storage::BucketInfo> info = bucket.Take();
    EXPECT_TRUE(info.has_value());
    const storage::BucketLocator locator = info->ToBucketLocator();
    client_->AddBucketsData({{locator, usage}});
    return locator;
  }

  // The quota `key` gets.
  int64_t Quota(const blink::StorageKey& key) {
    base::test::TestFuture<blink::mojom::QuotaStatusCode, int64_t, int64_t>
        answer;
    quota_manager_->GetUsageAndQuota(key, answer.GetCallback());
    EXPECT_EQ(answer.Get<0>(), blink::mojom::QuotaStatusCode::kOk);
    return answer.Get<2>();
  }

  base::test::TaskEnvironment task_environment_;
  scoped_refptr<storage::QuotaManagerImpl> quota_manager_;
  raw_ptr<storage::MockQuotaClient> client_ = nullptr;
  int past_floor_ = 0;
};

// A site's storage keys share its share; another site keeps all of its own.
TEST_F(MoltNetStorageSiteUsageTest, ASitesKeysShareItsShare) {
  Store(Key("https://a.test"), 10 * kMiB);
  Store(Key("https://x.a.test"), 20 * kMiB);
  EXPECT_EQ(Quota(Key("https://y.a.test")), kPerSite - 30 * kMiB);
  EXPECT_EQ(Quota(Key("https://a.test")), 10 * kMiB + kPerSite - 30 * kMiB);
  EXPECT_EQ(Quota(Key("https://polite.test")), kPerSite);
}

// A credentialless iframe's storage key (with its page's nonce) counts with
// its nonce, not with its origin's site: the counts per site follow the
// budget's groups (MoltBudgetSiteOf()).
TEST_F(MoltNetStorageSiteUsageTest, CredentiallessFramesCountWithTheirNonce) {
  const url::Origin tracker = url::Origin::Create(GURL("https://t.test"));
  const base::UnguessableToken page = base::UnguessableToken::Create();
  Store(Key("https://t.test"), 50 * kMiB);
  const blink::StorageKey framed =
      blink::StorageKey::CreateWithNonce(tracker, page);
  Store(framed, 5 * kMiB);
  EXPECT_EQ(Quota(Key("https://x.t.test")), kPerSite - 50 * kMiB);
  EXPECT_EQ(Quota(framed), kPerSite);
  // Another origin under the same page's nonce shares it.
  EXPECT_EQ(Quota(blink::StorageKey::CreateWithNonce(
                url::Origin::Create(GURL("https://o.test")), page)),
            kPerSite - 5 * kMiB);
}

// A change to a cached bucket's usage is counted at once: the next check
// asks the client for nothing.
TEST_F(MoltNetStorageSiteUsageTest, CachedChangesAreCounted) {
  const storage::BucketLocator bucket = Store(Key("https://a.test"), kMiB);
  ASSERT_EQ(Quota(Key("https://x.a.test")), kPerSite - kMiB);
  const size_t asked = client_->get_bucket_usage_call_count();
  client_->ModifyBucketAndNotify(bucket, 4 * kMiB);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Quota(Key("https://x.a.test")), kPerSite - 5 * kMiB);
  EXPECT_EQ(client_->get_bucket_usage_call_count(), asked);
}

// A greedy site storing in many new storage keys doesn't add to a polite
// site's quota checks: the polite site's check asks the client for none of
// the greedy site's buckets (Chromium's pass over every bucket would), and
// the greedy site's own check asks for its own.
TEST_F(MoltNetStorageSiteUsageTest, AGreedySiteDoesntSlowAnothersChecks) {
  Store(Key("https://polite.test"), kMiB);
  ASSERT_EQ(Quota(Key("https://polite.test")), kPerSite);

  constexpr int kKeys = 200;
  for (int i = 0; i < kKeys; ++i) {
    const storage::BucketLocator bucket =
        Store(Key(base::StringPrintf("https://s%d.greedy.test", i)), 0);
    client_->ModifyBucketAndNotify(bucket, 100 * kKiB);
  }
  task_environment_.RunUntilIdle();

  size_t asked = client_->get_bucket_usage_call_count();
  EXPECT_EQ(Quota(Key("https://polite.test")), kPerSite);
  EXPECT_EQ(client_->get_bucket_usage_call_count(), asked);

  asked = client_->get_bucket_usage_call_count();
  EXPECT_EQ(Quota(Key("https://greedy.test")),
            kPerSite - kKeys * 100 * kKiB);
  EXPECT_GE(client_->get_bucket_usage_call_count(), asked + kKeys);
  // Once counted, not asked again.
  asked = client_->get_bucket_usage_call_count();
  EXPECT_EQ(Quota(Key("https://greedy.test")),
            kPerSite - kKeys * 100 * kKiB);
  EXPECT_EQ(client_->get_bucket_usage_call_count(), asked);
}

// Two checks of one site at once, while one of its storage keys is stale
// (a change the cache couldn't take in): the second waits for the first's
// refresh of that key, and both answer with the room the site really has.
// Chromium-shaped, the second read the site's count while the key's usage
// was still being asked for, and answered with 20 MiB more room.
TEST_F(MoltNetStorageSiteUsageTest, ConcurrentChecksDontOvershoot) {
  const blink::StorageKey a = Key("https://a.test");
  Store(a, 10 * kMiB);
  ASSERT_EQ(Quota(a), kPerSite);
  const storage::BucketLocator sub = Store(Key("https://x.a.test"), 0);
  client_->ModifyBucketAndNotify(sub, 20 * kMiB);
  task_environment_.RunUntilIdle();

  base::test::TestFuture<blink::mojom::QuotaStatusCode, int64_t, int64_t>
      first;
  base::test::TestFuture<blink::mojom::QuotaStatusCode, int64_t, int64_t>
      second;
  quota_manager_->GetUsageAndQuota(a, first.GetCallback());
  quota_manager_->GetUsageAndQuota(a, second.GetCallback());
  // a uses 10 MiB; the site 30 MiB of its 60.
  EXPECT_EQ(first.Get<2>(), 10 * kMiB + (kPerSite - 30 * kMiB));
  EXPECT_EQ(second.Get<2>(), 10 * kMiB + (kPerSite - 30 * kMiB));
}

// Past the floor for all sites together, each quota answered tells the
// embedder.
TEST_F(MoltNetStorageSiteUsageTest, PastTheFloorTheEmbedderHears) {
  for (int i = 0; i < 3; ++i) {
    Store(Key(base::StringPrintf("https://site%d.test", i)), 40 * kMiB);
  }
  Quota(Key("https://site0.test"));
  EXPECT_EQ(past_floor_, 1);
  Quota(Key("https://other.test"));
  EXPECT_EQ(past_floor_, 2);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
