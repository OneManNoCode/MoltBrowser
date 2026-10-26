// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: what MoltNet's storage budget (molt_storage_budget_policy.h)
// counts as a site: the storage keys whose usage shares one site's share
// (MoltBudgetSiteOf()). The quota system counts each site's usage as it
// changes (molt_storage_usage_counts.h, in ClientUsageTracker and
// UsageTracker). Header-only: storage/browser's sources are Chromium's
// BUILD.gn, which MoltBrowser doesn't change.

#ifndef STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
#define STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_

#include <cstdint>
#include <map>
#include <optional>

#include "base/unguessable_token.h"
#include "net/base/schemeful_site.h"
#include "storage/browser/quota/molt_storage_usage_counts.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace storage {

// The site a storage key's usage counts to: its top-level site, except that
// a key with a nonce (a credentialless iframe's, a fenced frame's), whose
// top-level site is its own origin's site wherever it is embedded
// (blink::StorageKey::CreateWithNonce()), counts only with the keys of its
// nonce (the credentialless frames of one page). Counted with its origin's
// site, a site's credentialless frames on every site, and its own pages,
// would share one share, whose room each reads in its quota.
struct MoltBudgetSite {
  std::optional<base::UnguessableToken> nonce;
  // Its top-level site; none for a key with a nonce (a default-made
  // net::SchemefulSite would be an opaque site of its own each time).
  std::optional<net::SchemefulSite> top_level_site;

  friend bool operator==(const MoltBudgetSite&,
                         const MoltBudgetSite&) = default;
  friend bool operator<(const MoltBudgetSite& a, const MoltBudgetSite& b) {
    if (a.nonce != b.nonce) {
      return a.nonce < b.nonce;
    }
    return a.top_level_site < b.top_level_site;
  }
};

inline MoltBudgetSite MoltBudgetSiteOf(const blink::StorageKey& storage_key) {
  if (storage_key.nonce().has_value()) {
    return MoltBudgetSite{storage_key.nonce(), std::nullopt};
  }
  return MoltBudgetSite{std::nullopt, storage_key.top_level_site()};
}

// Whether `a` and `b` count to the same site's share (MoltBudgetSiteOf()).
inline bool MoltSameBudgetSite(const blink::StorageKey& a,
                               const blink::StorageKey& b) {
  return MoltBudgetSiteOf(a) == MoltBudgetSiteOf(b);
}

// The usage of `storage_key`'s site, summed from every storage key's
// (MoltSameBudgetSite()): what the counts per site
// (molt_storage_usage_counts.h) keep without the pass, for tests.
inline int64_t MoltSiteUsage(
    const std::map<blink::StorageKey, int64_t>& usage_by_storage_key,
    const blink::StorageKey& storage_key) {
  int64_t site_usage = 0;
  for (const auto& [key, usage] : usage_by_storage_key) {
    if (MoltSameBudgetSite(key, storage_key)) {
      site_usage += usage;
    }
  }
  return site_usage;
}

}  // namespace storage

#endif  // STORAGE_BROWSER_QUOTA_MOLT_STORAGE_BUDGET_H_
