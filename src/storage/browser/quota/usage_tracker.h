// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STORAGE_BROWSER_QUOTA_USAGE_TRACKER_H_
#define STORAGE_BROWSER_QUOTA_USAGE_TRACKER_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "components/services/storage/public/cpp/buckets/bucket_info.h"
#include "components/services/storage/public/cpp/buckets/bucket_locator.h"
#include "components/services/storage/public/mojom/quota_client.mojom.h"
#include "storage/browser/quota/molt_storage_budget.h"  // MoltBrowser
#include "third_party/blink/public/common/storage_key/storage_key.h"  // MoltBrowser
#include "storage/browser/quota/quota_callbacks.h"
#include "storage/browser/quota/quota_client_type.h"
#include "storage/browser/quota/quota_manager_impl.h"
#include "storage/browser/quota/quota_task.h"
#include "storage/browser/quota/special_storage_policy.h"

namespace blink {
class StorageKey;
}  // namespace blink

namespace storage {

class ClientUsageTracker;

// A helper class that gathers and tracks the amount of data stored in
// all quota clients.
//
// Ownership: Each QuotaManagerImpl instance owns 1 instance of this class.
// Thread-safety: All methods except the constructor must be called on the same
// sequence.
class COMPONENT_EXPORT(STORAGE_BROWSER) UsageTracker
    : public QuotaTaskObserver {
 public:
  // The caller must ensure that all mojo::QuotaClient instances outlive this
  // instance.
  UsageTracker(
      QuotaManagerImpl* quota_manager_impl,
      const base::flat_map<mojom::QuotaClient*, QuotaClientType>& client_types,
      scoped_refptr<SpecialStoragePolicy> special_storage_policy);

  UsageTracker(const UsageTracker&) = delete;
  UsageTracker& operator=(const UsageTracker&) = delete;

  ~UsageTracker() override;

  // Retrieves all buckets for type from QuotaDatabase and requests bucket usage
  // from each registered client. Returns cached bucket usage if one exists for
  // a bucket.
  void GetGlobalUsage(UsageCallback callback);

  // Retrieves all buckets for a `storage_key` from QuotaDatabase and requests
  // bucket usage from each registered client. Returns cached bucket usage if
  // one exists for a bucket.
  void GetStorageKeyUsageWithBreakdown(const blink::StorageKey& storage_key,
                                       UsageWithBreakdownCallback callback);

  // Requests bucket usage from each registered client. Returns cached bucket
  // usage if one exists for a bucket.
  void GetBucketUsageWithBreakdown(const BucketLocator& bucket,
                                   UsageWithBreakdownCallback callback);

  // Updates usage for `bucket` in the ClientUsageTracker for `client_type`.
  void UpdateBucketUsageCache(QuotaClientType client_type,
                              const BucketLocator& bucket,
                              std::optional<int64_t> delta);

  // Deletes `bucket` from the cache for `client_type` if it exists.
  // Called by QuotaManagerImpl::BucketDataDeleter.
  void DeleteBucketCache(QuotaClientType client_type,
                         const BucketLocator& bucket);

  // Returns accumulated usage for all cached buckets from registered
  // ClientUsageTrackers. Used to determine storage pressure.
  int64_t GetCachedUsage() const;

  // Retrieves all cached usage organized by host. Expected to be called after
  // GetGlobalUsage which retrieves and caches host usage.
  std::map<std::string, int64_t> GetCachedHostsUsage() const;

  // Returns all cached usage organized by StorageKey. Used for histogram
  // recording.
  std::map<blink::StorageKey, int64_t> GetCachedStorageKeysUsage() const;

  // Returns all cached usage organized by bucket. Used for eviction.
  std::map<BucketLocator, int64_t> GetCachedBucketsUsage() const;

  // Checks if there are ongoing tasks to get usage. Used to prevent a
  // UsageTracker reset from happening before a task is complete.
  bool IsWorking() const {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    return !global_usage_callbacks_.empty() ||
           !storage_key_usage_callbacks_.empty() ||
           !bucket_usage_callbacks_.empty();
  }

  // Sets if a `storage_key` for `client_type` should / should not be excluded
  // from quota restrictions.
  void SetUsageCacheEnabled(QuotaClientType client_type,
                            const blink::StorageKey& storage_key,
                            bool enabled);

  // MoltBrowser: for MoltNet's storage budget (molt_storage_usage_counts.h;
  // sites are molt_storage_budget.h's). Asks again for the usage of
  // `site`'s stale storage keys, then runs
  // `callback`; the first time, counts the cache per site from then on and
  // asks for every bucket's usage instead. One refresh at a time per site:
  // while one runs, `callback` waits for it and one more. MoltSiteUsage() and
  // MoltTotalUsage() then read the counts, costing a lookup per client.
  void MoltRefreshSiteUsage(const MoltBudgetSite& site,
                            base::OnceClosure callback);
  int64_t MoltSiteUsage(const MoltBudgetSite& site) const;
  int64_t MoltTotalUsage() const;
  // MoltBrowser: `site` is gone (a page's nonce, its buckets deleted):
  // nothing of it stays stale.
  void MoltForgetSite(const MoltBudgetSite& site);

 private:
  // MoltBrowser:
  void MoltDidCountEverything(base::OnceClosure callback,
                              int64_t usage,
                              int64_t unlimited_usage);
  void MoltRefreshStaleKeys(const MoltBudgetSite& site,
                            base::OnceClosure done);
  void MoltDidRefresh(const MoltBudgetSite& site, base::OnceClosure done);
  void MoltMarkStale(const blink::StorageKey& storage_key);
  void MoltSettle(const MoltBudgetSite& site);

  struct AccumulateInfo;
  friend class ClientUsageTracker;

  void DidGetAllBuckets(QuotaErrorOr<std::set<BucketInfo>> result);
  void DidGetBucketsForStorageKey(const blink::StorageKey& storage_key,
                                  QuotaErrorOr<std::set<BucketInfo>> result);

  void AccumulateClientGlobalUsage(base::OnceClosure barrier_callback,
                                   AccumulateInfo* info,
                                   int64_t total_usage,
                                   int64_t unlimited_usage);
  void AccumulateClientUsageWithBreakdown(base::OnceClosure barrier_callback,
                                          AccumulateInfo* info,
                                          QuotaClientType client,
                                          int64_t total_usage,
                                          int64_t unlimited_usage);

  void FinallySendGlobalUsage(std::unique_ptr<AccumulateInfo> info);
  void FinallySendStorageKeyUsageWithBreakdown(
      std::unique_ptr<AccumulateInfo> info,
      const blink::StorageKey& storage_key);
  void FinallySendBucketUsageWithBreakdown(std::unique_ptr<AccumulateInfo> info,
                                           const BucketLocator& bucket);

  ClientUsageTracker& GetClient(QuotaClientType type);

  SEQUENCE_CHECKER(sequence_checker_);

  // Raw pointer usage is safe because `quota_manager_impl_` owns `this` and
  // is therefore valid throughout its lifetime.
  const raw_ptr<QuotaManagerImpl> quota_manager_impl_;
  base::flat_map<QuotaClientType, std::unique_ptr<ClientUsageTracker>>
      client_tracker_map_;

  std::vector<UsageCallback> global_usage_callbacks_;
  std::map<blink::StorageKey, std::vector<UsageWithBreakdownCallback>>
      storage_key_usage_callbacks_;
  std::map<BucketLocator, std::vector<UsageWithBreakdownCallback>>
      bucket_usage_callbacks_;

  // MoltBrowser: whether the clients count their cache per site, whether
  // every bucket's usage has been asked for since, and the storage keys
  // whose usage changed in a way the cache couldn't take in.
  bool molt_counting_ = false;
  bool molt_counted_everything_ = false;
  MoltStaleStorageKeys<MoltBudgetSite, blink::StorageKey> molt_stale_;
  // MoltBrowser: the sites whose stale keys are being asked for, with the
  // checks waiting for the next refresh; and those with a refresh of their
  // own due (kMoltStaleSettleDelaySeconds).
  MoltSiteRefreshes<MoltBudgetSite, base::OnceClosure> molt_refreshes_;
  std::set<MoltBudgetSite> molt_settles_;

  base::WeakPtrFactory<UsageTracker> weak_factory_{this};
};

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_USAGE_TRACKER_H_
