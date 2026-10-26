// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: the group a storage key's localStorage or sessionStorage
// area counts to in a partition that keeps caches, totals and a commit
// rate per site (molt_dom_storage_site_policy.h: MoltDomStorageGroupKindFor()).
// Header-only, for LocalStorageImpl, SessionStorageImpl and
// SessionStorageNamespaceImpl.

#ifndef COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_H_
#define COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_H_

#include <string>

#include "components/services/storage/dom_storage/molt_dom_storage_site_policy.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"

namespace storage {

// The name of the group `storage_key`'s area counts to: its top-level
// site's serialization, or, for a key with a nonce, the nonce's, or, for a
// key with an opaque top-level site, the key's own.
inline std::string MoltDomStorageSiteOf(const blink::StorageKey& storage_key) {
  const MoltDomStorageGroupKind kind = MoltDomStorageGroupKindFor(
      storage_key.nonce().has_value(), storage_key.top_level_site().opaque());
  switch (kind) {
    case MoltDomStorageGroupKind::kTopLevelSite:
      return MoltDomStorageGroupName(kind,
                                     storage_key.top_level_site().Serialize());
    case MoltDomStorageGroupKind::kNonce:
      return MoltDomStorageGroupName(kind, storage_key.nonce()->ToString());
    case MoltDomStorageGroupKind::kOwnKey:
      return MoltDomStorageGroupName(kind, storage_key.Serialize());
  }
  return MoltDomStorageGroupName(MoltDomStorageGroupKind::kOwnKey,
                                 storage_key.Serialize());
}

// Whether the areas of `a` and `b` count to the same group.
inline bool MoltDomStorageSameSite(const blink::StorageKey& a,
                                   const blink::StorageKey& b) {
  return MoltDomStorageSiteOf(a) == MoltDomStorageSiteOf(b);
}

}  // namespace storage

#endif  // COMPONENTS_SERVICES_STORAGE_DOM_STORAGE_MOLT_DOM_STORAGE_SITE_H_
