// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_factory.h"

#include <memory>

#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "url/gurl.h"

namespace molt_ai::web_ai {

// static
WebAISitePermissions* WebAISitePermissions::Get(
    content::BrowserContext* context) {
  return context ? WebAISitePermissionsFactory::GetForProfile(
                       Profile::FromBrowserContext(context))
                 : nullptr;
}

// ---------------------------------------------------------------------------
// WebAISitePermissionsFactory
// ---------------------------------------------------------------------------

// static
WebAISitePermissionsFactory* WebAISitePermissionsFactory::GetInstance() {
  static base::NoDestructor<WebAISitePermissionsFactory> instance;
  return instance.get();
}

// static
WebAISitePermissions* WebAISitePermissionsFactory::GetForProfile(
    Profile* profile) {
  return static_cast<WebAISitePermissions*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

WebAISitePermissionsFactory::WebAISitePermissionsFactory()
    : ProfileKeyedServiceFactory(
          "MoltWebAISitePermissions",
          // Incognito gets its own instance (in memory, starting empty);
          // the Guest session is an off-the-record profile.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .WithGuest(ProfileSelection::kOffTheRecordOnly)
              .WithSystem(ProfileSelection::kNone)
              .Build()) {}

WebAISitePermissionsFactory::~WebAISitePermissionsFactory() = default;

std::unique_ptr<KeyedService>
WebAISitePermissionsFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  // MoltNet windows (an off-the-record profile) never offer the APIs.
  if (tor::IsMoltNetProfile(profile)) {
    return nullptr;
  }
  // Only a regular, on-disk profile keeps answers across restarts.
  PrefService* prefs = profile->IsOffTheRecord() || profile->IsGuestSession()
                           ? nullptr
                           : profile->GetPrefs();
  return std::make_unique<WebAISitePermissions>(prefs);
}

void WebAISitePermissionsFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // No SYNCABLE_PREF: which sites may use the on-device model stays on this
  // device.
  registry->RegisterDictionaryPref(kSitesPref);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void ClearWebAISiteAnswers(
    Profile* profile,
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  if (WebAISitePermissions* service = WebAISitePermissions::Get(profile)) {
    service->RemoveAnswersBetween(begin, end, url_filter);
  }
}

void ClearWebAIPromptHistory(
    Profile* profile,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter) {
  if (WebAISitePermissions* service = WebAISitePermissions::Get(profile)) {
    service->RemovePromptHistory(url_filter);
  }
}

void ResetWebAISitePermissions(Profile* profile) {
  if (WebAISitePermissions* service = WebAISitePermissions::Get(profile)) {
    service->RemoveAll();
  }
}

}  // namespace molt_ai::web_ai
