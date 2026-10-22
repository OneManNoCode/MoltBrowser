// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A profile's answers for the built-in AI APIs (Allow / Block per top-level
// origin) and the consent prompt's pauses, over SitePermissionTable
// (web_ai_site_permissions.h):
//  * a regular profile keeps them in its prefs (kSitesPref, a dictionary
//    MoltBrowser owns; registered without SYNCABLE_PREF, so it never syncs);
//  * Incognito and Guest keep them in memory, for the profile's lifetime,
//    starting empty (nothing is inherited from the regular profile, so an
//    Incognito window can't be linked to it through these answers);
//  * MoltNet windows have none: the APIs are off there.
//
// Read by the binder (MoltAIManager's consent), written by the consent
// infobar (web_ai_consent.cc) and the settings page (molt://ai-settings,
// "Sites using on-device AI"), and cleared by Clear browsing data ("Site
// settings" clears answers; history and site usage data clear the prompt's
// history) and by a profile reset, through the hooks at the bottom.
// Design: docs/design/WEB_AI_APIS.md §4.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_SERVICE_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_SERVICE_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"

class GURL;
class PrefService;
class Profile;

namespace content {
class BrowserContext;
}  // namespace content

namespace url {
class Origin;
}  // namespace url

namespace user_prefs {
class PrefRegistrySyncable;
}  // namespace user_prefs

namespace molt_ai::web_ai {

// The regular profile's answers (see web_ai_site_permissions.h for the
// format).
inline constexpr char kSitesPref[] = "molt_ai.web_ai.sites";

class WebAISitePermissions : public KeyedService {
 public:
  // The service of |context|'s profile, created on first use; null where
  // there is none (the system profile, MoltNet).
  static WebAISitePermissions* Get(content::BrowserContext* context);

  // |prefs|: where a regular profile keeps its answers. Null keeps them in
  // memory only.
  explicit WebAISitePermissions(PrefService* prefs);
  WebAISitePermissions(const WebAISitePermissions&) = delete;
  WebAISitePermissions& operator=(const WebAISitePermissions&) = delete;
  ~WebAISitePermissions() override;

  // What applies to pages whose top-level origin is |origin|: the answer,
  // kBlocked while the prompt is paused, else kAsk. kAsk for opaque and
  // non-http(s) origins.
  SiteConsent GetConsent(const url::Origin& origin) const;
  // The consent infobar: Allow / Block, or closed with its close button.
  void SetAnswer(const url::Origin& origin, SiteConsent answer);
  void RecordDismissal(const url::Origin& origin);

  // The settings page.
  std::vector<SiteListEntry> List() const;
  // |site| must be a key List() returned; anything else changes nothing.
  bool Remove(std::string_view site);
  void RemoveAll();
  // False for Incognito and Guest (in memory only).
  bool persistent() const { return persistent_; }

  // Clear browsing data. |url_filter| picks sites by their origin's URL;
  // null means every site.
  void RemoveAnswersBetween(
      base::Time begin,
      base::Time end,
      const base::RepeatingCallback<bool(const GURL&)>& url_filter);
  void RemovePromptHistory(
      const base::RepeatingCallback<bool(const GURL&)>& url_filter);

  // Runs |callback| after every change (any site). Sessions of a site whose
  // answer is no longer Allow are closed through this.
  base::CallbackListSubscription AddObserver(base::RepeatingClosure callback);

  // KeyedService:
  void Shutdown() override;

 private:
  void Load();
  // Writes the table to the pref (regular profiles) and tells observers.
  void Changed();
  std::vector<std::string> SitesMatching(
      const base::RepeatingCallback<bool(const GURL&)>& url_filter) const;

  raw_ptr<PrefService> prefs_;
  const bool persistent_;
  SitePermissionTable table_;
  base::RepeatingClosureList observers_;
};

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

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_SITE_PERMISSIONS_SERVICE_H_
