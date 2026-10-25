// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: per-site limits for MoltNet's in-memory HTTP cache.
//
// Off-the-record network contexts keep their HTTP cache in memory
// (disk_cache::MemBackendImpl): one size limit for every site together (2%
// of the machine's memory, at most 50 MiB) and one least-recently-used list.
// When the cache is full, a write by any site evicts the least recently used
// entries of all of them, so a page could make another site's entries go
// (by filling the cache) and find out whether its own went (by timing a
// reload), and memory pressure on the machine evicted entries too. Entries
// are keyed by top-level site, so no site reads another's; what was shared
// was the room.
//
// With a budget (MoltCacheSiteBudget, set on MoltNet's network contexts
// through HttpCache::MoltSetPerSiteBudget()):
//
//  * Each site's entries (those of the keys whose top-level site is the
//    site: its own pages' and what they embed) may use per_site_bytes
//    together. When a site's write takes it past that, its own least
//    recently used entries are evicted, down to nine tenths of it, as
//    Chromium does for the whole cache; never another site's.
//  * An entry not used for idle_expiry_seconds is dropped (on its next use,
//    and by a sweep), whatever other sites do: so the cache holds what the
//    sites in use now use, and room a site took is given back once it
//    stops using it.
//  * All sites together stay within total_bytes, which fits
//    kMoltHttpCacheShares sites' full shares: a write that would take the
//    cache past it isn't stored (the response is simply not cached); nothing
//    is evicted for it. So a site's entries go only because of its own use,
//    and a site below its share can store new entries unless more than
//    kMoltHttpCacheShares - 1 other sites have each used their whole share
//    within the idle expiry.
//  * The machine's memory doesn't change any of it: the limits are fixed,
//    and memory pressure evicts nothing.
//
// Std-only, so molt_ai_unittests and a standalone harness test it.

#ifndef NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_BUDGET_POLICY_H_
#define NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_BUDGET_POLICY_H_

#include <cstdint>
#include <string_view>

namespace disk_cache {

struct MoltCacheSiteBudget {
  // Every site's share (0: no budget, Chromium's one limit and LRU).
  int64_t per_site_bytes = 0;
  // All sites together.
  int64_t total_bytes = 0;
  // One entry's stream at most (MaxFileSize()).
  int64_t max_entry_bytes = 0;
  // An entry unused this long is dropped (0: never).
  int64_t idle_expiry_seconds = 0;
  // How often the idle entries are looked for.
  int64_t sweep_interval_seconds = 0;

  friend constexpr bool operator==(const MoltCacheSiteBudget&,
                                   const MoltCacheSiteBudget&) = default;
};

// MoltNet's numbers. A site may cache 10 MiB, what Chromium gives all sites
// together where it can't read the machine's memory; an entry may be half
// of that (Chromium: an eighth of the cache, 6.25 MiB of 50). The total,
// twelve shares (120 MiB, 3% of the 4 GiB machine MoltNet's limits are
// sized for; Chromium uses 2% of the machine's memory, at most 50 MiB), is
// reached only when twelve sites have each cached 10 MiB and kept using it.
inline constexpr int64_t kMoltHttpCacheMiB = 1024 * 1024;
inline constexpr int64_t kMoltHttpCachePerSiteBytes = 10 * kMoltHttpCacheMiB;
inline constexpr int64_t kMoltHttpCacheShares = 12;
inline constexpr int64_t kMoltHttpCacheTotalBytes =
    kMoltHttpCacheShares * kMoltHttpCachePerSiteBytes;
inline constexpr int64_t kMoltHttpCacheMaxEntryBytes =
    kMoltHttpCachePerSiteBytes / 2;
inline constexpr int64_t kMoltHttpCacheIdleExpirySeconds = 30 * 60;
inline constexpr int64_t kMoltHttpCacheSweepIntervalSeconds = 60;
static_assert(kMoltHttpCacheTotalBytes < (int64_t{1} << 31));

constexpr MoltCacheSiteBudget MoltNetHttpCacheBudget() {
  MoltCacheSiteBudget budget;
  budget.per_site_bytes = kMoltHttpCachePerSiteBytes;
  budget.total_bytes = kMoltHttpCacheTotalBytes;
  budget.max_entry_bytes = kMoltHttpCacheMaxEntryBytes;
  budget.idle_expiry_seconds = kMoltHttpCacheIdleExpirySeconds;
  budget.sweep_interval_seconds = kMoltHttpCacheSweepIntervalSeconds;
  return budget;
}

constexpr bool MoltUsesCacheSiteBudget(const MoltCacheSiteBudget& budget) {
  return budget.per_site_bytes > 0 && budget.total_bytes > 0;
}

// The site an HTTP cache key's entry counts against: the top-level site of
// the key's network isolation key. net::HttpCache::GenerateCacheKey() makes
//   "<credentials>/<upload id>/_dk_[s_][cn_]<top-level site> <frame site>
//    [<partition>] <url>"
// for a key split by network isolation key (the double key), where the
// sites are serialized SchemefulSites (which never hold a space). A key
// without the double key (a shared or unsplit entry, which MoltNet's context
// never makes) counts against no site: "".
constexpr std::string_view MoltHttpCacheSiteOfKey(std::string_view key) {
  // "<credentials>/<upload id>/".
  size_t pos = key.find('/');
  if (pos == std::string_view::npos) {
    return {};
  }
  pos = key.find('/', pos + 1);
  if (pos == std::string_view::npos) {
    return {};
  }
  std::string_view rest = key.substr(pos + 1);
  constexpr std::string_view kDoubleKey = "_dk_";
  if (!rest.starts_with(kDoubleKey)) {
    return {};
  }
  rest.remove_prefix(kDoubleKey.size());
  // HttpCache::kSubframeDocumentResourcePrefix, then
  // kCrossSiteMainFrameNavigationPrefix, each optional, in that order.
  if (rest.starts_with("s_")) {
    rest.remove_prefix(2);
  }
  if (rest.starts_with("cn_")) {
    rest.remove_prefix(3);
  }
  const size_t end = rest.find(' ');
  if (end == std::string_view::npos || end == 0) {
    return {};
  }
  return rest.substr(0, end);
}

// After a site's entries grew to `site_size`: whether its own least
// recently used entries are evicted, and down to what.
constexpr bool MoltCacheSiteOverShare(int64_t site_size,
                                      const MoltCacheSiteBudget& budget) {
  return site_size > budget.per_site_bytes;
}
constexpr int64_t MoltCacheSiteEvictionTarget(
    const MoltCacheSiteBudget& budget) {
  return budget.per_site_bytes - budget.per_site_bytes / 10;
}

// Whether a write that took its site to `site_size` and the cache to
// `total_size` (after the site's own evictions) is refused: the site past
// its share (its entries in use couldn't go), or the cache past its total.
constexpr bool MoltCacheWriteRefused(int64_t site_size,
                                     int64_t total_size,
                                     const MoltCacheSiteBudget& budget) {
  return site_size > budget.per_site_bytes || total_size > budget.total_bytes;
}

// Whether an entry unused for `idle_seconds` is dropped.
constexpr bool MoltCacheEntryExpired(int64_t idle_seconds,
                                     const MoltCacheSiteBudget& budget) {
  return budget.idle_expiry_seconds > 0 &&
         idle_seconds >= budget.idle_expiry_seconds;
}

}  // namespace disk_cache

#endif  // NET_DISK_CACHE_MEMORY_MOLT_MEM_BACKEND_BUDGET_POLICY_H_
