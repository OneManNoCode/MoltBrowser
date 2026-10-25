// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/frame_tool_permissions.h"

#include <utility>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "url/url_constants.h"

namespace molt_ai {

namespace {

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

// The key of |origin|: its serialization if it is an http(s) origin
// frame_tools accepts; else empty.
std::string KeyOf(const url::Origin& origin) {
  if (origin.opaque() || (origin.scheme() != url::kHttpsScheme &&
                          origin.scheme() != url::kHttpScheme)) {
    return std::string();
  }
  std::string key = origin.Serialize();
  return frame_tools::IsPairOrigin(key) ? key : std::string();
}

// |key| is exactly how an http(s) origin serializes (the pref file is not
// trusted).
bool IsCanonicalKey(const std::string& key) {
  return frame_tools::IsPairOrigin(key) &&
         KeyOf(url::Origin::Create(GURL(key))) == key;
}

}  // namespace

// static
FrameToolPermissions* FrameToolPermissions::Get(
    content::BrowserContext* context) {
  return context ? FrameToolPermissionsFactory::GetForProfile(
                       Profile::FromBrowserContext(context))
                 : nullptr;
}

FrameToolPermissions::FrameToolPermissions(PrefService* prefs)
    : prefs_(prefs), persistent_(prefs != nullptr) {
  Load();
}

FrameToolPermissions::~FrameToolPermissions() = default;

void FrameToolPermissions::Shutdown() {
  // Prefs may go before this service is destroyed.
  prefs_ = nullptr;
}

void FrameToolPermissions::Load() {
  if (!prefs_) {
    return;
  }
  const frame_tools::PermissionTable stored =
      frame_tools::PermissionTable::FromStored(
          base::Value(prefs_->GetList(kFrameToolsAllowedPref).Clone()));
  for (const frame_tools::Pair& pair : stored.List()) {
    if (IsCanonicalKey(pair.main_origin) && IsCanonicalKey(pair.frame_origin)) {
      table_.Allow(pair.main_origin, pair.frame_origin,
                   frame_tools::Grant::kAlways, pair.since_ms);
    }
  }
}

void FrameToolPermissions::Changed() {
  if (prefs_) {
    prefs_->SetList(kFrameToolsAllowedPref, table_.ToStored());
  }
}

frame_tools::Grant FrameToolPermissions::Get(
    const url::Origin& main_origin,
    const url::Origin& frame_origin) const {
  const std::string main_key = KeyOf(main_origin);
  const std::string frame_key = KeyOf(frame_origin);
  if (main_key.empty() || frame_key.empty()) {
    return frame_tools::Grant::kNone;
  }
  return table_.Get(main_key, frame_key);
}

std::vector<std::string> FrameToolPermissions::AllowedFrames(
    const url::Origin& main_origin) const {
  const std::string main_key = KeyOf(main_origin);
  if (main_key.empty()) {
    return {};
  }
  return table_.AllowedFrames(main_key);
}

bool FrameToolPermissions::Allow(const url::Origin& main_origin,
                                 const url::Origin& frame_origin,
                                 frame_tools::Grant grant) {
  if (grant == frame_tools::Grant::kAlways && !persistent_) {
    return false;
  }
  const std::string main_key = KeyOf(main_origin);
  const std::string frame_key = KeyOf(frame_origin);
  if (main_key.empty() || frame_key.empty() ||
      !table_.Allow(main_key, frame_key, grant, NowMs())) {
    return false;
  }
  if (grant == frame_tools::Grant::kAlways) {
    Changed();
  }
  return true;
}

std::vector<frame_tools::Pair> FrameToolPermissions::List() const {
  return table_.List();
}

bool FrameToolPermissions::Remove(std::string_view main_origin,
                                  std::string_view frame_origin) {
  if (!table_.Remove(main_origin, frame_origin)) {
    return false;
  }
  Changed();
  return true;
}

void FrameToolPermissions::RemoveAll() {
  table_.RemoveAll();
  Changed();
}

void FrameToolPermissions::RemoveBetween(
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  // Null is 0 and Max() is INT64_MAX, so "all time" spans every pair.
  const int64_t begin_ms = begin.InMillisecondsSinceUnixEpoch();
  const int64_t end_ms = end.InMillisecondsSinceUnixEpoch();
  bool removed = false;
  for (const frame_tools::Pair& pair : table_.List()) {
    if (pair.since_ms < begin_ms || pair.since_ms >= end_ms) {
      continue;
    }
    if (!url_filter.is_null() && !url_filter.Run(GURL(pair.main_origin)) &&
        !url_filter.Run(GURL(pair.frame_origin))) {
      continue;
    }
    removed |= table_.Remove(pair.main_origin, pair.frame_origin);
  }
  if (removed) {
    Changed();
  }
}

// ---------------------------------------------------------------------------
// FrameToolPermissionsFactory
// ---------------------------------------------------------------------------

// static
FrameToolPermissionsFactory* FrameToolPermissionsFactory::GetInstance() {
  static base::NoDestructor<FrameToolPermissionsFactory> instance;
  return instance.get();
}

// static
FrameToolPermissions* FrameToolPermissionsFactory::GetForProfile(
    Profile* profile) {
  return static_cast<FrameToolPermissions*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

FrameToolPermissionsFactory::FrameToolPermissionsFactory()
    : ProfileKeyedServiceFactory(
          "MoltFrameToolPermissions",
          // Incognito gets its own instance (in memory, starting empty);
          // the Guest session is an off-the-record profile.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .WithGuest(ProfileSelection::kOffTheRecordOnly)
              .WithSystem(ProfileSelection::kNone)
              .Build()) {}

FrameToolPermissionsFactory::~FrameToolPermissionsFactory() = default;

std::unique_ptr<KeyedService>
FrameToolPermissionsFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  // MoltNet windows (an off-the-record profile) never list another site's
  // frame's tools.
  if (tor::IsMoltNetProfile(profile)) {
    return nullptr;
  }
  // Only a regular, on-disk profile keeps "Always" across restarts.
  PrefService* prefs = profile->IsOffTheRecord() || profile->IsGuestSession()
                           ? nullptr
                           : profile->GetPrefs();
  return std::make_unique<FrameToolPermissions>(prefs);
}

void FrameToolPermissionsFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // No SYNCABLE_PREF: which frames the agent may use stays on this device.
  registry->RegisterListPref(kFrameToolsAllowedPref);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void ClearFrameToolPermissions(
    Profile* profile,
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  if (FrameToolPermissions* service = FrameToolPermissions::Get(profile)) {
    service->RemoveBetween(begin, end, url_filter);
  }
}

void ResetFrameToolPermissions(Profile* profile) {
  if (FrameToolPermissions* service = FrameToolPermissions::Get(profile)) {
    service->RemoveAll();
  }
}

}  // namespace molt_ai
