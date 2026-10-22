// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_service.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/callback.h"
#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace molt_ai::web_ai {

namespace {

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

int64_t ToMs(base::Time time) {
  // Null is 0 and Max() is INT64_MAX, so "all time" spans every record.
  return time.InMillisecondsSinceUnixEpoch();
}

// The key for |origin|; empty for origins that can't have one (opaque,
// not http(s)).
std::string SiteKey(const url::Origin& origin) {
  if (origin.opaque() || (origin.scheme() != url::kHttpsScheme &&
                          origin.scheme() != url::kHttpScheme)) {
    return std::string();
  }
  std::string key = origin.Serialize();
  return IsPlausibleSiteKey(key) ? key : std::string();
}

// |key| is exactly how an http(s) origin serializes (stored keys are
// checked again on load: the pref file is not trusted).
bool IsCanonicalSiteKey(const std::string& key) {
  return IsPlausibleSiteKey(key) &&
         SiteKey(url::Origin::Create(GURL(key))) == key;
}

StoredSite ReadStoredSite(const base::DictValue& dict) {
  StoredSite stored;
  if (const std::string* answer = dict.FindString(kStoredAnswerKey)) {
    stored.answer = *answer;
  }
  stored.answered_ms = dict.FindDouble(kStoredAnsweredKey);
  stored.dismissals = dict.FindInt(kStoredDismissalsKey);
  stored.last_dismissal_ms = dict.FindDouble(kStoredLastDismissalKey);
  return stored;
}

base::DictValue WriteStoredSite(const StoredSite& stored) {
  base::DictValue dict;
  if (stored.answer) {
    dict.Set(kStoredAnswerKey, *stored.answer);
  }
  if (stored.answered_ms) {
    dict.Set(kStoredAnsweredKey, *stored.answered_ms);
  }
  if (stored.dismissals) {
    dict.Set(kStoredDismissalsKey, *stored.dismissals);
  }
  if (stored.last_dismissal_ms) {
    dict.Set(kStoredLastDismissalKey, *stored.last_dismissal_ms);
  }
  return dict;
}

}  // namespace

// ---------------------------------------------------------------------------
// WebAISitePermissions
// ---------------------------------------------------------------------------

WebAISitePermissions::WebAISitePermissions(PrefService* prefs)
    : prefs_(prefs), persistent_(prefs != nullptr) {
  Load();
}

WebAISitePermissions::~WebAISitePermissions() = default;

void WebAISitePermissions::Shutdown() {
  // Prefs may go before this service is destroyed.
  prefs_ = nullptr;
}

void WebAISitePermissions::Load() {
  if (!prefs_) {
    return;
  }
  const base::DictValue& stored = prefs_->GetDict(kSitesPref);
  if (stored.empty() ||
      ClassifyStoredVersion(stored.FindInt(kStoredVersionKey)) ==
          StoredFormat::kDiscard) {
    return;
  }
  const base::DictValue* sites = stored.FindDict(kStoredSitesKey);
  if (!sites) {
    return;
  }
  const int64_t now = NowMs();
  for (const auto [site, value] : *sites) {
    if (!value.is_dict() || !IsCanonicalSiteKey(site)) {
      continue;
    }
    if (std::optional<SiteRecord> record =
            RecordFromStored(ReadStoredSite(value.GetDict()), now)) {
      table_.Insert(site, *record);
    }
  }
}

void WebAISitePermissions::Changed() {
  if (prefs_) {
    base::DictValue sites;
    for (const auto& [site, record] : table_.records()) {
      sites.Set(site, WriteStoredSite(StoredFromRecord(record)));
    }
    base::DictValue stored;
    stored.Set(kStoredVersionKey, kStoredFormatVersion);
    stored.Set(kStoredSitesKey, std::move(sites));
    prefs_->SetDict(kSitesPref, std::move(stored));
  }
  observers_.Notify();
}

SiteConsent WebAISitePermissions::GetConsent(const url::Origin& origin) const {
  const std::string key = SiteKey(origin);
  return key.empty() ? SiteConsent::kAsk : table_.Get(key, NowMs());
}

void WebAISitePermissions::SetAnswer(const url::Origin& origin,
                                     SiteConsent answer) {
  const std::string key = SiteKey(origin);
  if (!key.empty() && table_.SetAnswer(key, answer, NowMs())) {
    Changed();
  }
}

void WebAISitePermissions::RecordDismissal(const url::Origin& origin) {
  const std::string key = SiteKey(origin);
  if (key.empty()) {
    return;
  }
  const SiteRecord* before = table_.Find(key);
  const uint32_t dismissals_before = before ? before->dismissals : 0;
  table_.RecordDismissal(key, NowMs());
  const SiteRecord* after = table_.Find(key);
  if (after && after->dismissals != dismissals_before) {
    Changed();
  }
}

std::vector<SiteListEntry> WebAISitePermissions::List() const {
  return table_.List(NowMs());
}

bool WebAISitePermissions::Remove(std::string_view site) {
  if (!table_.Remove(site)) {
    return false;
  }
  Changed();
  return true;
}

void WebAISitePermissions::RemoveAll() {
  if (table_.Clear()) {
    Changed();
  }
}

std::vector<std::string> WebAISitePermissions::SitesMatching(
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) const {
  std::vector<std::string> sites = table_.Sites();
  if (url_filter.is_null()) {
    return sites;
  }
  std::vector<std::string> matching;
  for (std::string& site : sites) {
    if (url_filter.Run(GURL(site))) {
      matching.push_back(std::move(site));
    }
  }
  return matching;
}

void WebAISitePermissions::RemoveAnswersBetween(
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  if (table_.RemoveAnswersBetween(ToMs(begin), ToMs(end),
                                  SitesMatching(url_filter)) > 0) {
    Changed();
  }
}

void WebAISitePermissions::RemovePromptHistory(
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  if (table_.RemovePromptHistory(SitesMatching(url_filter)) > 0) {
    Changed();
  }
}

base::CallbackListSubscription WebAISitePermissions::AddObserver(
    base::RepeatingClosure callback) {
  return observers_.Add(std::move(callback));
}

}  // namespace molt_ai::web_ai
