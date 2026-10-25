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
// Std-only, so molt_ai_unittests tests it.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_

#include <cstddef>
#include <cstdint>

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

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_POLICY_H_
