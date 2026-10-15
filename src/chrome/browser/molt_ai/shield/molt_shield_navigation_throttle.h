// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldNavigationThrottle — MoltShield's navigation-level hook.
//
//  * Top-level documents (the page the user navigated to) are NEVER blocked:
//    tracker lists name domains to block as third-party resources, and
//    applied to typed/clicked navigations they would make sites like
//    hotjar.com unreachable. (Kept from the previous throttle.)
//  * Frames (iframes, fenced frames) are checked as "sub_frame" against the
//    engine, asynchronously, and blocked ad frames are collapsed
//    (BLOCK_REQUEST_AND_COLLAPSE) instead of showing an error page.
//  * For every document it fetches the cosmetic resources (hide CSS +
//    scriptlets) in parallel with the network request, and holds the commit
//    only if they aren't ready by response time (bounded wait). They're
//    stashed on the NavigationHandle for MoltShieldTabHelper to send to the
//    renderer at ReadyToCommitNavigation, ahead of the commit IPC.
//
// Tracking parameters are stripped from navigations by
// MoltShieldNavigationInterceptor (an internal redirect, which a
// NavigationThrottle can't produce); this throttle sees the stripped URL in
// WillRedirectRequest like any redirect.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_THROTTLE_H_

#include <optional>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "chrome/browser/molt_ai/shield/molt_shield_types.h"
#include "content/public/browser/navigation_handle_user_data.h"
#include "content/public/browser/navigation_throttle.h"
#include "url/gurl.h"

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace molt_ai::shield {

// Cosmetic resources for the document a navigation is about to commit.
class ShieldNavigationData
    : public content::NavigationHandleUserData<ShieldNavigationData> {
 public:
  ShieldNavigationData(const ShieldNavigationData&) = delete;
  ShieldNavigationData& operator=(const ShieldNavigationData&) = delete;
  ~ShieldNavigationData() override;

  GURL url;           // Document URL the resources were computed for.
  CosmeticData data;

 private:
  explicit ShieldNavigationData(content::NavigationHandle& handle);
  friend NavigationHandleUserData;
  NAVIGATION_HANDLE_USER_DATA_KEY_DECL();
};

class MoltShieldNavigationThrottle : public content::NavigationThrottle {
 public:
  // UI thread. Adds the throttle unless MoltShield is off globally or for
  // the navigation's top-level site.
  static void MaybeAdd(content::NavigationThrottleRegistry& registry);

  MoltShieldNavigationThrottle(content::NavigationThrottleRegistry& registry,
                               bool check_frame,
                               bool want_cosmetics);
  MoltShieldNavigationThrottle(const MoltShieldNavigationThrottle&) = delete;
  MoltShieldNavigationThrottle& operator=(const MoltShieldNavigationThrottle&) =
      delete;
  ~MoltShieldNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  ThrottleCheckResult WillProcessResponse() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult OnRequest();
  void OnFrameChecked(GURL url, CheckResult result);
  void RequestCosmetics(const GURL& url);
  void OnCosmetics(GURL url, CosmeticData data);
  void StashCosmetics();
  void OnCosmeticsTimeout();

  const bool check_frame_;
  const bool want_cosmetics_;
  bool deferred_for_frame_ = false;
  bool deferred_for_cosmetics_ = false;
  GURL cosmetics_url_;
  std::optional<CosmeticData> cosmetics_;
  base::OneShotTimer cosmetics_timeout_;
  base::WeakPtrFactory<MoltShieldNavigationThrottle> weak_factory_{this};
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_NAVIGATION_THROTTLE_H_
