// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/filter_list_manager.h"

#include "chrome/browser/molt_ai/shield/molt_shield.h"

#include <algorithm>
#include <fstream>
#include <unordered_map>

#include "base/memory/raw_ptr.h"

namespace molt_ai {

struct FilterListManager::Impl {
  raw_ptr<MoltShield> shield;
  std::string cache_dir;
  std::unordered_map<std::string, FilterListInfo> lists;
};

FilterListManager::FilterListManager(MoltShield* shield)
    : impl_(std::make_unique<Impl>()) {
  impl_->shield = shield;
}

FilterListManager::~FilterListManager() = default;

void FilterListManager::Initialize(const std::string& cache_dir) {
  impl_->cache_dir = cache_dir;

  // Register default lists
  auto defaults = GetDefaultLists();
  for (const auto& list : defaults) {
    impl_->lists[list.id] = list;
  }

  // Load cached lists from disk
  LoadCachedLists();
}

std::vector<FilterListInfo> FilterListManager::GetFilterLists() const {
  std::vector<FilterListInfo> result;
  result.reserve(impl_->lists.size());
  for (const auto& pair : impl_->lists) {
    result.push_back(pair.second);
  }
  return result;
}

void FilterListManager::AddFilterList(const std::string& id,
                                       const std::string& name,
                                       const std::string& url) {
  FilterListInfo info;
  info.id = id;
  info.name = name;
  info.url = url;
  info.enabled = true;
  info.is_default = false;
  impl_->lists[id] = info;
}

void FilterListManager::RemoveFilterList(const std::string& id) {
  auto it = impl_->lists.find(id);
  if (it != impl_->lists.end() && !it->second.is_default) {
    impl_->lists.erase(it);
  }
}

void FilterListManager::SetFilterListEnabled(const std::string& id,
                                              bool enabled) {
  auto it = impl_->lists.find(id);
  if (it != impl_->lists.end()) {
    it->second.enabled = enabled;
  }
}

void FilterListManager::UpdateAllLists(FilterListCallback callback) {
  // TODO: Wire to Chromium's network::SimpleURLLoader for async downloads.
  // For each enabled list:
  //   1. Download list content from list.url
  //   2. Save to cache_dir/list.id.txt
  //   3. Call shield->LoadFilterList(list.id, content)
  //   4. Update list.rule_count and list.last_updated

  // For now, just load from cache
  LoadCachedLists();

  if (callback) {
    callback(true, "");
  }
}

void FilterListManager::UpdateList(const std::string& id,
                                    FilterListCallback callback) {
  auto it = impl_->lists.find(id);
  if (it == impl_->lists.end()) {
    if (callback) callback(false, "List not found: " + id);
    return;
  }

  // TODO: Download from it->second.url using SimpleURLLoader
  // For now, try loading from cache
  std::string cache_path = impl_->cache_dir + "/" + id + ".txt";
  int count = impl_->shield->LoadFilterListFromFile(id, cache_path);
  if (count > 0) {
    it->second.rule_count = count;
    if (callback) callback(true, "");
  } else {
    if (callback) callback(false, "Failed to load: " + cache_path);
  }
}

void FilterListManager::LoadCachedLists() {
  for (auto& pair : impl_->lists) {
    if (!pair.second.enabled) continue;

    std::string cache_path =
        impl_->cache_dir + "/" + pair.first + ".txt";
    int count = impl_->shield->LoadFilterListFromFile(
        pair.first, cache_path);
    if (count > 0) {
      pair.second.rule_count = count;
    }
  }
}

std::vector<FilterListInfo> FilterListManager::GetDefaultLists() {
  return {
      {"easylist", "EasyList",
       "https://easylist.to/easylist/easylist.txt",
       "", 0, true, true, 0},
      {"easyprivacy", "EasyPrivacy",
       "https://easylist.to/easylist/easyprivacy.txt",
       "", 0, true, true, 0},
      {"ublock_filters", "uBlock Filters",
       "https://ublockorigin.github.io/uAssets/filters/filters.txt",
       "", 0, true, true, 0},
      {"ublock_badware", "uBlock Badware",
       "https://ublockorigin.github.io/uAssets/filters/badware.txt",
       "", 0, true, true, 0},
      {"ublock_privacy", "uBlock Privacy",
       "https://ublockorigin.github.io/uAssets/filters/privacy.txt",
       "", 0, true, true, 0},
      {"ublock_quick_fixes", "uBlock Quick Fixes",
       "https://ublockorigin.github.io/uAssets/filters/quick-fixes.txt",
       "", 0, true, true, 0},
      {"ublock_annoyances", "uBlock Annoyances",
       "https://ublockorigin.github.io/uAssets/filters/annoyances-others.txt",
       "", 0, true, true, 0},
      {"peter_lowe", "Peter Lowe's Ad Server List",
       "https://pgl.yoyo.org/adservers/serverlist.php?hostformat=adblockplus&showintro=1&mimetype=plaintext",
       "", 0, true, true, 0},
  };
}

}  // namespace molt_ai
