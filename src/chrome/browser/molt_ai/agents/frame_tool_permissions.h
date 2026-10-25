// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A profile's answers to the web agent's question about a frame of another
// site that offers tools (frame_tools.h, docs/design/AGENT_ACTIONS.md
// §2.7.14), over frame_tools::PermissionTable:
//  * "This session" answers live in memory, until the browser quits;
//  * "Always" answers of a regular, on-disk profile are kept in its prefs
//    (kAllowedPref, registered without SYNCABLE_PREF, so they never sync),
//    until the user removes them (the agent studio), clears "Site
//    settings" or resets the profile (the hooks below);
//  * Incognito and the Guest session have their own service, in memory
//    only, starting empty: "Always" isn't offered there;
//  * MoltNet windows have none: another site's frame's tools are never
//    listed there.
//
// Read by the web agent, the AI chat and Agent mode's studio; written only
// from a user's click (the web agent's question, the studio's Remove).

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOL_PERMISSIONS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOL_PERMISSIONS_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/time/time.h"
#include "chrome/browser/molt_ai/agents/frame_tools.h"
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

namespace molt_ai {

// The regular profile's "Always" pairs (frame_tools::PermissionTable::
// ToStored()).
inline constexpr char kFrameToolsAllowedPref[] =
    "molt_ai.agent.frame_tools_allowed";

class FrameToolPermissions : public KeyedService {
 public:
  // The service of |context|'s profile, created on first use; null where
  // there is none (MoltNet, the system profile).
  static FrameToolPermissions* Get(content::BrowserContext* context);

  // |prefs|: where a regular profile keeps its "Always" answers. Null keeps
  // everything in memory only.
  explicit FrameToolPermissions(PrefService* prefs);
  FrameToolPermissions(const FrameToolPermissions&) = delete;
  FrameToolPermissions& operator=(const FrameToolPermissions&) = delete;
  ~FrameToolPermissions() override;

  // kNone unless the user allowed |frame_origin| on |main_origin|. Opaque
  // and non-http(s) origins never are.
  frame_tools::Grant Get(const url::Origin& main_origin,
                         const url::Origin& frame_origin) const;
  // The serialized frame origins allowed on |main_origin|.
  std::vector<std::string> AllowedFrames(const url::Origin& main_origin) const;

  // The user's answer (kSession or kAlways; kAlways only where
  // persistent()). False if nothing was allowed.
  bool Allow(const url::Origin& main_origin,
             const url::Origin& frame_origin,
             frame_tools::Grant grant);

  // The studio.
  std::vector<frame_tools::Pair> List() const;
  // |main_origin| and |frame_origin| as List() gave them.
  bool Remove(std::string_view main_origin, std::string_view frame_origin);
  void RemoveAll();
  // False for Incognito and Guest.
  bool persistent() const { return persistent_; }

  // Clear browsing data ("Site settings"): pairs allowed in [begin, end)
  // whose main or frame origin |url_filter| picks (null: every pair).
  void RemoveBetween(base::Time begin,
                     base::Time end,
                     const base::RepeatingCallback<bool(const GURL&)>&
                         url_filter);

  // KeyedService:
  void Shutdown() override;

 private:
  void Load();
  // Writes the "Always" pairs to the pref (regular profiles).
  void Changed();

  raw_ptr<PrefService> prefs_;
  const bool persistent_;
  frame_tools::PermissionTable table_;
};

class FrameToolPermissionsFactory : public ProfileKeyedServiceFactory {
 public:
  static FrameToolPermissionsFactory* GetInstance();
  static FrameToolPermissions* GetForProfile(Profile* profile);

  FrameToolPermissionsFactory(const FrameToolPermissionsFactory&) = delete;
  FrameToolPermissionsFactory& operator=(const FrameToolPermissionsFactory&) =
      delete;

 private:
  friend class base::NoDestructor<FrameToolPermissionsFactory>;

  FrameToolPermissionsFactory();
  ~FrameToolPermissionsFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
};

// ---- Hooks in upstream code (one call each) ----

// ChromeBrowsingDataRemoverDelegate, DATA_TYPE_CONTENT_SETTINGS ("Site
// settings"). |url_filter| may be null (all).
void ClearFrameToolPermissions(
    Profile* profile,
    base::Time begin,
    base::Time end,
    const base::RepeatingCallback<bool(const GURL&)>& url_filter);

// ProfileResetter::ResetContentSettings: everything.
void ResetFrameToolPermissions(Profile* profile);

}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOL_PERMISSIONS_H_
