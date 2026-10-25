// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: MoltNet's storage budget: what one site stores never changes
// what another can store or keeps, and nothing a page can store depends on
// the machine.
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
//    it embeds) may use molt_per_site_quota, whatever the other sites
//    stored, and that is the only limit a write meets: there is no total
//    for all sites that a group of sites could fill and another find full,
//    and the machine's memory is never asked on a write, so whether one
//    succeeds says nothing about other sites or the machine
//    (MoltStorageQuota());
//  * the memory it all takes is watched instead: once all sites together
//    store more than pool_size (the floor, which every machine MoltNet runs
//    on holds), each quota answered tells the embedder
//    (QuotaSettings::molt_on_growth_beyond_floor), which checks the
//    machine's memory apart from any write and ends the whole identity if
//    the machine is short of memory (chrome/browser/molt_ai/tor/
//    molt_net_memory_valve_policy.h). No write fails because of the total
//    or the machine; nothing another site stored is removed.
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

// What a site using `site_usage` (all its storage keys) may still store of
// its own share.
constexpr int64_t MoltSiteRoom(int64_t site_usage, int64_t per_site_quota) {
  return std::max<int64_t>(0, per_site_quota - std::max<int64_t>(0, site_usage));
}

// The quota a storage key (or one of its buckets) using `usage` gets, given
// Chromium's `quota` for it and its top-level site's `site_usage` (all of
// the site's storage keys): no more room than the site's own share has
// left. Nothing else: not other sites' usage, not the machine. Chromium's
// quota where there is no budget.
constexpr int64_t MoltStorageQuota(int64_t quota,
                                   int64_t usage,
                                   int64_t site_usage,
                                   int64_t per_site_quota) {
  if (!MoltUsesStorageBudget(per_site_quota)) {
    return quota;
  }
  const int64_t room = MoltSiteRoom(site_usage, per_site_quota);
  const int64_t base = std::max<int64_t>(0, usage);
  const int64_t limited = base > std::numeric_limits<int64_t>::max() - room
                              ? std::numeric_limits<int64_t>::max()
                              : base + room;
  return std::min(quota, limited);
}

// Whether all sites' usage (`total_usage`) is past the floor
// (QuotaSettings::pool_size), so the embedder is told to watch the
// machine's memory (QuotaSettings::molt_on_growth_beyond_floor).
constexpr bool MoltStorageBeyondFloor(int64_t per_site_quota,
                                      int64_t total_usage,
                                      int64_t floor) {
  return MoltUsesStorageBudget(per_site_quota) && total_usage > floor;
}

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_
