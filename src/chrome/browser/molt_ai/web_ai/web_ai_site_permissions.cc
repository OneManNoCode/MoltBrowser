// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace molt_ai::web_ai {

namespace {

constexpr std::string_view kHttpsPrefix = "https://";
constexpr std::string_view kHttpPrefix = "http://";

int64_t ClampTime(int64_t ms) {
  return std::clamp<int64_t>(ms, 0, kMaxStoredTimeMs);
}

// A stored time, or nullopt if it isn't one (NaN, infinite, negative, past
// kMaxStoredTimeMs).
std::optional<int64_t> StoredTime(std::optional<double> ms) {
  if (!ms || !std::isfinite(*ms) || *ms < 0 ||
      *ms > static_cast<double>(kMaxStoredTimeMs)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(*ms);
}

bool IsAnswer(SiteConsent consent) {
  return consent == SiteConsent::kAllowed || consent == SiteConsent::kBlocked;
}

using SiteSet = std::set<std::string_view, std::less<>>;

SiteSet ToSet(const std::vector<std::string>& sites) {
  return SiteSet(sites.begin(), sites.end());
}

}  // namespace

bool IsPlausibleSiteKey(std::string_view site) {
  if (site.size() > kMaxSiteKeyBytes) {
    return false;
  }
  std::string_view rest;
  if (site.starts_with(kHttpsPrefix)) {
    rest = site.substr(kHttpsPrefix.size());
  } else if (site.starts_with(kHttpPrefix)) {
    rest = site.substr(kHttpPrefix.size());
  } else {
    return false;
  }
  if (rest.empty()) {
    return false;
  }
  for (char c : rest) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7f || c == '/' || c == '?' || c == '#' ||
        c == '@' || c == '\\') {
      return false;
    }
  }
  return true;
}

bool IsUnderEmbargo(const SiteRecord& record, int64_t now_ms) {
  if (record.answer != SiteConsent::kAsk ||
      record.dismissals < kDismissalsBeforeEmbargo) {
    return false;
  }
  // Both times are in [0, kMaxStoredTimeMs], so this can't overflow.
  const int64_t elapsed = ClampTime(now_ms) - ClampTime(record.last_dismissal_ms);
  return elapsed < kEmbargoMs && elapsed > -kEmbargoMs;
}

SiteConsent EffectiveConsent(const SiteRecord& record, int64_t now_ms) {
  if (IsAnswer(record.answer)) {
    return record.answer;
  }
  return IsUnderEmbargo(record, now_ms) ? SiteConsent::kBlocked
                                        : SiteConsent::kAsk;
}

// ---------------------------------------------------------------------------
// SitePermissionTable
// ---------------------------------------------------------------------------

SitePermissionTable::SitePermissionTable() = default;
SitePermissionTable::~SitePermissionTable() = default;
SitePermissionTable::SitePermissionTable(SitePermissionTable&&) = default;
SitePermissionTable& SitePermissionTable::operator=(SitePermissionTable&&) =
    default;

SiteConsent SitePermissionTable::Get(std::string_view site,
                                     int64_t now_ms) const {
  const SiteRecord* record = Find(site);
  return record ? EffectiveConsent(*record, now_ms) : SiteConsent::kAsk;
}

const SiteRecord* SitePermissionTable::Find(std::string_view site) const {
  auto it = records_.find(site);
  return it == records_.end() ? nullptr : &it->second;
}

bool SitePermissionTable::SetAnswer(const std::string& site,
                                    SiteConsent answer,
                                    int64_t now_ms) {
  if (!IsPlausibleSiteKey(site)) {
    return false;
  }
  if (!IsAnswer(answer)) {
    records_.erase(site);
    return true;
  }
  if (!records_.contains(site)) {
    MakeRoom();
  }
  SiteRecord& record = records_[site];
  record = SiteRecord();
  record.answer = answer;
  record.answered_ms = ClampTime(now_ms);
  return true;
}

bool SitePermissionTable::RecordDismissal(const std::string& site,
                                          int64_t now_ms) {
  if (!IsPlausibleSiteKey(site)) {
    return false;
  }
  auto it = records_.find(site);
  if (it != records_.end() && IsAnswer(it->second.answer)) {
    return false;
  }
  if (it == records_.end()) {
    MakeRoom();
    it = records_.emplace(site, SiteRecord()).first;
  }
  SiteRecord& record = it->second;
  record.dismissals = std::min(record.dismissals + 1, kMaxStoredDismissals);
  record.last_dismissal_ms = ClampTime(now_ms);
  return IsUnderEmbargo(record, now_ms);
}

bool SitePermissionTable::Remove(std::string_view site) {
  auto it = records_.find(site);
  if (it == records_.end()) {
    return false;
  }
  records_.erase(it);
  return true;
}

bool SitePermissionTable::Clear() {
  const bool had_any = !records_.empty();
  records_.clear();
  return had_any;
}

size_t SitePermissionTable::RemoveAnswersBetween(
    int64_t begin_ms,
    int64_t end_ms,
    const std::vector<std::string>& sites) {
  const SiteSet wanted = ToSet(sites);
  size_t removed = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    const SiteRecord& record = it->second;
    if (IsAnswer(record.answer) && record.answered_ms >= begin_ms &&
        record.answered_ms < end_ms && wanted.contains(it->first)) {
      it = records_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t SitePermissionTable::RemovePromptHistory(
    const std::vector<std::string>& sites) {
  const SiteSet wanted = ToSet(sites);
  size_t removed = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    // Answered records carry no prompt history (it is cleared on answer).
    if (!IsAnswer(it->second.answer) && wanted.contains(it->first)) {
      it = records_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

std::vector<std::string> SitePermissionTable::Sites() const {
  std::vector<std::string> sites;
  sites.reserve(records_.size());
  for (const auto& [site, record] : records_) {
    sites.push_back(site);
  }
  return sites;
}

std::vector<SiteListEntry> SitePermissionTable::List(int64_t now_ms) const {
  std::vector<SiteListEntry> rows;
  for (const auto& [site, record] : records_) {
    SiteListEntry row;
    row.site = site;
    if (record.answer == SiteConsent::kAllowed) {
      row.kind = SiteListEntry::Kind::kAllowed;
      row.since_ms = record.answered_ms;
    } else if (record.answer == SiteConsent::kBlocked) {
      row.kind = SiteListEntry::Kind::kBlocked;
      row.since_ms = record.answered_ms;
    } else if (IsUnderEmbargo(record, now_ms)) {
      row.kind = SiteListEntry::Kind::kPaused;
      row.since_ms = record.last_dismissal_ms;
      row.until_ms = record.last_dismissal_ms + kEmbargoMs;
    } else {
      continue;
    }
    rows.push_back(std::move(row));
  }
  // Keys are already in order; stable_sort keeps that within each kind.
  std::stable_sort(rows.begin(), rows.end(),
                   [](const SiteListEntry& a, const SiteListEntry& b) {
                     return a.kind < b.kind;
                   });
  return rows;
}

bool SitePermissionTable::Insert(const std::string& site,
                                 const SiteRecord& record) {
  if (!IsPlausibleSiteKey(site)) {
    return false;
  }
  SiteRecord clean;
  if (IsAnswer(record.answer)) {
    clean.answer = record.answer;
    clean.answered_ms = ClampTime(record.answered_ms);
  } else if (record.dismissals > 0) {
    clean.dismissals = std::min(record.dismissals, kMaxStoredDismissals);
    clean.last_dismissal_ms = ClampTime(record.last_dismissal_ms);
  } else {
    return false;
  }
  if (!records_.contains(site)) {
    MakeRoom();
  }
  records_[site] = clean;
  return true;
}

void SitePermissionTable::MakeRoom() {
  if (records_.size() < kMaxSites) {
    return;
  }
  // The oldest prompt history first: it is the least the user said.
  auto victim = records_.end();
  for (auto it = records_.begin(); it != records_.end(); ++it) {
    if (IsAnswer(it->second.answer)) {
      continue;
    }
    if (victim == records_.end() ||
        it->second.last_dismissal_ms < victim->second.last_dismissal_ms) {
      victim = it;
    }
  }
  if (victim == records_.end()) {
    // Only answers: the oldest one.
    for (auto it = records_.begin(); it != records_.end(); ++it) {
      if (victim == records_.end() ||
          it->second.answered_ms < victim->second.answered_ms) {
        victim = it;
      }
    }
  }
  if (victim != records_.end()) {
    records_.erase(victim);
  }
}

// ---------------------------------------------------------------------------
// The stored form
// ---------------------------------------------------------------------------

StoredFormat ClassifyStoredVersion(std::optional<int> version) {
  if (!version || *version < 1) {
    return StoredFormat::kDiscard;
  }
  return StoredFormat::kRead;
}

std::optional<SiteRecord> RecordFromStored(const StoredSite& stored,
                                           int64_t now_ms) {
  const int64_t now = ClampTime(now_ms);
  SiteRecord record;
  if (stored.answer &&
      (*stored.answer == kStoredAllow || *stored.answer == kStoredBlock)) {
    record.answer = *stored.answer == kStoredAllow ? SiteConsent::kAllowed
                                                   : SiteConsent::kBlocked;
    // Unknown or future times count as now.
    record.answered_ms = std::min(StoredTime(stored.answered_ms).value_or(now),
                                  now);
    return record;
  }
  if (!stored.dismissals || *stored.dismissals < 1) {
    return std::nullopt;
  }
  const std::optional<int64_t> last = StoredTime(stored.last_dismissal_ms);
  if (!last) {
    return std::nullopt;
  }
  record.dismissals =
      std::min(static_cast<uint32_t>(*stored.dismissals), kMaxStoredDismissals);
  record.last_dismissal_ms = *last;
  return record;
}

StoredSite StoredFromRecord(const SiteRecord& record) {
  StoredSite stored;
  if (IsAnswer(record.answer)) {
    stored.answer = record.answer == SiteConsent::kAllowed ? kStoredAllow
                                                           : kStoredBlock;
    stored.answered_ms = static_cast<double>(ClampTime(record.answered_ms));
    return stored;
  }
  stored.dismissals = static_cast<int>(
      std::min(record.dismissals, kMaxStoredDismissals));
  stored.last_dismissal_ms =
      static_cast<double>(ClampTime(record.last_dismissal_ms));
  return stored;
}

}  // namespace molt_ai::web_ai
