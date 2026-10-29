// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The page map's lists and items as the spoken-choice matcher's
// candidates (docs/design/TOUCHLESS_BROWSING.md §6.3, §6.4): what "the
// fourth one", "number 12" or "the live one by Queen" is chosen from.
//
// Each candidate keeps what the browser acts on when it is chosen (the
// node and the kind of item) next to what the matcher reads (position,
// on-screen number, coarse role, title and fields). All text comes from
// the page map, so it already passed SpeechSafeText(); it is page text and
// is only ever read out and compared, never taken as a command.
//
// Numbers. A list is read aloud as "1: ..., 2: ..." by position. While the
// numbers overlay is shown, the browser reads and matches the numbers on
// screen instead (each entry's item number), so what is heard is what is
// seen: CandidatesFromList() then sets |screen_number|, and "number 23"
// means the item showing 23. Items (buttons, links, fields) are always
// chosen by their item numbers.
//
// Std-only (no //base): unit-tested in molt_ai_unittests.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CANDIDATES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CANDIDATES_H_

#include <cstddef>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"
#include "chrome/browser/molt_ai/voice/spoken_choice.h"

namespace molt_ai {
namespace voice {

// What a candidate is, for the browser: how to act on it.
enum class CandidateSource : uint8_t {
  kListEntry,  // an entry of PageMap::lists
  kItem,       // a PageItem
  kHeading,    // a PageHeading
  kTab,        // a browser tab
};

// The browser's side of one candidate, at the same index as the
// matcher's ChoiceCandidate.
struct CandidateTarget {
  CandidateSource source = CandidateSource::kItem;
  // The node to act on: the entry's primary action, the item, the
  // heading. Invalid for an entry with nothing to act on, and for tabs.
  PageNodeHandle handle;
  // The item number of |handle| (0 for none), and its kind.
  int item_number = 0;
  ItemKind kind = ItemKind::kClickable;
  // kListEntry: the entry's index in its list. kTab: the tab's index.
  int index = 0;
  // A link's target (raw page data), for the confirmation check.
  std::string url;
};

struct CandidateSet {
  CandidateSet();
  ~CandidateSet();
  CandidateSet(const CandidateSet&);
  CandidateSet& operator=(const CandidateSet&);
  CandidateSet(CandidateSet&&);
  CandidateSet& operator=(CandidateSet&&);

  size_t size() const { return candidates.size(); }
  bool empty() const { return candidates.empty(); }

  std::vector<ChoiceCandidate> candidates;
  std::vector<CandidateTarget> targets;  // same size
};

// A browser tab, for "switch to the YouTube tab".
struct TabInfo {
  std::string title;  // cleaned by the browser before it gets here
  std::string host;
};

// The role a list entry has for role words ("the first video"): kArticle
// for a feed's entries, kVideo for an entry that shows a duration,
// kListItem otherwise.
ChoiceRole EntryRole(const PageList& list, const PageListEntry& entry);

// The role of an item kind (spoken_choice.h's mapping from the tree).
ChoiceRole ItemRole(ItemKind kind);

// |list|'s entries, by position. With |numbers_shown|, each entry's
// on-screen number is its item number. Fields: the channel (author), the
// date, the duration, the counts and the other texts (details), at most
// kMaxChoiceFields.
CandidateSet CandidatesFromList(const PageMap& map,
                                const PageList& list,
                                bool numbers_shown);

// Every item of the map, by item number (position and on-screen number
// are both the item number), on screen or not, disabled or not (pressing
// a disabled one says so).
CandidateSet CandidatesFromItems(const PageMap& map);

// The links among the map's items, by position in reading order; the
// on-screen number is the item number when |numbers_shown|.
CandidateSet CandidatesFromLinks(const PageMap& map, bool numbers_shown);

// The headings, by position.
CandidateSet CandidatesFromHeadings(const PageMap& map);

// The window's tabs, by position (1-based tab order).
CandidateSet CandidatesFromTabs(const std::vector<TabInfo>& tabs);

// The candidates of |set| at |indices| (into |set|), renumbered 1..n in
// that order, without on-screen numbers: the close choices the browser
// read back ("Did you mean 1: ..., 2: ...?").
CandidateSet SubsetOf(const CandidateSet& set,
                      const std::vector<size_t>& indices);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_CANDIDATES_H_
