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
//    it embeds) may use molt_per_site_quota, whatever the other sites
//    stored: there is no total for all sites that a group of sites could
//    fill, and so none another site could find full;
//  * the memory it all takes is bounded by the machine instead: all sites
//    together may always store pool_size (the floor, which every machine
//    MoltNet runs on holds), and beyond it a write may only take what the
//    machine has available above molt_memory_reserve
//    (MoltNeedsMemoryCheck(), MoltStorageQuota()). A write the machine has
//    no room for fails, for the site that writes; nothing another site
//    stored is removed.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_
#define STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

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

// Whether answering a storage key's quota needs the machine's available
// memory: only where its site could take all sites' usage (`total_usage`)
// beyond the floor (QuotaSettings::pool_size), never below it, so the
// machine never shows in what MoltNet's sites can store while they hold
// less than the floor together.
constexpr bool MoltNeedsMemoryCheck(int64_t per_site_quota,
                                    int64_t site_usage,
                                    int64_t total_usage,
                                    int64_t floor) {
  if (!MoltUsesStorageBudget(per_site_quota)) {
    return false;
  }
  const int64_t site_room = MoltSiteRoom(site_usage, per_site_quota);
  const int64_t total = std::max<int64_t>(0, total_usage);
  return site_room > 0 && total > floor - site_room;
}

// The room the machine leaves beyond what is stored: what it has available
// (`available_memory`, nullopt: unknown) above `memory_reserve`. Unknown
// means none.
constexpr int64_t MoltMachineRoom(std::optional<int64_t> available_memory,
                                  int64_t memory_reserve) {
  if (!available_memory) {
    return 0;
  }
  return std::max<int64_t>(0, *available_memory - memory_reserve);
}

// The quota a storage key (or one of its buckets) using `usage` gets, given
// Chromium's `quota` for it, its top-level site's `site_usage` (all of the
// site's storage keys) and all sites' `total_usage`: no more room than the
// site's own share has left, and no more than all sites together may still
// take: up to the floor always, beyond it what the machine can spare.
// Chromium's quota where there is no budget.
constexpr int64_t MoltStorageQuota(int64_t quota,
                                   int64_t usage,
                                   int64_t site_usage,
                                   int64_t total_usage,
                                   int64_t per_site_quota,
                                   int64_t floor,
                                   std::optional<int64_t> available_memory,
                                   int64_t memory_reserve) {
  if (!MoltUsesStorageBudget(per_site_quota)) {
    return quota;
  }
  const int64_t site_room = MoltSiteRoom(site_usage, per_site_quota);
  const int64_t below_floor =
      std::max<int64_t>(0, floor - std::max<int64_t>(0, total_usage));
  const int64_t all_room =
      std::max(below_floor, MoltMachineRoom(available_memory, memory_reserve));
  const int64_t room = std::min(site_room, all_room);
  const int64_t base = std::max<int64_t>(0, usage);
  const int64_t limited = base > std::numeric_limits<int64_t>::max() - room
                              ? std::numeric_limits<int64_t>::max()
                              : base + room;
  return std::min(quota, limited);
}

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_POLICY_H_
