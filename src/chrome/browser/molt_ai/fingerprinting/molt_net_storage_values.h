// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's storage limits: the most a MoltNet page can store before writes
// fail, the same on every machine (molt_net_storage.h applies them).
// Std-only and constexpr, so molt_ai_unittests checks them standalone.
//
// Chromium derives an off-the-record profile's limits from the machine:
//   * the quota pool is 15-20% (a random draw) of the physical memory, and
//     each site (storage key) may use a third of it, for IndexedDB, Cache
//     Storage, the origin-private file system, service worker scripts and
//     Background Fetch together (storage/browser/quota/quota_settings.cc);
//     navigator.storage.estimate() has reported a fixed quota since 156,
//     but writing until QuotaExceededError measures the real one;
//   * blob memory is 2 GiB on 64-bit desktops, a fifth of the memory on
//     32-bit ones and ChromeOS, and 1% on Android
//     (storage/browser/blob/blob_memory_controller.cc), and with
//     Chromium's stateful memory pressure it shrinks while the machine is
//     short of memory.
//
// MoltNet's data is held in memory, in the browser and storage processes,
// so the fixed values must be safe on the smallest machines MoltNet runs
// on. They are Chromium's own rules applied to a 4 GiB machine, rounded
// down: the incognito pool at the low end of its range (15% of 4 GiB, 614
// MiB) is 600 MiB, and blobs get a fifth of 4 GiB (819 MiB): 800 MiB.
// That is less than Incognito allows on most machines (on 8 GB, 1.2-1.6
// GiB of pool), and far more than sites store in practice (a few MB of
// caches and databases); a site that needs more (offline web apps, large
// in-browser downloads) gets the same QuotaExceededError on every MoltNet
// machine. No blob data goes to disk (off-the-record profiles never page
// blobs out).
//
// The 600 MiB are the most MoltNet's sites may store together (Chromium
// only evicts down to 70% of its pool every half hour, of any site), and
// each site, its storage keys and those of what it embeds together, a
// tenth of that (storage/browser/quota/molt_storage_budget_policy.h): one
// site's writes can then neither fail nor have another site's data evicted
// unless ten other sites hold their whole share.
//
// Not changed, because they already are the same everywhere: localStorage
// and sessionStorage (10 MiB per storage area, components/services/storage/
// dom_storage/dom_storage_constants.h) and the blob transport sizes.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_VALUES_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_VALUES_H_

#include <cstdint>

namespace molt_ai::fingerprinting {

inline constexpr int64_t kMoltNetStorageMiB = 1024 * 1024;

// The machine the limits are sized for.
inline constexpr int64_t kMoltNetStorageReferenceMemory =
    4096 * kMoltNetStorageMiB;

// Quota (storage::QuotaSettings): the most all MoltNet sites may store
// together, what one site may (all its storage keys; also for "session
// only" sites: every MoltNet site is), and nothing kept free on a disk
// MoltNet doesn't use.
inline constexpr int64_t kMoltNetQuotaPoolBytes = 600 * kMoltNetStorageMiB;
inline constexpr int64_t kMoltNetQuotaPerSiteBytes = 60 * kMoltNetStorageMiB;
// A storage key may use what its site may.
inline constexpr int64_t kMoltNetQuotaPerStorageKeyBytes =
    kMoltNetQuotaPerSiteBytes;

// Blobs (storage::BlobStorageLimits::max_blob_in_memory_space), all MoltNet
// sites together; no disk.
inline constexpr uint64_t kMoltNetBlobMemoryBytes = 800 * kMoltNetStorageMiB;

// Chromium's rules on the reference machine, rounded down to 100 MiB.
inline constexpr int64_t kMoltNetStorageRounding = 100 * kMoltNetStorageMiB;
static_assert(kMoltNetQuotaPoolBytes ==
              kMoltNetStorageReferenceMemory * 15 / 100 /
                  kMoltNetStorageRounding * kMoltNetStorageRounding);
static_assert(kMoltNetQuotaPerSiteBytes == kMoltNetQuotaPoolBytes / 10);
static_assert(static_cast<int64_t>(kMoltNetBlobMemoryBytes) ==
              kMoltNetStorageReferenceMemory / 5 / kMoltNetStorageRounding *
                  kMoltNetStorageRounding);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_VALUES_H_
