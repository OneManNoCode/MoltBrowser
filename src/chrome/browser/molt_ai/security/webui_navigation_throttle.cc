// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/security/webui_navigation_throttle.h"

#include <memory>

#include "base/logging.h"
#include "chrome/browser/molt_ai/security/webui_navigation_policy.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/reload_type.h"

namespace molt_ai::security {

// static
void WebUINavigationThrottle::MaybeAdd(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!handle.IsRendererInitiated() || handle.IsSameDocument()) {
    return;
  }
  registry.AddThrottle(std::make_unique<WebUINavigationThrottle>(registry));
}

WebUINavigationThrottle::WebUINavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

WebUINavigationThrottle::~WebUINavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
WebUINavigationThrottle::WillStartRequest() {
  return Check();
}

content::NavigationThrottle::ThrottleCheckResult
WebUINavigationThrottle::WillRedirectRequest() {
  return Check();
}

const char* WebUINavigationThrottle::GetNameForLogging() {
  return "MoltWebUINavigationThrottle";
}

content::NavigationThrottle::ThrottleCheckResult
WebUINavigationThrottle::Check() {
  content::NavigationHandle* handle = navigation_handle();
  const bool history_or_reload =
      handle->IsHistory() ||
      handle->GetReloadType() != content::ReloadType::NONE;
  if (ShouldBlockWebInitiatedWebUINavigation(
          handle->GetURL(), handle->IsRendererInitiated(), history_or_reload,
          handle->GetInitiatorOrigin())) {
    DVLOG(1) << "[MoltBrowser] blocked a web-initiated navigation to "
             << handle->GetURL().scheme() << "://" << handle->GetURL().host();
    // Like FilterURL's about:blank#blocked, but nothing to commit: the
    // frame stays where it was (a new noopener window stays blank).
    return CANCEL;
  }
  return PROCEED;
}

}  // namespace molt_ai::security
