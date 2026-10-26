// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: localStorage and sessionStorage caches per site, for storage
// partitions that ask for it (LocalStorageControl / SessionStorageControl
// MoltKeepCachesPerSite(): MoltNet's, through
// ContentBrowserClient::ShouldKeepDomStorageCachesPerSite()).
//
// At Chromium 156 the storage service keeps each storage area (a storage
// key's localStorage, or its sessionStorage in one tab) it has opened in a
// cache, and purges the unused ones (no page has them open) of every site
// when a new area is opened and all areas together pass one limit per
// partition: 50 areas or 20 MiB on desktop (LocalStorageImpl::
// PurgeUnusedAreasIfNeeded(), SessionStorageImpl's). So one site, opening
// areas (its subdomains, or frames it embeds, each have one), could push
// every other site's unused areas out of the cache, and, timing how fast its
// own areas reopen, learn how much the other sites keep open.
//
// With caches per site, the areas are grouped by their top-level site (a
// frame's areas count to the site it is embedded in), and:
//  * opening an area may purge only the unused areas of its own site, when
//    that site's areas pass the per-site limits (MoltDomStoragePurgesSite());
//    nothing another site does changes what is purged, or when;
//  * an unused area is purged once it has been unused for
//    kMoltDomStorageIdleExpirySeconds, checked every
//    kMoltDomStorageSweepIntervalSeconds whatever any site does
//    (MoltDomStorageAreaExpired()), so the cache stays bounded without a
//    limit for all sites;
//  * the low-end-device rule (purge on every open) and PurgeMemory() (which
//    drops every site's caches) don't apply.
// A purged area loses nothing: its data is in the partition's database and
// is read again when a page opens it. A partition that keeps DOM storage per
// site in memory (MoltNet's) keeps no database
// (MoltDomStorageKeepsNoDatabase()): there, an area is purged only if it
// holds no item (MoltDomStorageMayDropArea()), and nothing is read back.
//
// No database: Chromium's partitions in memory keep each kind's areas in an
// in-memory database on a sequence of its own, which every area commits
// its changes to and reads its items from when it opens. A site's first
// read of an area then waited for every commit queued before it, other
// sites' included; per-site commit rates bound each site's commits, but
// not those of many sites together. The database there only holds a second
// copy of what the areas hold, read again for an area purged from the
// cache, so a partition kept per site in memory runs without one (the
// storage service's own mode for a database that can't be opened: an area
// starts empty and holds its items itself, its cache mode keys and values)
// and keeps every area holding an item: opening an area never waits on
// anything another site queued, and there are no commits at all. What the
// areas hold is still bounded by each site's total, and counted once
// instead of twice.
//
// The same partitions also keep, per site:
//  * a total for what the site's areas hold (MoltDomStorageSiteTotals):
//    each area may hold its own 10 MiB, as in Chromium, and all of a site's
//    localStorage areas together (its subdomains', and those of the frames
//    it embeds) may hold kMoltDomStorageBytesPerSite, as may all of its
//    sessionStorage areas in every tab together. A write that would take the
//    site past it fails as one past the area's own limit does (the storage
//    service refuses it and the page's value reverts). Without it, a site
//    could store 10 MiB for each of as many subdomains as it likes, in
//    memory. Past kMoltDomStorageFloorBytes for all sites together (each
//    kind on its own), the memory valve hears of each further MiB, as it
//    does of storage and blobs past their floors;
//  * one commit rate: Chromium limits how often each area writes its
//    changes to the database (60 commits and 10 MiB an hour, as a rate it
//    catches up with), and all areas share one database sequence. Here all
//    of a site's areas together commit at that rate
//    (MoltDomStorageSiteCommitDelayMs()), so a site with many subdomains
//    can't keep the sequence busy for other sites more than one area could
//    (for a partition with a database; MoltNet's has none);
//  * one write rate (MoltDomStorageMayWrite()): every write a page sends is
//    work on the storage service's sequence, which all sites' storage
//    operations share (the change, and a notification carrying the value to
//    every renderer with the area open), and Chromium takes as many as a
//    page sends. Here a site's writes, of each kind, come out of a
//    token bucket: kMoltDomStorageSiteWritesBurst writes and
//    kMoltDomStorageSiteWriteBytesBurst bytes, refilled at
//    kMoltDomStorageSiteWritesPerSecond and
//    kMoltDomStorageSiteWriteBytesPerSecond. A write past it fails as one
//    past the area's limit does (never as a bad message): for localStorage
//    the page's value reverts.
//
// Std-only, so molt_ai_unittests tests it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace storage {

// A quarter of Chromium's desktop limits for a whole partition (50 areas,
// 20 MiB), for each site: a site rarely has more than a few areas.
inline constexpr size_t kMoltDomStorageAreasPerSite = 12;
inline constexpr size_t kMoltDomStorageCachePerSiteBytes = 5 * 1024 * 1024;

// How long an area may stay unused in the cache, and how often that is
// checked (the same as MoltNet's HTTP cache entries).
inline constexpr int64_t kMoltDomStorageIdleExpirySeconds = 30 * 60;
inline constexpr int64_t kMoltDomStorageSweepIntervalSeconds = 60;

// Whether opening an area of a site whose cached areas (`site_areas`, of
// which `site_unused_areas` are unused) hold `site_cache_bytes` purges that
// site's unused areas.
constexpr bool MoltDomStoragePurgesSite(size_t site_areas,
                                        size_t site_unused_areas,
                                        size_t site_cache_bytes) {
  return site_unused_areas > 0 &&
         (site_areas > kMoltDomStorageAreasPerSite ||
          site_cache_bytes > kMoltDomStorageCachePerSiteBytes);
}

// Whether an area unused for `unused_seconds` is purged by the sweep.
constexpr bool MoltDomStorageAreaExpired(int64_t unused_seconds) {
  return unused_seconds >= kMoltDomStorageIdleExpirySeconds;
}

static_assert(kMoltDomStorageSweepIntervalSeconds <
              kMoltDomStorageIdleExpirySeconds);

// ---- No database ----

// Whether a partition's localStorage or sessionStorage runs without a
// database: one that keeps DOM storage per site (`caches_per_site`) and in
// memory (`in_memory`), so a database would only hold a second copy of
// what the areas hold. An on-disk partition always keeps its database.
constexpr bool MoltDomStorageKeepsNoDatabase(bool caches_per_site,
                                             bool in_memory) {
  return caches_per_site && in_memory;
}

// Whether an unused area may leave the cache: with a database, always (its
// items are read again from there); without one, only if it holds no item
// (also none of zero bytes), since its items are nowhere else.
constexpr bool MoltDomStorageMayDropArea(bool has_database, bool holds_items) {
  return has_database || !holds_items;
}

// ---- Totals per site ----

// What all of a site's areas of one kind may hold together: one area's own
// limit (blink's kPerStorageAreaQuota and the storage service's
// kPerStorageAreaOverQuotaAllowance), so a site of one origin in one tab can
// store what Chromium lets it store.
inline constexpr size_t kMoltDomStorageBytesPerSite =
    10 * 1024 * 1024 + 100 * 1024;

// The floor for all sites together, each kind on its own: five sites at
// their full share. Past it the memory valve hears of each further
// kMoltDomStorageGrowthStepBytes (molt_net_memory_valve_policy.h).
inline constexpr size_t kMoltDomStorageFloorBytes =
    5 * kMoltDomStorageBytesPerSite;
inline constexpr size_t kMoltDomStorageGrowthStepBytes = 1024 * 1024;

// Whether all sites' areas growing from `old_total` to `new_total` bytes
// tell the valve: past the floor, once per step of growth.
constexpr bool MoltDomStorageGrowthNotifies(size_t old_total,
                                            size_t new_total) {
  if (new_total <= old_total || new_total <= kMoltDomStorageFloorBytes) {
    return false;
  }
  return old_total <= kMoltDomStorageFloorBytes ||
         new_total / kMoltDomStorageGrowthStepBytes !=
             old_total / kMoltDomStorageGrowthStepBytes;
}

// What a partition's areas of one kind hold, per area and per site. An area
// is named by `area` (a localStorage area's storage key, a sessionStorage
// map's id; what it holds is in the database whether or not it is cached),
// its site by `site` (its storage key's top-level site). Not thread-safe:
// the storage service uses it on its sequence.
class MoltDomStorageSiteTotals {
 public:
  // Whether `area` of `site` may grow to `new_bytes`: if the site's areas
  // together stay within kMoltDomStorageBytesPerSite. Counted if so.
  bool MayGrowTo(std::string_view area, std::string_view site,
                 size_t new_bytes) {
    Area& counted = areas_[std::string(area)];
    if (counted.site.empty()) {
      counted.site = std::string(site);
    }
    size_t& site_bytes = sites_[counted.site];
    const size_t others = site_bytes - counted.bytes;
    if (new_bytes > counted.bytes &&
        others + new_bytes > kMoltDomStorageBytesPerSite) {
      if (counted.bytes == 0) {
        Forget(area);
      }
      return false;
    }
    total_ = total_ - counted.bytes + new_bytes;
    site_bytes = others + new_bytes;
    counted.bytes = new_bytes;
    return true;
  }

  // `area` of `site` holds `new_bytes` now, no more than it is counted for.
  // An area not counted yet (a sessionStorage map copied for a tab, made
  // with what the map it copies holds) is counted as it is, even past its
  // site's share: the site's areas then grow no more until they shrink.
  void ShrankTo(std::string_view area, std::string_view site,
                size_t new_bytes) {
    auto it = areas_.find(area);
    if (it == areas_.end()) {
      if (new_bytes > 0) {
        areas_.emplace(std::string(area), Area{std::string(site), new_bytes});
        sites_[std::string(site)] += new_bytes;
        total_ += new_bytes;
      }
      return;
    }
    if (new_bytes >= it->second.bytes) {
      return;
    }
    const size_t gone = it->second.bytes - new_bytes;
    total_ -= gone;
    sites_[it->second.site] -= gone;
    it->second.bytes = new_bytes;
  }

  // `area` and what it held are gone (deleted from the database).
  void Forget(std::string_view area) {
    auto it = areas_.find(area);
    if (it == areas_.end()) {
      return;
    }
    total_ -= it->second.bytes;
    auto site = sites_.find(it->second.site);
    site->second -= it->second.bytes;
    if (site->second == 0) {
      sites_.erase(site);
    }
    areas_.erase(it);
  }

  size_t total() const { return total_; }
  size_t SiteBytes(std::string_view site) const {
    auto it = sites_.find(site);
    return it == sites_.end() ? 0 : it->second;
  }

 private:
  struct Area {
    std::string site;
    size_t bytes = 0;
  };
  std::map<std::string, Area, std::less<>> areas_;
  std::map<std::string, size_t, std::less<>> sites_;
  size_t total_ = 0;
};

// ---- One commit rate per site ----

// What one area may commit an hour in Chromium (LocalStorageImpl,
// SessionStorageImpl: kMaxCommitsPerHour, kMaxBytesPerHour), for all of a
// site's areas together.
inline constexpr int64_t kMoltDomStorageSiteCommitsPerHour = 60;
inline constexpr int64_t kMoltDomStorageSiteBytesPerHour = 10 * 1024 * 1024;
inline constexpr int64_t kMoltDomStorageHourMs = 60 * 60 * 1000;

// How long a site's next commit waits for the site's rate, in
// milliseconds: the time its `commits` and `bytes` committed so far need at
// the rate (as StorageAreaImpl::RateLimiter computes it for an area), less
// the `elapsed_ms` since the site's first area opened.
constexpr int64_t MoltDomStorageSiteCommitDelayMs(int64_t commits,
                                                  int64_t bytes,
                                                  int64_t elapsed_ms) {
  const int64_t for_commits =
      commits / kMoltDomStorageSiteCommitsPerHour * kMoltDomStorageHourMs +
      commits % kMoltDomStorageSiteCommitsPerHour * kMoltDomStorageHourMs /
          kMoltDomStorageSiteCommitsPerHour;
  const int64_t for_bytes =
      bytes / kMoltDomStorageSiteBytesPerHour * kMoltDomStorageHourMs +
      bytes % kMoltDomStorageSiteBytesPerHour * kMoltDomStorageHourMs /
          kMoltDomStorageSiteBytesPerHour;
  const int64_t needed = for_commits > for_bytes ? for_commits : for_bytes;
  return needed > elapsed_ms ? needed - elapsed_ms : 0;
}
static_assert(MoltDomStorageSiteCommitDelayMs(60, 0, 0) ==
              kMoltDomStorageHourMs);
static_assert(MoltDomStorageSiteCommitDelayMs(1, 0, 60 * 1000) == 0);

// A site's commits so far, and when its first area opened (milliseconds on
// the caller's clock).
struct MoltDomStorageSiteRate {
  int64_t start_ms = 0;
  int64_t commits = 0;
  int64_t bytes = 0;
};

// ---- One write rate per site ----

// What a site's areas of one kind may take in writes (Put()s), as a token
// bucket: up to the burst at once, refilled at the rate. Chosen for what
// real pages do with localStorage, which is synchronous and small: a few to
// a few hundred keys written as a page loads (consent and analytics ids,
// feature flags, cached settings), state saved on each change while a user
// types or plays (tens of writes a second at most; a game saving every
// frame, 60), and rarely more than a few hundred KiB in all (pages that
// keep more use IndexedDB). The bytes' burst is one area's whole limit, so
// a page can always fill its storage at once.
inline constexpr int64_t kMoltDomStorageSiteWritesBurst = 1000;
inline constexpr int64_t kMoltDomStorageSiteWritesPerSecond = 100;
inline constexpr int64_t kMoltDomStorageSiteWriteBytesBurst =
    static_cast<int64_t>(kMoltDomStorageBytesPerSite);
inline constexpr int64_t kMoltDomStorageSiteWriteBytesPerSecond =
    1024 * 1024;

// A site's write allowance. Tokens are kept in thousandths, so that a
// millisecond refills a whole number of them: a rate of N a second refills
// N thousandths a millisecond. Full when made.
struct MoltDomStorageWriteAllowance {
  int64_t at_ms = 0;
  int64_t writes_milli = kMoltDomStorageSiteWritesBurst * 1000;
  int64_t bytes_milli = kMoltDomStorageSiteWriteBytesBurst * 1000;
};

// Refills `allowance` up to `now_ms` (milliseconds on the caller's
// monotonic clock), then whether a write of `bytes` (its key and value)
// fits: one write and its bytes, taken if so. A refused write takes
// nothing. A write larger than the bytes' burst (which the area's own
// limit refuses anyway) costs the burst.
constexpr bool MoltDomStorageMayWrite(MoltDomStorageWriteAllowance& allowance,
                                      int64_t now_ms,
                                      uint64_t bytes) {
  constexpr int64_t kWritesFull = kMoltDomStorageSiteWritesBurst * 1000;
  constexpr int64_t kBytesFull = kMoltDomStorageSiteWriteBytesBurst * 1000;
  // Long enough to fill both from empty; capping it keeps the products
  // below in range.
  constexpr int64_t kFillMs =
      kMoltDomStorageSiteWriteBytesBurst * 1000 /
          kMoltDomStorageSiteWriteBytesPerSecond +
      kMoltDomStorageSiteWritesBurst * 1000 /
          kMoltDomStorageSiteWritesPerSecond +
      1;
  if (now_ms > allowance.at_ms) {
    const int64_t elapsed =
        now_ms - allowance.at_ms < kFillMs ? now_ms - allowance.at_ms : kFillMs;
    const int64_t writes =
        allowance.writes_milli + elapsed * kMoltDomStorageSiteWritesPerSecond;
    const int64_t bytes_left = allowance.bytes_milli +
                               elapsed * kMoltDomStorageSiteWriteBytesPerSecond;
    allowance.writes_milli = writes < kWritesFull ? writes : kWritesFull;
    allowance.bytes_milli = bytes_left < kBytesFull ? bytes_left : kBytesFull;
    allowance.at_ms = now_ms;
  }
  const int64_t cost =
      (bytes < static_cast<uint64_t>(kMoltDomStorageSiteWriteBytesBurst)
           ? static_cast<int64_t>(bytes)
           : kMoltDomStorageSiteWriteBytesBurst) *
      1000;
  if (allowance.writes_milli < 1000 || allowance.bytes_milli < cost) {
    return false;
  }
  allowance.writes_milli -= 1000;
  allowance.bytes_milli -= cost;
  return true;
}

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_
