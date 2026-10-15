// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Cancels navigations a web page starts toward chrome:// / molt:// pages
// (S19); see webui_navigation_policy.h for why the molt:// alias needs it.
// Added from ChromeContentBrowserClient::CreateThrottlesForNavigation for
// renderer-initiated, cross-document navigations only.

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_THROTTLE_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

namespace molt_ai::security {

class WebUINavigationThrottle : public content::NavigationThrottle {
 public:
  static void MaybeAdd(content::NavigationThrottleRegistry& registry);

  explicit WebUINavigationThrottle(
      content::NavigationThrottleRegistry& registry);
  WebUINavigationThrottle(const WebUINavigationThrottle&) = delete;
  WebUINavigationThrottle& operator=(const WebUINavigationThrottle&) = delete;
  ~WebUINavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  ThrottleCheckResult Check();
};

}  // namespace molt_ai::security

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_WEBUI_NAVIGATION_THROTTLE_H_
