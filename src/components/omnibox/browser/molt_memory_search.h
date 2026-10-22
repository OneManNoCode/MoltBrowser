// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltMemorySearch — how the @memory omnibox shortcut (MoltMemoryProvider)
// reaches a profile's AI memory, which lives in //chrome
// (chrome/browser/molt_ai/memory/memory_omnibox_search.h implements it).
//
// Each profile that has a memory (regular profiles only: never Incognito,
// Guest, MoltNet or any other off-the-record profile) registers its search
// under its own PrefService, the one AutocompleteProviderClient::GetPrefs()
// returns for an omnibox of that profile. An off-the-record or Guest
// profile's omnibox has a PrefService of its own, under which nothing is
// registered, so it finds no memory. This keeps the plumbing out of
// upstream's AutocompleteProviderClient (docs/design/SEMANTIC_SEARCH.md).
//
// Everything here runs on the UI thread.

#ifndef COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_SEARCH_H_
#define COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_SEARCH_H_

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "components/omnibox/browser/molt_memory_query.h"

class PrefService;

class MoltMemorySearch {
 public:
  struct Results {
    Results();
    ~Results();
    Results(Results&&);
    Results& operator=(Results&&);
    Results(const Results&);
    Results& operator=(const Results&);

    // Best first. Untrusted page data: see molt_memory::SuggestionRows().
    std::vector<molt_memory::MemoryHit> hits;
    // Ranked by the on-device search model (else by matching words).
    bool semantic = false;
    // No further results follow for this search.
    bool final = true;
  };
  using ResultsCallback = base::RepeatingCallback<void(Results)>;
  // Set by the caller once it no longer wants a search's results; read on
  // the memory worker, so a stale search doesn't even run.
  using CancelFlag = std::shared_ptr<std::atomic<bool>>;

  virtual ~MoltMemorySearch() = default;

  // Memory's settings in this profile.
  virtual bool IsMemoryEnabled() const = 0;
  virtual bool IsMemoryPaused() const = 0;

  // Searches this profile's memory for |query| (molt_memory::NormalizeQuery()
  // made, non-empty). |callback| runs on this thread, never synchronously:
  // first with hits ranked by matching words; then, only when the on-device
  // search model is already loaded (it is never started for this), with hits
  // ranked by meaning. The last call has |final| set. Once |cancelled| is
  // set, nothing more is searched (bind |callback| to a WeakPtr too). The
  // query never leaves the browser and the sandboxed model process.
  virtual void Search(const std::string& query,
                      size_t max_results,
                      CancelFlag cancelled,
                      ResultsCallback callback) = 0;

  // The search registered for |prefs|, or nullptr.
  static MoltMemorySearch* ForPrefs(const PrefService* prefs);
  // |search| must stay alive until it unregisters.
  static void Register(const PrefService* prefs, MoltMemorySearch* search);
  static void Unregister(const PrefService* prefs, MoltMemorySearch* search);
};

#endif  // COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_SEARCH_H_
