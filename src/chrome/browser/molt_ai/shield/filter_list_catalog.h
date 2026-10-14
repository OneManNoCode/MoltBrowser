// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The filter lists MoltShield knows about. Default set: EasyList,
// EasyPrivacy and uBlock Origin's filters/privacy/badware/quick-fixes
// (≈150k filters); Fanboy's cookie/annoyance lists are opt-in.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_CATALOG_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_CATALOG_H_

#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "chrome/browser/molt_ai/shield/molt_shield_store.h"

namespace molt_ai::shield {

struct FilterListEntry {
  // Stable id: cache file name (lists/<id>.txt) and settings key.
  const char* id;
  const char* title;
  // Official publisher URL, then a mirror (may be empty) tried if it fails.
  // Both are fetched through the profile's network context (MoltNet/Tor
  // aware) with credentials omitted.
  const char* url;
  const char* fallback_url;
  // uBlock Origin's own lists may use `trusted-*` scriptlets.
  bool trusted;
  bool enabled_by_default;
};

// TODO(molt-update): signed list bundles from a GenEye CDN. The intended
// shape is one compressed bundle of all default lists + a detached Ed25519
// signature, verified with ListSignatureVerifier (list_signature_verifier.h)
// before anything is written to the cache; upstream URLs above stay as the
// fallback. Disabled until the key scheme the update work in
// molt_ai/update is building is available.
inline constexpr char kSignedBundleUrl[] = "";
inline constexpr char kSignedBundleSignatureUrl[] = "";

base::span<const FilterListEntry> GetFilterListCatalog();

// Returns nullptr for unknown ids.
const FilterListEntry* FindFilterList(std::string_view id);

// The user's override if any, else the catalog default.
bool IsListEnabled(const FilterListEntry& entry,
                   const ShieldSettings& settings);

// Enabled lists in catalog order, with the sha256 recorded at download time
// (empty for lists never downloaded).
std::vector<ListSpec> EnabledListSpecs(const StoreState& state);

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_CATALOG_H_
