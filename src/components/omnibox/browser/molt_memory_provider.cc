// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "components/omnibox/browser/molt_memory_provider.h"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/escape.h"
#include "base/strings/utf_string_conversions.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_classification.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider_client.h"
#include "components/url_formatter/url_formatter.h"
#include "url/gurl.h"

namespace {

using molt_memory::ShortcutState;

// Relevance of the hint row for "@m".."@memor": below what the other
// providers' default match usually gets, so it never takes Enter.
constexpr int kHintRelevance = 900;

}  // namespace

MoltMemoryProvider::MoltMemoryProvider(AutocompleteProviderClient* client,
                                       AutocompleteProviderListener* listener)
    : AutocompleteProvider(AutocompleteProvider::Type::kBuiltin),
      client_(client) {
  AddListener(listener);
}

MoltMemoryProvider::~MoltMemoryProvider() {
  if (cancelled_) {
    cancelled_->store(true);
  }
}

ShortcutState MoltMemoryProvider::State(MoltMemorySearch** search) const {
  *search = nullptr;
  if (!client_) {
    return ShortcutState::kHidden;
  }
  const bool off_the_record = client_->IsOffTheRecord();
  const bool guest = client_->IsGuestSession();
  // Off-the-record and Guest omniboxes don't even look (their PrefService
  // never has a memory registered anyway).
  MoltMemorySearch* found = (off_the_record || guest)
                                ? nullptr
                                : MoltMemorySearch::ForPrefs(client_->GetPrefs());
  const ShortcutState state = molt_memory::ShortcutStateFor(
      off_the_record, guest, found != nullptr,
      found && found->IsMemoryEnabled(), found && found->IsMemoryPaused());
  if (state == ShortcutState::kReady) {
    *search = found;
  }
  return state;
}

void MoltMemoryProvider::Start(const AutocompleteInput& input,
                               bool minimal_changes) {
  // Whatever the last search has still to say is for another input.
  CancelSearch();
  done_ = true;
  matches_.clear();
  query_.clear();
  input_ = input;

  if (input.IsZeroSuggest()) {
    return;
  }
  const bool shortcut = molt_memory::IsShortcutInput(input.text());
  if (!shortcut && !molt_memory::IsShortcutPrefix(input.text())) {
    return;
  }

  MoltMemorySearch* search = nullptr;
  const ShortcutState state = State(&search);
  if (state == ShortcutState::kHidden) {
    return;  // Not even a hint.
  }
  if (!shortcut) {
    if (state == ShortcutState::kReady) {
      matches_.push_back(HintRow(/*allowed_to_be_default=*/false));
    }
    return;
  }
  if (state != ShortcutState::kReady) {
    // Memory is off or paused: say so; nothing is searched.
    matches_.push_back(StateRow(state));
    return;
  }

  query_ = molt_memory::ShortcutQuery(input.text());
  if (query_.empty()) {
    // "@memory" alone: Enter opens molt://memory.
    matches_.push_back(HintRow(/*allowed_to_be_default=*/true));
    return;
  }
  matches_.push_back(SearchRow());
  if (input.omit_asynchronous_matches()) {
    return;
  }
  done_ = false;
  cancelled_ = std::make_shared<std::atomic<bool>>(false);
  search->Search(query_, kRequestedHits, cancelled_,
                 base::BindRepeating(&MoltMemoryProvider::OnResults,
                                     weak_factory_.GetWeakPtr()));
}

void MoltMemoryProvider::Stop(AutocompleteStopReason stop_reason) {
  // Includes the controller's stop timer: results by meaning that arrive
  // later are dropped, never shown after the popup settled.
  CancelSearch();
  AutocompleteProvider::Stop(stop_reason);
}

void MoltMemoryProvider::CancelSearch() {
  if (cancelled_) {
    cancelled_->store(true);
    cancelled_.reset();
  }
  weak_factory_.InvalidateWeakPtrs();
}

void MoltMemoryProvider::OnResults(MoltMemorySearch::Results results) {
  if (done_ || query_.empty()) {
    return;
  }
  matches_.clear();
  matches_.push_back(SearchRow());
  AddHitRows(results);
  done_ = results.final;
  NotifyListeners(/*updated_matches=*/true);
}

AutocompleteMatch MoltMemoryProvider::SearchRow() {
  AutocompleteMatch match(this, molt_memory::kSearchRowRelevance,
                          /*deletable=*/false,
                          omnibox::AutocompleteMatchType::kNavsuggest);
  match.destination_url = GURL(molt_memory::MemorySearchUrl(query_));
  match.fill_into_edit = input_.text();
  match.contents = u"Search memory for \u201C" + base::UTF8ToUTF16(query_) +
                   u"\u201D";
  match.contents_class = {{0, ACMatchClassification::NONE}};
  match.allowed_to_be_default_match = true;
  return match;
}

AutocompleteMatch MoltMemoryProvider::HintRow(
    bool allowed_to_be_default) {
  AutocompleteMatch match(
      this,
      allowed_to_be_default ? molt_memory::kSearchRowRelevance
                            : kHintRelevance,
      /*deletable=*/false, omnibox::AutocompleteMatchType::kNavsuggest);
  match.destination_url = GURL(molt_memory::kMemoryPageUrl);
  match.fill_into_edit = std::u16string(molt_memory::kShortcutKeyword) + u" ";
  match.contents = std::u16string(molt_memory::kShortcutKeyword) +
                   u" \u2014 search your AI memory on this device";
  match.contents_class = {{0, ACMatchClassification::NONE}};
  match.allowed_to_be_default_match = allowed_to_be_default;
  return match;
}

AutocompleteMatch MoltMemoryProvider::StateRow(ShortcutState state) {
  AutocompleteMatch match(this, molt_memory::kSearchRowRelevance,
                          /*deletable=*/false,
                          omnibox::AutocompleteMatchType::kNavsuggest);
  match.destination_url = GURL(molt_memory::kMemoryPageUrl);
  match.fill_into_edit = input_.text();
  match.contents = state == ShortcutState::kPaused
                       ? u"Memory is paused \u2014 open Memory to resume it"
                       : u"Memory is off \u2014 open Memory to turn it on";
  match.contents_class = {{0, ACMatchClassification::NONE}};
  match.allowed_to_be_default_match = true;
  return match;
}

void MoltMemoryProvider::AddHitRows(const MoltMemorySearch::Results& results) {
  const std::u16string query = base::UTF8ToUTF16(query_);
  const url_formatter::FormatUrlTypes format_types =
      AutocompleteMatch::GetFormatTypes(/*preserve_scheme=*/false,
                                        /*preserve_subdomain=*/false);
  for (const molt_memory::SuggestionRow& row :
       molt_memory::SuggestionRows(results.hits, kMaxHits)) {
    const GURL url(row.url);
    // Only ever a page: never javascript:, data:, file: or chrome:.
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    // A page found in memory. kHistoryBody ("a past page whose contents
    // match") has a page icon and no special handling.
    AutocompleteMatch match(this, row.relevance, /*deletable=*/false,
                            omnibox::AutocompleteMatchType::kHistoryBody);
    match.destination_url = url;
    match.allowed_to_be_default_match = false;

    // Text only: the title and URL are classified for bolding and never
    // parsed. url_formatter shows the host safely (IDN spoof checks).
    match.contents = url_formatter::FormatUrl(
        url, format_types, base::UnescapeRule::SPACES, nullptr, nullptr,
        nullptr);
    match.contents_class = ClassifyTermMatches(
        FindTermMatches(query, match.contents), match.contents.size(),
        ACMatchClassification::MATCH | ACMatchClassification::URL,
        ACMatchClassification::URL);
    match.description =
        AutocompleteInput::SanitizeString(base::UTF8ToUTF16(row.title));
    match.description_class = ClassifyTermMatches(
        FindTermMatches(query, match.description), match.description.size(),
        ACMatchClassification::MATCH, ACMatchClassification::NONE);
    match.fill_into_edit =
        AutocompleteInput::FormattedStringWithEquivalentMeaning(
            url, url_formatter::FormatUrl(url), client_->GetSchemeClassifier(),
            nullptr);
    matches_.push_back(std::move(match));
  }
}
