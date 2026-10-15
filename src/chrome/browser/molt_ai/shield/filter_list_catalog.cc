// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/filter_list_catalog.h"

#include <utility>

#include "base/containers/span.h"

namespace molt_ai::shield {

namespace {

// Keep in sync with tools/shield_bench (LISTS) so benchmarks measure what
// ships.
constexpr FilterListEntry kCatalog[] = {
    {"easylist", "EasyList", "https://easylist.to/easylist/easylist.txt",
     "https://raw.githubusercontent.com/easylist/easylist/gh-pages/"
     "easylist.txt",
     /*trusted=*/false, /*enabled_by_default=*/true},
    {"easyprivacy", "EasyPrivacy",
     "https://easylist.to/easylist/easyprivacy.txt",
     "https://raw.githubusercontent.com/easylist/easylist/gh-pages/"
     "easyprivacy.txt",
     false, true},
    {"ublock-filters", "uBlock filters",
     "https://ublockorigin.github.io/uAssets/filters/filters.txt",
     "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/"
     "filters.txt",
     true, true},
    {"ublock-privacy", "uBlock filters – Privacy",
     "https://ublockorigin.github.io/uAssets/filters/privacy.txt",
     "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/"
     "privacy.txt",
     true, true},
    {"ublock-badware", "uBlock filters – Badware risks",
     "https://ublockorigin.github.io/uAssets/filters/badware.txt",
     "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/"
     "badware.txt",
     true, true},
    {"ublock-quick-fixes", "uBlock filters – Quick fixes",
     "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt",
     "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/"
     "quick-fixes.txt",
     true, true},
    {"fanboy-cookiemonster", "EasyList Cookie List",
     "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt",
     /*fallback_url=*/"", false, false},
    {"fanboy-annoyance", "Fanboy's Annoyance List",
     "https://secure.fanboy.co.nz/fanboy-annoyance.txt",
     "https://raw.githubusercontent.com/easylist/easylist/gh-pages/"
     "fanboy-annoyance.txt",
     false, false},
};

}  // namespace

base::span<const FilterListEntry> GetFilterListCatalog() {
  return kCatalog;
}

const FilterListEntry* FindFilterList(std::string_view id) {
  for (const FilterListEntry& entry : kCatalog) {
    if (id == entry.id) {
      return &entry;
    }
  }
  return nullptr;
}

bool IsListEnabled(const FilterListEntry& entry,
                   const ShieldSettings& settings) {
  auto it = settings.list_enabled.find(entry.id);
  return it != settings.list_enabled.end() ? it->second
                                           : entry.enabled_by_default;
}

std::vector<ListSpec> EnabledListSpecs(const StoreState& state) {
  std::vector<ListSpec> specs;
  for (const FilterListEntry& entry : kCatalog) {
    if (!IsListEnabled(entry, state.settings)) {
      continue;
    }
    ListSpec spec;
    spec.id = entry.id;
    spec.trusted = entry.trusted;
    auto it = state.lists.find(entry.id);
    if (it != state.lists.end()) {
      spec.expected_sha256 = it->second.sha256_hex;
    }
    specs.push_back(std::move(spec));
  }
  return specs;
}

}  // namespace molt_ai::shield
