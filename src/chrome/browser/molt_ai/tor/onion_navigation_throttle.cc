// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_navigation_throttle.h"

#include <memory>
#include <string>
#include <utility>

#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/onion_blocking_page.h"
#include "chrome/browser/molt_ai/tor/onion_util.h"
#include "chrome/browser/profiles/profile.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"
#include "url/gurl.h"

namespace molt_ai {
namespace tor {

// static
void OnionNavigationThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  content::WebContents* web_contents =
      registry.GetNavigationHandle().GetWebContents();
  Profile* profile =
      web_contents
          ? Profile::FromBrowserContext(web_contents->GetBrowserContext())
          : nullptr;
  // MoltNet windows reach onion services through Tor. Everything else
  // (including an unknown profile) gets the throttle: blocking is the safe
  // default.
  if (profile && IsMoltNetProfile(profile)) {
    return;
  }
  registry.AddThrottle(std::make_unique<OnionNavigationThrottle>(registry));
}

OnionNavigationThrottle::OnionNavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

OnionNavigationThrottle::~OnionNavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
OnionNavigationThrottle::WillStartRequest() {
  return Check();
}

content::NavigationThrottle::ThrottleCheckResult
OnionNavigationThrottle::WillRedirectRequest() {
  // A page (or an HTTPS upgrade) may redirect to an onion URL.
  return Check();
}

const char* OnionNavigationThrottle::GetNameForLogging() {
  return "MoltOnionNavigationThrottle";
}

content::NavigationThrottle::ThrottleCheckResult
OnionNavigationThrottle::Check() {
  content::NavigationHandle* handle = navigation_handle();
  const GURL& url = handle->GetURL();
  if (!IsOnionHost(url.host())) {
    return PROCEED;
  }
  if (!handle->IsInPrimaryMainFrame() || !url.SchemeIsHTTPOrHTTPS()) {
    return ThrottleCheckResult(BLOCK_REQUEST, net::ERR_BLOCKED_BY_CLIENT);
  }
  // Same pattern as the HTTPS-Only Mode interstitial: commit the blocking
  // page's HTML as this navigation's error page and let
  // SecurityInterstitialTabHelper own the page and route its buttons.
  auto page =
      std::make_unique<OnionBlockingPage>(handle->GetWebContents(), url);
  std::string html = page->GetHTMLContents();
  security_interstitials::SecurityInterstitialTabHelper::AssociateBlockingPage(
      handle, std::move(page));
  return ThrottleCheckResult(CANCEL, net::ERR_BLOCKED_BY_CLIENT,
                             std::move(html));
}

}  // namespace tor
}  // namespace molt_ai
