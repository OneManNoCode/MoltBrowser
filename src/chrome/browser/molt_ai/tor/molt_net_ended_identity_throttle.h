// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetEndedIdentityThrottle — the browser refuses every navigation of a
// MoltNet identity that has ended (MoltNetRouting::EndIdentity(): New
// Identity, a setting the live check refuses), whatever its stage: before
// its request starts, on a redirect, and before its response is used. The
// refusal sent to the identity's network contexts takes a moment to be
// applied there (molt_net_identity_end_policy.h); a navigation the browser
// would start meanwhile (a reload, one a throttle had deferred) is refused
// here first, and one already under way doesn't commit. Added, after the
// onion throttle, to MoltNet navigations only
// (ChromeContentBrowserClient::CreateThrottlesForNavigation()).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ENDED_IDENTITY_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ENDED_IDENTITY_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace molt_ai {
namespace tor {

class MoltNetEndedIdentityThrottle : public content::NavigationThrottle {
 public:
  // Adds the throttle if the navigation belongs to a MoltNet profile.
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);

  explicit MoltNetEndedIdentityThrottle(
      content::NavigationThrottleRegistry& registry);
  MoltNetEndedIdentityThrottle(const MoltNetEndedIdentityThrottle&) = delete;
  MoltNetEndedIdentityThrottle& operator=(const MoltNetEndedIdentityThrottle&) =
      delete;
  ~MoltNetEndedIdentityThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  ThrottleCheckResult WillProcessResponse() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult Check();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_ENDED_IDENTITY_THROTTLE_H_
