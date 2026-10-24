// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNet's storage limits: what a MoltNet page can store before its writes
// fail is the same on every machine (the values, and why they are safe on
// small machines, are in molt_net_storage_values.h).
//
// Hooks (each marked "MoltBrowser:"):
//
//   content  StoragePartitionImpl::GetQuotaSettings (every storage
//              partition of the profile: the quota pool and per-site limit
//              of IndexedDB, Cache Storage, the origin-private file system,
//              service worker scripts and Background Fetch), asked through
//              ContentBrowserClient::GetFixedQuotaSettings
//                                                  -> MoltNetQuotaSettings
//   storage  the quota manager and evictor read MoltNet's per-site budget
//              from the settings (QuotaSettings::molt_per_site_quota,
//              storage/browser/quota/molt_storage_budget_policy.h)
//            ChromeBlobStorageContext::GetFor (the profile's blob storage:
//              Blob, File, blob: URLs, fetch().blob(), IndexedDB's blobs),
//              asked through ContentBrowserClient::GetFixedBlobStorageLimits
//                                              -> MoltNetBlobStorageLimits
//   storage  BlobMemoryController::SetFixedStorageLimits (new): limits that
//              neither the machine's memory nor memory pressure change
//
// ChromeContentBrowserClient passes the profile's renderer mode
// (ModeForBrowserContext(): kUniform for MoltNet, kOff while the kill
// switch is off). Every other profile keeps Chromium's limits. A profile's
// storage is set up once, so the kill switch applies from the next MoltNet
// session.

#ifndef CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_H_

#include <optional>

#include "chrome/common/molt_ai/fingerprinting/molt_fingerprinting_switches.h"
#include "storage/browser/blob/blob_storage_constants.h"
#include "storage/browser/quota/quota_settings.h"

namespace molt_ai::fingerprinting {

// The fixed quota settings for the storage partitions of a profile whose
// renderers run in `mode`: MoltNet's for kUniform, else nullopt (Chromium's
// own).
std::optional<storage::QuotaSettings> MoltNetQuotaSettings(RendererMode mode);

// The fixed blob limits for a profile whose renderers run in `mode`:
// MoltNet's for kUniform, else nullopt.
std::optional<storage::BlobStorageLimits> MoltNetBlobStorageLimits(
    RendererMode mode);

}  // namespace molt_ai::fingerprinting

#endif  // CHROME_BROWSER_MOLT_AI_FINGERPRINTING_MOLT_NET_STORAGE_H_
