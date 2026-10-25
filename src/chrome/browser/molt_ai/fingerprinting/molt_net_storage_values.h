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
// Each site, its storage keys and those of what it embeds together, may
// store a tenth of the 600 MiB, whatever the other sites stored
// (storage/browser/quota/molt_storage_budget_policy.h). There is no total a
// group of sites could fill: the 600 MiB are what MoltNet's sites can
// always store together (every machine MoltNet runs on holds it), and
// beyond it a write may take only what the machine has available above a
// reserve of 1 GiB; the write the machine has no room for fails, for the
// site that writes, and nothing is evicted. Below the 600 MiB the machine
// isn't asked, so nothing a page can store shows the machine's memory;
// beyond it, only a group of sites that has filled the machine to its
// reserve can find a write refused, when the machine would soon run out of
// memory anyway.
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

// Quota (storage::QuotaSettings): what all MoltNet sites can always store
// together (beyond it, the machine decides), what one site may (all its
// storage keys; also for "session only" sites: every MoltNet site is), and
// nothing kept free on a disk MoltNet doesn't use.
inline constexpr int64_t kMoltNetQuotaPoolBytes = 600 * kMoltNetStorageMiB;
inline constexpr int64_t kMoltNetQuotaPerSiteBytes = 60 * kMoltNetStorageMiB;
// The memory a write beyond the pool must leave available on the machine.
inline constexpr int64_t kMoltNetStorageMemoryReserveBytes =
    1024 * kMoltNetStorageMiB;
// A storage key may use what its site may.
inline constexpr int64_t kMoltNetQuotaPerStorageKeyBytes =
    kMoltNetQuotaPerSiteBytes;

// Blobs (storage::BlobStorageLimits::max_blob_in_memory_space): what all
// MoltNet sites can always hold together (beyond it, the machine decides,
// with kMoltNetStorageMemoryReserveBytes left available); no disk.
inline constexpr uint64_t kMoltNetBlobMemoryBytes = 800 * kMoltNetStorageMiB;
// What one budget key may hold (storage::BlobStorageLimits::
// molt_per_key_memory): the blobs of one renderer process, which in MoltNet
// serves one site in one tab, or the browser's own
// (storage/browser/blob/molt_blob_budget_policy.h). A quarter of the floor,
// so no single process takes the total past it alone.
inline constexpr uint64_t kMoltNetBlobMemoryPerKeyBytes =
    kMoltNetBlobMemoryBytes / 4;

// Chromium's rules on the reference machine, rounded down to 100 MiB.
inline constexpr int64_t kMoltNetStorageRounding = 100 * kMoltNetStorageMiB;
static_assert(kMoltNetQuotaPoolBytes ==
              kMoltNetStorageReferenceMemory * 15 / 100 /
                  kMoltNetStorageRounding * kMoltNetStorageRounding);
static_assert(kMoltNetQuotaPerSiteBytes == kMoltNetQuotaPoolBytes / 10);
static_assert(static_cast<int64_t>(kMoltNetBlobMemoryBytes) ==
              kMoltNetStorageReferenceMemory / 5 / kMoltNetStorageRounding *
                  kMoltNetStorageRounding);
static_assert(kMoltNetBlobMemoryPerKeyBytes > 0 &&
              kMoltNetBlobMemoryPerKeyBytes < kMoltNetBlobMemoryBytes);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_VALUES_H_
