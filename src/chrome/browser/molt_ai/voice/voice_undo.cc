// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_undo.h"

#include <utility>

namespace molt_ai {
namespace voice {

namespace {

// Whether |entry| holds on page |page_id| in tab |active_tab|.
bool Holds(const UndoEntry& entry, int64_t page_id, int active_tab) {
  if (entry.tab >= 0 && entry.tab != active_tab) {
    return false;
  }
  return entry.page_id < 0 || entry.page_id == page_id;
}

}  // namespace

UndoEntry::UndoEntry() = default;
UndoEntry::~UndoEntry() = default;
UndoEntry::UndoEntry(const UndoEntry&) = default;
UndoEntry& UndoEntry::operator=(const UndoEntry&) = default;
UndoEntry::UndoEntry(UndoEntry&&) = default;
UndoEntry& UndoEntry::operator=(UndoEntry&&) = default;

void NoteUndo(std::vector<UndoEntry>& stack, UndoEntry entry) {
  if (entry.kind == UndoKind::kNone || entry.text.size() > kMaxUndoTextBytes ||
      entry.url.size() > kMaxUndoTextBytes ||
      entry.name.size() > kMaxUndoTextBytes) {
    return;
  }
  stack.push_back(std::move(entry));
  while (stack.size() > kMaxUndoEntries) {
    stack.erase(stack.begin());
  }
}

UndoEntry PendingNavigation(int64_t from_page_id, int tab, bool back) {
  UndoEntry entry;
  entry.kind = back ? UndoKind::kGoForward : UndoKind::kGoBack;
  entry.pending = true;
  entry.from_page_id = from_page_id;
  entry.tab = tab;
  return entry;
}

void SettleUndo(std::vector<UndoEntry>& stack,
                int64_t page_id,
                int active_tab,
                bool page_loaded) {
  for (size_t i = stack.size(); i-- > 0;) {
    UndoEntry& entry = stack[i];
    if (!entry.pending) {
      continue;
    }
    if (entry.tab >= 0 && entry.tab != active_tab) {
      // Another tab became active before the page was seen: what it opened
      // can't be told apart from the switch.
      stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    if (page_id != entry.from_page_id) {
      entry.pending = false;
      entry.page_id = page_id;
    } else if (page_loaded) {
      stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(i));
    }
  }
}

UndoTake TakeUndo(std::vector<UndoEntry>& stack,
                  int64_t page_id,
                  int active_tab) {
  UndoTake take;
  if (stack.empty()) {
    return take;
  }
  UndoEntry& top = stack.back();
  if (top.pending) {
    take.in_flight = true;
    return take;
  }
  if (!Holds(top, page_id, active_tab)) {
    stack.pop_back();
    take.stale = true;
    return take;
  }
  take.entry = std::move(top);
  stack.pop_back();
  return take;
}

}  // namespace voice
}  // namespace molt_ai
