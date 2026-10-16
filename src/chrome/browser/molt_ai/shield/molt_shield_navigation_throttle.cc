// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_navigation_throttle.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/shield/molt_shield_request_utils.h"
#include "chrome/browser/molt_ai/shield/molt_shield_service.h"
#include "chrome/browser/molt_ai/shield/molt_shield_site_settings.h"
#include "chrome/browser/molt_ai/shield/molt_shield_tab_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/molt_ai/shield/molt_shield_features.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "net/base/net_errors.h"

namespace molt_ai::shield {

namespace {

// Upper bound on holding a commit for cosmetic resources. Lookups take
// ~0.01-0.5 ms on the engine sequence (tools/shield_bench) and start with
// the request, so this only bites if the engine is still loading.
constexpr base::TimeDelta kCosmeticsWait = base::Milliseconds(300);

}  // namespace

ShieldNavigationData::ShieldNavigationData(content::NavigationHandle& handle) {}
ShieldNavigationData::~ShieldNavigationData() = default;
NAVIGATION_HANDLE_USER_DATA_KEY_IMPL(ShieldNavigationData);

// static
void MoltShieldNavigationThrottle::MaybeAdd(
    content::NavigationThrottleRegistry& registry) {
  // Kill switches (chrome://flags/#molt-shield-frame-blocking,
  // #molt-shield-cosmetics, --disable-molt-shield): with both off no
  // throttle (and no tab helper) is added.
  const bool frame_blocking = molt_shield::IsFrameBlockingEnabled();
  const bool cosmetics = molt_shield::IsCosmeticsEnabled();
  if (!frame_blocking && !cosmetics) {
    return;
  }
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  // (NavigationThrottleRegistry::IsHTTPOrHTTPS() is gone at 152; it was
  // this check on the URL at registration time.)
  if (!handle.GetURL().SchemeIsHTTPOrHTTPS() || handle.IsSameDocument()) {
    return;
  }
  content::WebContents* web_contents = handle.GetWebContents();
  if (!web_contents) {
    return;
  }
  MoltShieldService* service = MoltShieldService::Get();
  service->RegisterProfile(
      Profile::FromBrowserContext(web_contents->GetBrowserContext()));
  if (!service->IsEnabled()) {
    return;
  }

  // Top-level site this document will belong to.
  const bool is_top_level = handle.IsInOutermostMainFrame();
  GURL top_frame_url = handle.GetURL();
  if (!is_top_level) {
    content::RenderFrameHost* parent = handle.GetParentFrameOrOuterDocument();
    if (!parent) {
      return;
    }
    top_frame_url = parent->GetOutermostMainFrame()->GetLastCommittedURL();
  }
  if (!AreShieldsUpForSite(web_contents->GetBrowserContext(), top_frame_url)) {
    return;
  }
  const bool check_frame = frame_blocking && !is_top_level;
  const bool want_cosmetics = cosmetics && service->IsCosmeticEnabled();
  if (!check_frame && !want_cosmetics) {
    return;
  }

  // Delivers the cosmetics at ReadyToCommitNavigation and keeps per-tab
  // counts.
  MoltShieldTabHelper::CreateForWebContents(web_contents);

  registry.AddThrottle(std::make_unique<MoltShieldNavigationThrottle>(
      registry, check_frame, want_cosmetics));
}

MoltShieldNavigationThrottle::MoltShieldNavigationThrottle(
    content::NavigationThrottleRegistry& registry,
    bool check_frame,
    bool want_cosmetics)
    : content::NavigationThrottle(registry),
      check_frame_(check_frame),
      want_cosmetics_(want_cosmetics) {}

MoltShieldNavigationThrottle::~MoltShieldNavigationThrottle() = default;

const char* MoltShieldNavigationThrottle::GetNameForLogging() {
  return "MoltShieldNavigationThrottle";
}

content::NavigationThrottle::ThrottleCheckResult
MoltShieldNavigationThrottle::WillStartRequest() {
  return OnRequest();
}

content::NavigationThrottle::ThrottleCheckResult
MoltShieldNavigationThrottle::WillRedirectRequest() {
  return OnRequest();
}

content::NavigationThrottle::ThrottleCheckResult
MoltShieldNavigationThrottle::OnRequest() {
  const GURL& url = navigation_handle()->GetURL();
  if (want_cosmetics_) {
    RequestCosmetics(url);
  }
  if (!check_frame_) {
    // Top-level document: never blocked.
    return PROCEED;
  }
  content::RenderFrameHost* parent =
      navigation_handle()->GetParentFrameOrOuterDocument();
  std::optional<url::Origin> initiator =
      navigation_handle()->GetInitiatorOrigin();
  if (!initiator && parent) {
    initiator = parent->GetLastCommittedOrigin();
  }
  std::optional<CheckInput> input =
      MakeCheckInput(url, initiator, kTypeSubdocument, "GET");
  if (!input) {
    return PROCEED;
  }
  deferred_for_frame_ = true;
  MoltShieldService::Get()->Check(
      std::move(*input),
      base::BindOnce(&MoltShieldNavigationThrottle::OnFrameChecked,
                     weak_factory_.GetWeakPtr(), url));
  return DEFER;
}

void MoltShieldNavigationThrottle::OnFrameChecked(GURL url,
                                                  CheckResult result) {
  deferred_for_frame_ = false;
  MoltShieldService::Get()->RecordCheck(result, kTypeSubdocument);
  if (result.should_block) {
    RecordBlockedForFrame(navigation_handle()->GetParentFrameOrOuterDocument());
    // Collapse the ad frame rather than showing an error page in it. May
    // delete `this`.
    CancelDeferredNavigation(content::NavigationThrottle::ThrottleCheckResult(
        BLOCK_REQUEST_AND_COLLAPSE, net::ERR_BLOCKED_BY_CLIENT));
    return;
  }
  Resume();  // May delete `this`.
}

void MoltShieldNavigationThrottle::RequestCosmetics(const GURL& url) {
  cosmetics_url_ = url;
  cosmetics_.reset();
  MoltShieldService::Get()->GetCosmetic(
      url, base::BindOnce(&MoltShieldNavigationThrottle::OnCosmetics,
                          weak_factory_.GetWeakPtr(), url));
}

content::NavigationThrottle::ThrottleCheckResult
MoltShieldNavigationThrottle::WillProcessResponse() {
  if (!want_cosmetics_ || navigation_handle()->IsDownload()) {
    return PROCEED;
  }
  if (cosmetics_) {
    StashCosmetics();
    return PROCEED;
  }
  deferred_for_cosmetics_ = true;
  cosmetics_timeout_.Start(
      FROM_HERE, kCosmeticsWait,
      base::BindOnce(&MoltShieldNavigationThrottle::OnCosmeticsTimeout,
                     weak_factory_.GetWeakPtr()));
  return DEFER;
}

void MoltShieldNavigationThrottle::OnCosmetics(GURL url, CosmeticData data) {
  if (url != cosmetics_url_) {
    return;  // Superseded by a redirect.
  }
  cosmetics_ = std::move(data);
  if (deferred_for_cosmetics_) {
    deferred_for_cosmetics_ = false;
    cosmetics_timeout_.Stop();
    StashCosmetics();
    Resume();  // May delete `this`.
  }
}

void MoltShieldNavigationThrottle::OnCosmeticsTimeout() {
  if (!deferred_for_cosmetics_) {
    return;
  }
  // Commit without cosmetics rather than stall the page; network blocking
  // still applies.
  deferred_for_cosmetics_ = false;
  Resume();  // May delete `this`.
}

void MoltShieldNavigationThrottle::StashCosmetics() {
  if (!cosmetics_ || (cosmetics_->hide_css.empty() &&
                      cosmetics_->scriptlet_js.empty() &&
                      cosmetics_->procedural_json.empty() &&
                      cosmetics_->generichide)) {
    return;
  }
  ShieldNavigationData* stash =
      ShieldNavigationData::GetOrCreateForNavigationHandle(
          *navigation_handle());
  stash->url = cosmetics_url_;
  stash->data = std::move(*cosmetics_);
  cosmetics_.reset();
}

}  // namespace molt_ai::shield
