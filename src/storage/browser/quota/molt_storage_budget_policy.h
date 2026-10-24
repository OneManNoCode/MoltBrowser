// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: MoltNet's storage budget: what one site stores never changes
// what another can store or keeps.
//
// At Chromium 156 a storage key's writes (IndexedDB, Cache Storage, the
// origin-private file system, service worker scripts, Background Fetch)
// are checked against its own quota only, and the quota pool limits nothing
// directly: it decides eviction. Every 30 minutes (the first time 5 minutes
// after the quota database is set up) the quota manager evicts the least
// recently used buckets, of any site, until all sites together use 70% of
// the pool, also in an off-the-record profile, whose data is in memory.
// So sites that together store more than that could have another site's
// data evicted, and with no limit but the per-key one (a site can use
// many storage keys: its subdomains, embedded in its own pages) memory
// wasn't bounded either until eviction ran.
//
// With QuotaSettings::molt_per_site_quota set (MoltNet's partitions,
// chrome/browser/molt_ai/fingerprinting/molt_net_storage.cc):
//
//  * nothing is evicted to bring the pool down
//    (QuotaTemporaryStorageEvictor; a bucket that expires, through the
//    Storage Buckets API, still goes);
//  * every top-level site's storage keys together (its own and those of what
//    it embeds) may use molt_per_site_quota;
//  * all sites together may use pool_size, the memory bound: a write that
//    would go beyond it fails. It fits MoltSitesToFillStorage() sites' full
//    budgets, so a site still below its own budget is refused only once
//    that many other sites hold theirs.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_
#define STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_

#include <algorithm>
#include <cstdint>
#include <limits>

namespace storage {

// Whether the quota system runs with MoltNet's budget: `per_site_quota` is
// QuotaSettings::molt_per_site_quota.
constexpr bool MoltUsesStorageBudget(int64_t per_site_quota) {
  return per_site_quota > 0;
}

// Whether the evictor may evict buckets to bring all sites' usage down
// (never with MoltNet's budget).
constexpr bool MoltMayEvictForThePool(int64_t per_site_quota) {
  return !MoltUsesStorageBudget(per_site_quota);
}

// The quota a storage key (or one of its buckets) using `usage` gets, given
// Chromium's `quota` for it, its top-level site's `site_usage` (all of the
// site's storage keys) and all sites' `total_usage`: no more room than the
// site's budget and the total have left. Chromium's quota where there is no
// budget.
constexpr int64_t MoltStorageQuota(int64_t quota,
                                   int64_t usage,
                                   int64_t site_usage,
                                   int64_t total_usage,
                                   int64_t per_site_quota,
                                   int64_t total_quota) {
  if (!MoltUsesStorageBudget(per_site_quota)) {
    return quota;
  }
  const int64_t site_room = std::max<int64_t>(0, per_site_quota - site_usage);
  const int64_t total_room = std::max<int64_t>(0, total_quota - total_usage);
  const int64_t room = std::min(site_room, total_room);
  const int64_t base = std::max<int64_t>(0, usage);
  const int64_t limited = base > std::numeric_limits<int64_t>::max() - room
                              ? std::numeric_limits<int64_t>::max()
                              : base + room;
  return std::min(quota, limited);
}

// How many other sites must hold their whole budget before a site still
// below its own is refused: one with `per_site` - 1 bytes writing one more
// fails only once the others hold more than `total` - `per_site`.
constexpr int64_t MoltSitesToFillStorage(int64_t total, int64_t per_site) {
  return (total - per_site) / per_site + 1;
}

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_
