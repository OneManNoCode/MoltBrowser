// Copyright 2025 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// FilterListManager — Downloads and manages ad blocking filter lists
// Supports EasyList, EasyPrivacy, uBlock Origin, and custom lists.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molt_ai {

class MoltShield;

// A filter list subscription
struct FilterListInfo {
  std::string id;           // Unique identifier
  std::string name;         // Display name
  std::string url;          // Download URL
  std::string local_path;   // Local cache path
  int rule_count = 0;       // Number of rules loaded
  bool enabled = true;      // Whether this list is active
  bool is_default = false;  // Whether this is a built-in list
  int64_t last_updated = 0; // Unix timestamp of last update
};

// Callback for async operations
using FilterListCallback = std::function<void(bool success,
                                               const std::string& error)>;

// ============================================================
// FilterListManager
// ============================================================
class FilterListManager {
 public:
  explicit FilterListManager(MoltShield* shield);
  ~FilterListManager();

  FilterListManager(const FilterListManager&) = delete;
  FilterListManager& operator=(const FilterListManager&) = delete;

  // Initialize with default filter lists
  void Initialize(const std::string& cache_dir);

  // Get all registered filter lists
  std::vector<FilterListInfo> GetFilterLists() const;

  // Add a custom filter list subscription
  void AddFilterList(const std::string& id,
                     const std::string& name,
                     const std::string& url);

  // Remove a filter list
  void RemoveFilterList(const std::string& id);

  // Enable/disable a filter list
  void SetFilterListEnabled(const std::string& id, bool enabled);

  // Download and update all enabled filter lists
  // TODO: Wire to Chromium's network::SimpleURLLoader
  void UpdateAllLists(FilterListCallback callback = nullptr);

  // Download a specific filter list
  void UpdateList(const std::string& id,
                  FilterListCallback callback = nullptr);

  // Load cached filter lists from disk
  void LoadCachedLists();

  // Get default filter list URLs
  static std::vector<FilterListInfo> GetDefaultLists();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_FILTER_LIST_MANAGER_H_
