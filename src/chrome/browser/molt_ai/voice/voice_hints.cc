// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_hints.h"

#include <algorithm>
#include <utility>

#include "chrome/browser/molt_ai/voice/page_map_text.h"
#include "chrome/browser/molt_ai/voice/spoken_text.h"
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr int64_t kFutureSlackSeconds = 24 * 3600;

// Names compare as voice mode compares names: folded.
bool SameName(std::string_view label, const std::string& hint) {
  return !hint.empty() && FoldForComparison(label) == FoldForComparison(hint);
}

const PageItem* HintedField(const PageMap& map, const std::string& name) {
  for (const PageItem& item : map.items) {
    // As FindSearchField(): a writable search field of the page itself.
    if (item.kind == ItemKind::kTextField && item.writable &&
        item.search_field && map.InPage(item.handle) &&
        SameName(item.label, name)) {
      return &item;
    }
  }
  return nullptr;
}

const PageList* HintedList(const PageMap& map, const std::string& name) {
  for (const PageList& list : map.lists) {
    if (!list.in_navigation && !list.entries.empty() &&
        SameName(list.label, name)) {
      return &list;
    }
  }
  return nullptr;
}

const PageMedia* HintedMedia(const PageMap& map, const std::string& name) {
  for (const PageMedia& media : map.media) {
    if (SameName(media.label, name)) {
      return &media;
    }
  }
  return nullptr;
}

LocatedPart Generic(SitePart part, const PageMap& map) {
  LocatedPart found;
  switch (part) {
    case SitePart::kSearchField:
      found.field = FindSearchField(map);
      break;
    case SitePart::kResultList:
      for (const PageList& list : map.lists) {
        if (!list.in_navigation && !list.entries.empty()) {
          found.list = &list;
          break;
        }
      }
      break;
    case SitePart::kPlayer:
      if (!map.media.empty()) {
        found.media = &map.media[0];
      }
      break;
  }
  if (found.found()) {
    found.source = PartSource::kGeneric;
  }
  return found;
}

bool PartMatches(SitePart part, const LocatedPart& found) {
  switch (part) {
    case SitePart::kSearchField:
      return found.field != nullptr;
    case SitePart::kResultList:
      return found.list != nullptr;
    case SitePart::kPlayer:
      return found.media != nullptr;
  }
  return false;
}

}  // namespace

std::string_view SitePartName(SitePart part) {
  switch (part) {
    case SitePart::kSearchField:
      return "search field";
    case SitePart::kResultList:
      return "results list";
    case SitePart::kPlayer:
      return "player";
  }
  return "";
}

SiteHint::SiteHint() = default;
SiteHint::~SiteHint() = default;
SiteHint::SiteHint(const SiteHint&) = default;
SiteHint& SiteHint::operator=(const SiteHint&) = default;
SiteHint::SiteHint(SiteHint&&) = default;
SiteHint& SiteHint::operator=(SiteHint&&) = default;

const std::string& SiteHint::name(SitePart part) const {
  switch (part) {
    case SitePart::kSearchField:
      return search_field;
    case SitePart::kResultList:
      return result_list;
    case SitePart::kPlayer:
      return player;
  }
  return search_field;
}

std::string& SiteHint::name(SitePart part) {
  switch (part) {
    case SitePart::kSearchField:
      return search_field;
    case SitePart::kResultList:
      return result_list;
    case SitePart::kPlayer:
      return player;
  }
  return search_field;
}

bool SiteHint::empty() const {
  return search_field.empty() && result_list.empty() && player.empty();
}

SitePartUse::SitePartUse() = default;
SitePartUse::~SitePartUse() = default;
SitePartUse::SitePartUse(const SitePartUse&) = default;
SitePartUse& SitePartUse::operator=(const SitePartUse&) = default;
SitePartUse::SitePartUse(SitePartUse&&) = default;
SitePartUse& SitePartUse::operator=(SitePartUse&&) = default;

LocatedPart LocateSitePart(SitePart part,
                           const PageMap& map,
                           const LocatedPart* skill,
                           const SiteHint* hint) {
  // 1. The site's built-in skill.
  if (skill && PartMatches(part, *skill)) {
    LocatedPart found = *skill;
    found.source = PartSource::kSkill;
    return found;
  }
  // 2. What worked here before.
  if (hint && !hint->name(part).empty()) {
    LocatedPart found;
    const std::string& name = hint->name(part);
    switch (part) {
      case SitePart::kSearchField:
        found.field = HintedField(map, name);
        break;
      case SitePart::kResultList:
        found.list = HintedList(map, name);
        break;
      case SitePart::kPlayer:
        found.media = HintedMedia(map, name);
        break;
    }
    if (found.found()) {
      found.source = PartSource::kHint;
      return found;
    }
  }
  // 3. The generic page map.
  return Generic(part, map);
}

std::string HintName(SitePart part, const LocatedPart& found) {
  std::string_view label;
  switch (part) {
    case SitePart::kSearchField:
      if (found.field) {
        label = found.field->label;
      }
      break;
    case SitePart::kResultList:
      if (found.list) {
        label = found.list->label;
      }
      break;
    case SitePart::kPlayer:
      if (found.media) {
        label = found.media->label;
      }
      break;
  }
  return SpeechSafeText(label, kMaxHintNameChars);
}

std::string HintHost(std::string_view host) {
  if (host.empty() || host.size() > kMaxHintHostBytes) {
    return std::string();
  }
  std::string out;
  out.reserve(host.size());
  for (char c : host) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '.';
    if (!ok) {
      return std::string();
    }
    out.push_back(c);
  }
  if (out.front() == '.' || out.back() == '.' ||
      out.find("..") != std::string::npos) {
    return std::string();
  }
  return out;
}

SiteHints::SiteHints() = default;
SiteHints::~SiteHints() = default;
SiteHints::SiteHints(const SiteHints&) = default;
SiteHints& SiteHints::operator=(const SiteHints&) = default;
SiteHints::SiteHints(SiteHints&&) = default;
SiteHints& SiteHints::operator=(SiteHints&&) = default;

const SiteHint* SiteHints::Find(std::string_view host) const {
  const std::string key = HintHost(host);
  if (key.empty()) {
    return nullptr;
  }
  for (const SiteHint& hint : hints_) {
    if (hint.host == key) {
      return &hint;
    }
  }
  return nullptr;
}

SiteHint* SiteHints::FindMutable(std::string_view host) {
  const std::string key = HintHost(host);
  if (key.empty()) {
    return nullptr;
  }
  for (SiteHint& hint : hints_) {
    if (hint.host == key) {
      return &hint;
    }
  }
  return nullptr;
}

void SiteHints::Evict() {
  while (hints_.size() > kMaxHintedSites) {
    hints_.erase(std::min_element(
        hints_.begin(), hints_.end(), [](const SiteHint& a, const SiteHint& b) {
          return a.last_used < b.last_used;
        }));
  }
}

void SiteHints::Worked(std::string_view host,
                       SitePart part,
                       std::string_view name,
                       int64_t now) {
  const std::string key = HintHost(host);
  std::string clean = SpeechSafeText(name, kMaxHintNameChars);
  if (key.empty() || clean.empty()) {
    return;
  }
  SiteHint* hint = FindMutable(key);
  if (!hint) {
    SiteHint added;
    added.host = key;
    hints_.push_back(std::move(added));
    hint = &hints_.back();
  }
  hint->name(part) = std::move(clean);
  hint->last_used = now;
  Evict();
}

void SiteHints::Failed(std::string_view host, SitePart part) {
  SiteHint* hint = FindMutable(host);
  if (!hint) {
    return;
  }
  hint->name(part).clear();
  if (hint->empty()) {
    Forget(host);
  }
}

bool SiteHints::Forget(std::string_view host) {
  const std::string key = HintHost(host);
  const size_t before = hints_.size();
  std::erase_if(hints_,
                [&key](const SiteHint& hint) { return hint.host == key; });
  return hints_.size() != before;
}

void SiteHints::Clear() {
  hints_.clear();
}

void SiteHints::Age(int64_t now) {
  std::erase_if(hints_, [now](const SiteHint& hint) {
    return hint.last_used < now - kHintLifetimeSeconds ||
           hint.last_used > now + kFutureSlackSeconds;
  });
  Evict();
}

bool SiteHints::Restore(SiteHint hint) {
  if (hints_.size() >= kMaxHintedSites || hint.last_used < 0 ||
      hint.host.empty() || HintHost(hint.host) != hint.host ||
      Find(hint.host)) {
    return false;
  }
  for (const SitePart part : {SitePart::kSearchField, SitePart::kResultList,
                              SitePart::kPlayer}) {
    const std::string& name = hint.name(part);
    if (SpeechSafeText(name, kMaxHintNameChars) != name) {
      return false;
    }
  }
  if (hint.empty()) {
    return false;
  }
  hints_.push_back(std::move(hint));
  return true;
}

}  // namespace voice
}  // namespace molt_ai
