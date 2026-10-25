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
// A purged area loses nothing: its data is in the partition's database (in
// memory for MoltNet) and is read again when a page opens it.
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
//    can't keep the sequence busy for other sites more than one area could.
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

  // `area` holds `new_bytes` now, fewer than before.
  void ShrankTo(std::string_view area, size_t new_bytes) {
    auto it = areas_.find(area);
    if (it == areas_.end() || new_bytes >= it->second.bytes) {
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

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_
