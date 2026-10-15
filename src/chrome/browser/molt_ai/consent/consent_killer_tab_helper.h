// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ConsentKillerTabHelper — auto-rejects GDPR / cookie consent dialogs.
//
// Injected (isolated world) at DOMContentLoaded into the primary main frame
// and into frames served by known consent-platform (CMP) hosts — Sourcepoint,
// TrustArc etc. render their dialog in a cross-origin iframe the main-frame
// script can't reach. The script:
//   1. clicks a curated vendor *reject* button (OneTrust, Cookiebot,
//      Quantcast, Didomi, Usercentrics, Sourcepoint, ...), searching open
//      shadow roots too (Usercentrics mounts in one);
//   2. otherwise looks for a "Reject all / Only necessary"-style button by
//      text -- but only inside the container of a known CMP framework
//      (OneTrust, Cookiebot, Didomi, ... whose text is about cookies or
//      consent), or anywhere in a known CMP frame/consent page. Never in a
//      site's own dialogs (role=dialog, aria-modal) or site-named banners:
//      it can't click arbitrary page buttons;
//   3. never clicks Customize/Settings/Preferences/Manage-style buttons, nor
//      anything that accepts.
// Site-specific handling beyond that is list-based: uBO's opt-in
// "Cookie Notices" list (catalog id ublock-cookies) hides banners and
// rejects on ~900 known sites with curated `trusted-click-element` rules.
// It runs immediately and then on DOM mutations (throttled), and gives up
// after a few seconds or once something was clicked — no fixed +600/+1800/
// +4000 ms timers.
//
// Off when MoltShield's "consent killer" switch is off or Shields are down
// for the site. With the kMoltShieldConsentKiller kill switch off (or
// --disable-molt-shield) the helper never observes its tab at all.

#ifndef CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_

#include <string_view>

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace molt_ai {
namespace consent {

class ConsentKillerTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<ConsentKillerTabHelper> {
 public:
  ~ConsentKillerTabHelper() override;

  // content::WebContentsObserver:
  void DOMContentLoaded(content::RenderFrameHost* render_frame_host) override;

  // True if `host` belongs to a consent platform that renders its dialog in
  // its own (usually cross-origin) document. Exposed for tests.
  static bool IsKnownCmpHost(std::string_view host);

 private:
  friend class content::WebContentsUserData<ConsentKillerTabHelper>;
  explicit ConsentKillerTabHelper(content::WebContents* contents);

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace consent
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_
