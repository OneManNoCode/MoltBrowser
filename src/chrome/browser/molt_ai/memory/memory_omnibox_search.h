// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MemoryOmniboxSearch — a profile's AI memory as the omnibox's @memory
// shortcut sees it (components/omnibox/browser/molt_memory_search.h,
// docs/design/SEMANTIC_SEARCH.md). Owned by MemoryService, which only
// regular profiles have; registered under the profile's PrefService while it
// lives, so an off-the-record, Guest or MoltNet omnibox (another PrefService)
// never finds it. UI thread.

#ifndef CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_OMNIBOX_SEARCH_H_
#define CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_OMNIBOX_SEARCH_H_

#include <cstddef>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/molt_ai/memory/memory_types.h"
#include "components/omnibox/browser/molt_memory_search.h"

class PrefService;

namespace molt_ai {
namespace memory {

class MemoryService;

class MemoryOmniboxSearch : public MoltMemorySearch {
 public:
  // Most pages one search returns (more than the omnibox shows, so it can
  // drop a non-http(s) one and still fill its rows).
  static constexpr size_t kMaxResults = 8;

  // Registers itself under |prefs|; |service| and |prefs| outlive it.
  MemoryOmniboxSearch(MemoryService* service, const PrefService* prefs);
  ~MemoryOmniboxSearch() override;
  MemoryOmniboxSearch(const MemoryOmniboxSearch&) = delete;
  MemoryOmniboxSearch& operator=(const MemoryOmniboxSearch&) = delete;

  // MoltMemorySearch:
  bool IsMemoryEnabled() const override;
  bool IsMemoryPaused() const override;
  void Search(const std::string& query,
              size_t max_results,
              CancelFlag cancelled,
              ResultsCallback callback) override;

  // The omnibox's view of MemoryService's answer: url and title of each hit.
  static Results ToResults(OmniboxSearchResult result);

 private:
  raw_ptr<MemoryService> service_;
  raw_ptr<const PrefService> prefs_;
};

}  // namespace memory
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_MEMORY_MEMORY_OMNIBOX_SEARCH_H_
