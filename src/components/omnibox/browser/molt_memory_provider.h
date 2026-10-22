// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltMemoryProvider — the @memory omnibox shortcut: "@memory <words>"
// searches the profile's AI memory on this device
// (docs/design/SEMANTIC_SEARCH.md, "The @memory omnibox shortcut").
//
//   * Default row: "Search memory for <words>", opening
//     molt://memory/?q=<words>. Below it, the best few pages (title and URL);
//     choosing one opens the page.
//   * Ranked by matching words first, quickly; when the on-device search
//     model is already loaded, by meaning once that answer arrives, if it
//     arrives before the controller's stop timer (1.5 s after the last
//     keystroke). The model is never loaded for typing.
//   * Nothing at all in off-the-record profiles (Incognito, MoltNet), the
//     Guest session, or a profile without memory. While Memory is off or
//     paused, one row saying so (opening molt://memory) and no search.
//   * "@m".."@memor" get a hint row where the shortcut works.
//
// AutocompleteController::ShouldRunProvider() gives "@memory ..." input to
// this provider alone (molt_memory::IsShortcutInput()), so no search engine,
// extension or other provider ever sees it, and chrome_omnibox_client.cc
// keeps it from the action predictor (preconnect/prerender) and the
// shortcuts database. Page titles are untrusted: they are normalized
// (molt_memory::SuggestionRows()) and only ever become match text with
// classifications; URLs must be http(s).

#ifndef COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_PROVIDER_H_
#define COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_PROVIDER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/omnibox/browser/autocomplete_enums.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/omnibox/browser/molt_memory_query.h"
#include "components/omnibox/browser/molt_memory_search.h"

class AutocompleteProviderClient;
class AutocompleteProviderListener;

class MoltMemoryProvider : public AutocompleteProvider {
 public:
  // Most pages shown under the default row, and how many are asked for (a
  // few spare in case some can't be shown).
  static constexpr size_t kMaxHits = 5;
  static constexpr size_t kRequestedHits = 8;

  MoltMemoryProvider(AutocompleteProviderClient* client,
                     AutocompleteProviderListener* listener);
  MoltMemoryProvider(const MoltMemoryProvider&) = delete;
  MoltMemoryProvider& operator=(const MoltMemoryProvider&) = delete;

  // AutocompleteProvider:
  void Start(const AutocompleteInput& input, bool minimal_changes) override;
  void Stop(AutocompleteStopReason stop_reason) override;

 private:
  ~MoltMemoryProvider() override;

  // What the shortcut does in this omnibox's profile, and its memory search
  // when it may search.
  molt_memory::ShortcutState State(MoltMemorySearch** search) const;

  void OnResults(MoltMemorySearch::Results results);
  // Drops the search in progress, if any.
  void CancelSearch();

  AutocompleteMatch SearchRow();
  AutocompleteMatch HintRow(bool allowed_to_be_default);
  AutocompleteMatch StateRow(molt_memory::ShortcutState state);
  // Rows for |results| (no match for a hit whose URL isn't http(s)).
  void AddHitRows(const MoltMemorySearch::Results& results);

  raw_ptr<AutocompleteProviderClient> client_;
  // The input and normalized query of the search in progress.
  AutocompleteInput input_;
  std::string query_;
  // Set when that search is no longer wanted (see MoltMemorySearch).
  MoltMemorySearch::CancelFlag cancelled_;

  // Invalidated by every Start() and Stop(): a search's late results are
  // dropped.
  base::WeakPtrFactory<MoltMemoryProvider> weak_factory_{this};
};

#endif  // COMPONENTS_OMNIBOX_BROWSER_MOLT_MEMORY_PROVIDER_H_
