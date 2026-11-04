// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Undo" in voice mode (docs/design/TOUCHLESS_BROWSING.md §2, "Recover";
// §5.6): what voice mode did last, put back. A short stack of what voice
// mode itself did in the window, newest last:
//
//   - it closed a tab: the tab is reopened (the one voice mode closed, from
//     the browser's recently closed tabs);
//   - it typed into, cleared or edited a field: the field gets back the
//     text it held, while the page is the same;
//   - it opened a page (a site, a search, a link, a result): back to the
//     page before, while the page it opened is the one shown; after "go
//     back", forward again;
//   - it bookmarked a page, or removed a bookmark: the bookmark is removed,
//     or put back.
//
// Only the newest entry is undone. One that no longer holds (the field's
// page is gone, the page voice mode opened isn't the one shown) is dropped
// and the browser says it can't undo that any more; a navigation that never
// happened (a link that opened nothing) is dropped without a word. What the
// user did by hand, and what can't be put back (a form sent, a purchase),
// is never on the stack.
//
// Pure and std-only (unit-tested in the voice harness and
// molt_ai_unittests); the voice loop (voice_controller_logic.h) notes the
// entries and plans the steps that undo them.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_UNDO_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_UNDO_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {

// Entries kept, at most; the oldest goes first.
inline constexpr size_t kMaxUndoEntries = 5;
// An entry's text (a field's old value, a bookmark's title), in bytes.
inline constexpr size_t kMaxUndoTextBytes = 4096;

enum class UndoKind : uint8_t {
  kNone,
  kReopenTab,        // voice mode closed a tab (|name|: its title)
  kRestoreText,      // voice mode changed |field|'s text (|text|: before)
  kGoBack,           // voice mode opened a page: back to the one before
  kGoForward,        // voice mode went back: forward again
  kRemoveBookmark,   // voice mode bookmarked |url|
  kRestoreBookmark,  // voice mode removed |url|'s bookmark (|text|: title)
};

struct UndoEntry {
  UndoEntry();
  ~UndoEntry();
  UndoEntry(const UndoEntry&);
  UndoEntry& operator=(const UndoEntry&);
  UndoEntry(UndoEntry&&);
  UndoEntry& operator=(UndoEntry&&);

  UndoKind kind = UndoKind::kNone;
  // The page it holds on: the field's (kRestoreText), the page opened
  // (kGoBack, kGoForward, once settled). -1 for an entry that holds
  // anywhere (a tab, a bookmark).
  int64_t page_id = -1;
  // A navigation noted when it was asked for: |from_page_id| is the page
  // it was asked on; it settles once another page is seen.
  bool pending = false;
  int64_t from_page_id = -1;
  // The active tab it was done in (-1: any).
  int tab = -1;
  // kRestoreText: the field.
  PageNodeHandle field;
  // kRestoreText: the text the field held; bookmarks: the page's title.
  std::string text;
  // What the browser names when it undoes it: the field, the tab's title
  // (speech-safe).
  std::string name;
  // Bookmarks: the page's address.
  std::string url;
};

// Adds |entry| on top, dropping the oldest past kMaxUndoEntries. Its text
// is capped at kMaxUndoTextBytes (never cut inside a character): an entry
// whose text was longer isn't kept, as putting back part of it would be
// wrong.
void NoteUndo(std::vector<UndoEntry>& stack, UndoEntry entry);

// A navigation voice mode asked for on page |from_page_id| in tab |tab|:
// pending until another page is seen (SettleUndo()). |back| for "go back"
// (undone by going forward).
UndoEntry PendingNavigation(int64_t from_page_id, int tab, bool back);

// Settles the pending navigations against page |page_id| in tab
// |active_tab|: one asked on another page now holds on this one; one asked
// on this very page holds still when |page_loaded| is false (its page may
// be on its way), and is dropped when it is true (the wait for it ended
// here: nothing was opened).
void SettleUndo(std::vector<UndoEntry>& stack,
                int64_t page_id,
                int active_tab,
                bool page_loaded);

// What "undo" finds on top of the stack.
struct UndoTake {
  // The entry to undo, taken off the stack.
  std::optional<UndoEntry> entry;
  // The newest entry no longer held, and was dropped (the browser says it
  // can't undo that any more).
  bool stale = false;
  // The newest entry is a navigation whose page hasn't been seen yet: it
  // stays (the browser asks to wait until the page has opened).
  bool in_flight = false;
};

// The newest entry, taken off the stack, if it holds on page |page_id| in
// tab |active_tab|.
UndoTake TakeUndo(std::vector<UndoEntry>& stack,
                  int64_t page_id,
                  int active_tab);

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_UNDO_H_
