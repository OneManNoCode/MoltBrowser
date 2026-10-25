// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/molt_net_ended_identity_throttle.h"

#include <memory>

#include "chrome/browser/molt_ai/tor/molt_net_identity_end_policy.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_routing.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {
namespace tor {

namespace {

Profile* ProfileOf(content::NavigationHandle& handle) {
  content::WebContents* web_contents = handle.GetWebContents();
  return web_contents
             ? Profile::FromBrowserContext(web_contents->GetBrowserContext())
             : nullptr;
}

}  // namespace

// static
void MoltNetEndedIdentityThrottle::MaybeCreateAndAdd(
    content::NavigationThrottleRegistry& registry) {
  Profile* profile = ProfileOf(registry.GetNavigationHandle());
  if (!profile || !IsMoltNetProfile(profile)) {
    return;
  }
  registry.AddThrottle(
      std::make_unique<MoltNetEndedIdentityThrottle>(registry));
}

MoltNetEndedIdentityThrottle::MoltNetEndedIdentityThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

MoltNetEndedIdentityThrottle::~MoltNetEndedIdentityThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
MoltNetEndedIdentityThrottle::WillStartRequest() {
  return Check();
}

content::NavigationThrottle::ThrottleCheckResult
MoltNetEndedIdentityThrottle::WillRedirectRequest() {
  return Check();
}

content::NavigationThrottle::ThrottleCheckResult
MoltNetEndedIdentityThrottle::WillProcessResponse() {
  return Check();
}

const char* MoltNetEndedIdentityThrottle::GetNameForLogging() {
  return "MoltNetEndedIdentityThrottle";
}

content::NavigationThrottle::ThrottleCheckResult
MoltNetEndedIdentityThrottle::Check() {
  Profile* profile = ProfileOf(*navigation_handle());
  return MoltNetNavigationMayProceed(
             profile && IsMoltNetProfile(profile),
             MoltNetRouting::HasIdentityEnded(profile))
             ? PROCEED
             : CANCEL_AND_IGNORE;
}

}  // namespace tor
}  // namespace molt_ai
