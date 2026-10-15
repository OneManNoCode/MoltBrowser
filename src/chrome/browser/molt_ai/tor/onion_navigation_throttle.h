// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OnionNavigationThrottle — keeps .onion names out of regular windows.
//
// RFC 7686 reserves .onion for Tor onion services. A regular (non-MoltNet)
// profile has no Tor, so loading one could only leak the onion name to the
// DNS resolver: Chromium's host resolver doesn't special-case .onion at this
// pin. For every navigation in every profile except MoltNet's, this
// throttle, registered before all others (chrome_content_browser_client.cc),
// stops a navigation to a .onion host before its request starts — on the
// initial URL and on every redirect:
//   * a primary main-frame http(s) navigation is cancelled and replaced by
//     OnionBlockingPage ("This is a Tor onion site ... [Open in a MoltNet
//     window]");
//   * anything else (subframes, prerendering, fenced frames, other schemes)
//     is blocked with ERR_BLOCKED_BY_CLIENT.
// MoltNet windows don't get the throttle: there the onion name goes to Tor
// inside the SOCKS5 CONNECT and is never resolved locally.
//
// Belt and braces: net/dns/host_resolver_manager.cc also refuses .onion
// names, which covers what isn't a navigation (subresources, preconnects,
// prefetches).

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_NAVIGATION_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace molt_ai {
namespace tor {

class OnionNavigationThrottle : public content::NavigationThrottle {
 public:
  // Adds the throttle unless the navigation belongs to a MoltNet profile.
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);

  explicit OnionNavigationThrottle(
      content::NavigationThrottleRegistry& registry);
  OnionNavigationThrottle(const OnionNavigationThrottle&) = delete;
  OnionNavigationThrottle& operator=(const OnionNavigationThrottle&) = delete;
  ~OnionNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult Check(bool is_redirect);
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_NAVIGATION_THROTTLE_H_
