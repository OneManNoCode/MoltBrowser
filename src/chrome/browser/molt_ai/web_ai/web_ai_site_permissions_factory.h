// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Creates WebAISitePermissions (web_ai_site_permissions_service.h) per
// profile and registers its pref, and the one-line hooks upstream code
// calls: Clear browsing data and a profile reset.
//
// Profiles: a regular profile's service keeps its answers in kSitesPref;
// its Incognito profile and the Guest session get their own, in memory
// only; MoltNet's off-the-record profile and the system profile get none.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_FACTORY_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_FACTORY_H_

#include <memory>

#include "base/functional/callback_forward.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class GURL;
class KeyedService;
class Profile;

namespace content {
class BrowserContext;
}  // namespace content

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace molt_ai::web_ai {

class WebAISitePermissions;

class WebAISitePermissionsFactory : public ProfileKeyedServiceFactory {
 public:
  static WebAISitePermissionsFactory* GetInstance();
  static WebAISitePermissions* GetForProfile(Profile* profile);

  WebAISitePermissionsFactory(const WebAISitePermissionsFactory&) = delete;
  WebAISitePermissionsFactory& operator=(const WebAISitePermissionsFactory&) =
      delete;

 private:
  friend class base::NoDestructor<WebAISitePermissionsFactory>;

  WebAISitePermissionsFactory();
  ~WebAISitePermissionsFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
};

// ---- Hooks in upstream code (one call each) ----

// ChromeBrowsingDataRemoverDelegate, DATA_TYPE_CONTENT_SETTINGS ("Site
// settings"): answers given in [begin, end). |url_filter| may be null (all).
void ClearWebAISiteAnswers(
    Profile* profile,
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter);

// ChromeBrowsingDataRemoverDelegate, DATA_TYPE_HISTORY and
// DATA_TYPE_SITE_USAGE_DATA: the prompt's history and pauses, whenever
// recorded (as PermissionDecisionAutoBlocker's embargoes).
void ClearWebAIPromptHistory(
    Profile* profile,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter);

// ProfileResetter::ResetContentSettings: everything.
void ResetWebAISitePermissions(Profile* profile);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_FACTORY_H_
