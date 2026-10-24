// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what MoltNet's storage budget (molt_storage_budget_policy.h)
// counts for a site: the usage of every storage key whose top-level site is
// the site's, from the quota system's per-key usage. Header-only:
// storage/browser's sources are Chromium's BUILD.gn, which MoltBrowser
// doesn't change.

#ifndef STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
#define STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_

#include <cstdint>
#include <map>

#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace storage {

// The usage of `storage_key`'s top-level site: its storage keys' together.
inline int64_t MoltSiteUsage(
    const std::map<blink::StorageKey, int64_t>& usage_by_storage_key,
    const blink::StorageKey& storage_key) {
  int64_t site_usage = 0;
  for (const auto& [key, usage] : usage_by_storage_key) {
    if (key.top_level_site() == storage_key.top_level_site()) {
      site_usage += usage;
    }
  }
  return site_usage;
}

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
