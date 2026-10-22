// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/memory_omnibox_search.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/memory/memory_service.h"

namespace molt_ai {
namespace memory {

MemoryOmniboxSearch::MemoryOmniboxSearch(MemoryService* service,
                                         const PrefService* prefs)
    : service_(service), prefs_(prefs) {
  MoltMemorySearch::Register(prefs_, this);
}

MemoryOmniboxSearch::~MemoryOmniboxSearch() {
  MoltMemorySearch::Unregister(prefs_, this);
}

bool MemoryOmniboxSearch::IsMemoryEnabled() const {
  return service_->IsEnabled();
}

bool MemoryOmniboxSearch::IsMemoryPaused() const {
  return service_->IsPaused();
}

void MemoryOmniboxSearch::Search(const std::string& query,
                                 size_t max_results,
                                 CancelFlag cancelled,
                                 ResultsCallback callback) {
  std::function<bool()> still_wanted;
  if (cancelled) {
    still_wanted = [cancelled]() { return !cancelled->load(); };
  }
  service_->SearchForOmnibox(
      query, static_cast<int>(std::min(max_results, kMaxResults)),
      std::move(still_wanted),
      base::BindRepeating(
          [](const ResultsCallback& callback, OmniboxSearchResult result) {
            callback.Run(ToResults(std::move(result)));
          },
          std::move(callback)));
}

// static
MoltMemorySearch::Results MemoryOmniboxSearch::ToResults(
    OmniboxSearchResult result) {
  Results out;
  out.semantic = result.semantic;
  out.final = result.final;
  out.hits.reserve(result.hits.size());
  for (QueryHit& hit : result.hits) {
    molt_memory::MemoryHit h;
    h.url = std::move(hit.url);
    h.title = std::move(hit.title);
    out.hits.push_back(std::move(h));
  }
  return out;
}

}  // namespace memory
}  // namespace molt_ai
