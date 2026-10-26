// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what MoltNet's storage budget (molt_storage_budget_policy.h)
// counts for a site: the usage of every storage key whose top-level site is
// the site's (its own and those of what it embeds), and all sites' usage for
// the floor.
//
// A quota answer under the budget needs the asking key's site's usage. The
// quota system caches each bucket's usage per client (ClientUsageTracker),
// updated as clients report changes, but has no total per site, so the
// answer summed every cached bucket of every site, after a pass over all of
// them (UsageTracker::GetGlobalUsage(): every bucket in the quota database,
// each client asked for every bucket it hadn't cached). Its cost grew with
// what all sites stored, on the quota manager's one sequence, so a site
// that made many storage keys or buckets slowed every site's quota checks.
//
// Instead, for quota systems under the budget:
//  * each client's cache keeps a total per site and one for all sites, moved
//    by every change to a cached bucket's usage (MoltSiteUsageTotals), so
//    reading a site's usage costs a lookup per client;
//  * a change the cache can't take in (a bucket whose usage isn't cached
//    yet, a change of unknown size, a storage key whose cache is turned back
//    on) marks the bucket's storage key stale for its site
//    (MoltStaleStorageKeys); a quota check asks again for the usage of its
//    own site's stale keys only, before it reads the site's total;
//  * one pass over everything when a usage tracker starts (the cache is
//    empty then; in MoltNet, so is the partition), after which every change
//    is either counted or marked.
// So a quota check costs what its own site's storage keys cost, whatever
// other sites store.
//
// Std-only templates (Site: net::SchemefulSite, Key: blink::StorageKey in
// the quota system), so molt_ai_unittests and a standalone harness test
// them. Header-only: storage/browser's sources are Chromium's BUILD.gn,
// which MoltBrowser doesn't change. Not thread-safe: the quota manager uses
// them on its sequence.

#ifndef STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
#define STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace storage {

// Cached usage per site, and for all sites.
template <typename Site>
class MoltSiteUsageTotals {
 public:
  // A cached bucket of `site` went from `old_usage` to `new_usage` (0 for
  // one not cached before, or no longer cached).
  void Changed(const Site& site, int64_t old_usage, int64_t new_usage) {
    const int64_t delta = new_usage - old_usage;
    if (delta == 0) {
      return;
    }
    auto it = sites_.try_emplace(site, 0).first;
    it->second += delta;
    total_ += delta;
    if (it->second == 0) {
      sites_.erase(it);
    }
  }

  int64_t Of(const Site& site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : it->second;
  }
  int64_t total() const { return total_; }
  size_t site_count() const { return sites_.size(); }

  void Clear() {
    sites_.clear();
    total_ = 0;
  }

 private:
  std::map<Site, int64_t> sites_;
  int64_t total_ = 0;
};

// The storage keys of each site whose usage must be asked for again before
// the site's total is read.
template <typename Site, typename Key>
class MoltStaleStorageKeys {
 public:
  void Mark(const Site& site, const Key& key) { sites_[site].insert(key); }

  // The stale keys of `site`, no longer marked: a change from now on marks
  // them again.
  std::vector<Key> Take(const Site& site) {
    std::vector<Key> keys;
    auto it = sites_.find(site);
    if (it == sites_.end()) {
      return keys;
    }
    keys.assign(it->second.begin(), it->second.end());
    sites_.erase(it);
    return keys;
  }

  bool HasAny(const Site& site) const { return sites_.contains(site); }
  void Clear() { sites_.clear(); }

 private:
  std::map<Site, std::set<Key>> sites_;
};

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
