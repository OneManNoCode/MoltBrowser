// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_candidates.h"

#include <algorithm>
#include <utility>

namespace molt_ai {
namespace voice {

namespace {

void AddField(ChoiceCandidate& candidate,
              ChoiceFieldKind kind,
              const std::string& text) {
  if (text.empty() || candidate.fields.size() >= kMaxChoiceFields) {
    return;
  }
  candidate.fields.emplace_back(kind, text);
}

void Add(CandidateSet& set, ChoiceCandidate candidate, CandidateTarget target) {
  set.candidates.push_back(std::move(candidate));
  set.targets.push_back(std::move(target));
}

CandidateTarget ItemTarget(const PageItem& item) {
  CandidateTarget target;
  target.source = CandidateSource::kItem;
  target.handle = item.handle;
  target.item_number = item.number;
  target.kind = item.kind;
  target.index = item.number - 1;
  target.url = item.url;
  return target;
}

ChoiceCandidate ItemCandidate(const PageItem& item,
                              int position,
                              int screen_number) {
  ChoiceCandidate candidate;
  candidate.position = position;
  candidate.screen_number = screen_number;
  candidate.role = ItemRole(item.kind);
  candidate.title = item.label;
  // A select's or slider's current choice tells such items apart; a text
  // field's value is the user's own text (never a credential field's: the
  // map leaves those out).
  if (item.kind == ItemKind::kSelect || item.kind == ItemKind::kSlider) {
    AddField(candidate, ChoiceFieldKind::kDetail, item.value);
  }
  return candidate;
}

}  // namespace

CandidateSet::CandidateSet() = default;
CandidateSet::~CandidateSet() = default;
CandidateSet::CandidateSet(const CandidateSet&) = default;
CandidateSet& CandidateSet::operator=(const CandidateSet&) = default;
CandidateSet::CandidateSet(CandidateSet&&) = default;
CandidateSet& CandidateSet::operator=(CandidateSet&&) = default;

ChoiceRole EntryRole(const PageList& list, const PageListEntry& entry) {
  if (list.kind == ListKind::kFeed) {
    return ChoiceRole::kArticle;
  }
  if (entry.duration_seconds >= 0) {
    return ChoiceRole::kVideo;
  }
  return ChoiceRole::kListItem;
}

ChoiceRole ItemRole(ItemKind kind) {
  switch (kind) {
    case ItemKind::kLink:
      return ChoiceRole::kLink;
    case ItemKind::kButton:
      return ChoiceRole::kButton;
    case ItemKind::kTextField:
      return ChoiceRole::kTextField;
    case ItemKind::kCheckBox:
    case ItemKind::kSwitch:
      return ChoiceRole::kCheckBox;
    case ItemKind::kRadio:
      return ChoiceRole::kRadioButton;
    case ItemKind::kSelect:
      return ChoiceRole::kListBox;
    case ItemKind::kOption:
    case ItemKind::kMenuItem:
      return ChoiceRole::kOption;
    case ItemKind::kTab:
      return ChoiceRole::kTab;
    case ItemKind::kMedia:
      return ChoiceRole::kVideo;
    case ItemKind::kTreeItem:
    case ItemKind::kSlider:
    case ItemKind::kClickable:
      return ChoiceRole::kOther;
  }
  return ChoiceRole::kOther;
}

CandidateSet CandidatesFromList(const PageMap& map,
                                const PageList& list,
                                bool numbers_shown) {
  CandidateSet set;
  const size_t count = std::min(list.entries.size(), kMaxChoiceCandidates);
  set.candidates.reserve(count);
  set.targets.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const PageListEntry& entry = list.entries[i];
    ChoiceCandidate candidate;
    candidate.position = static_cast<int>(i + 1);
    candidate.screen_number = numbers_shown ? entry.number : 0;
    candidate.role = EntryRole(list, entry);
    candidate.title = entry.title;
    AddField(candidate, ChoiceFieldKind::kAuthor, entry.channel);
    AddField(candidate, ChoiceFieldKind::kDate, entry.date);
    AddField(candidate, ChoiceFieldKind::kDuration, entry.duration);
    for (const EntryCount& count_field : entry.counts) {
      AddField(candidate, ChoiceFieldKind::kCount, count_field.text);
    }
    for (const std::string& text : entry.texts) {
      AddField(candidate, ChoiceFieldKind::kDetail, text);
    }

    CandidateTarget target;
    target.source = CandidateSource::kListEntry;
    target.index = static_cast<int>(i);
    if (entry.number > 0) {
      target.handle = entry.handle;
      target.item_number = entry.number;
      target.kind = entry.kind;
      if (const PageItem* item = map.ItemByNumber(entry.number)) {
        target.url = item->url;
      }
    }
    Add(set, std::move(candidate), std::move(target));
  }
  return set;
}

CandidateSet CandidatesFromItems(const PageMap& map) {
  CandidateSet set;
  const size_t count = std::min(map.items.size(), kMaxChoiceCandidates);
  set.candidates.reserve(count);
  set.targets.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const PageItem& item = map.items[i];
    Add(set, ItemCandidate(item, item.number, item.number), ItemTarget(item));
  }
  return set;
}

CandidateSet CandidatesFromLinks(const PageMap& map, bool numbers_shown) {
  CandidateSet set;
  int position = 0;
  for (const PageItem& item : map.items) {
    if (item.kind != ItemKind::kLink) {
      continue;
    }
    if (set.size() >= kMaxChoiceCandidates) {
      break;
    }
    ++position;
    Add(set, ItemCandidate(item, position, numbers_shown ? item.number : 0),
        ItemTarget(item));
  }
  return set;
}

CandidateSet CandidatesFromHeadings(const PageMap& map) {
  CandidateSet set;
  const size_t count = std::min(map.headings.size(), kMaxChoiceCandidates);
  for (size_t i = 0; i < count; ++i) {
    const PageHeading& heading = map.headings[i];
    ChoiceCandidate candidate;
    candidate.position = static_cast<int>(i + 1);
    candidate.role = ChoiceRole::kHeading;
    candidate.title = heading.text;
    CandidateTarget target;
    target.source = CandidateSource::kHeading;
    target.handle = heading.handle;
    target.index = static_cast<int>(i);
    Add(set, std::move(candidate), std::move(target));
  }
  return set;
}

CandidateSet CandidatesFromTabs(const std::vector<TabInfo>& tabs) {
  CandidateSet set;
  const size_t count = std::min(tabs.size(), kMaxChoiceCandidates);
  for (size_t i = 0; i < count; ++i) {
    ChoiceCandidate candidate;
    candidate.position = static_cast<int>(i + 1);
    candidate.role = ChoiceRole::kTab;
    candidate.title = tabs[i].title;
    AddField(candidate, ChoiceFieldKind::kSource, tabs[i].host);
    CandidateTarget target;
    target.source = CandidateSource::kTab;
    target.index = static_cast<int>(i);
    Add(set, std::move(candidate), std::move(target));
  }
  return set;
}

CandidateSet SubsetOf(const CandidateSet& set,
                      const std::vector<size_t>& indices) {
  CandidateSet subset;
  for (size_t index : indices) {
    if (index >= set.size()) {
      continue;
    }
    ChoiceCandidate candidate = set.candidates[index];
    candidate.position = static_cast<int>(subset.size() + 1);
    candidate.screen_number = 0;
    Add(subset, std::move(candidate), set.targets[index]);
  }
  return subset;
}

}  // namespace voice
}  // namespace molt_ai
