// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OnionLocationTabHelper — surfaces a site's Onion-Location header as an
// infobar. It never redirects on its own: the user has to click.
//
//   * In a MoltNet window: "This site is also available as a Tor onion
//     service. [Open .onion version]" loads the onion URL in the same tab
//     (Tor Browser's ".onion available").
//   * In a regular window, only while MoltNet is running this session (so
//     sites with an onion mirror don't nag everyone): "[Open in MoltNet
//     window]" opens it in a new MoltNet window.
//
// Validation (GetOnionLocation, onion_location.h / onion_util.h): only on
// committed https top-level documents that aren't onion services
// themselves; the header must appear once and be an absolute http(s) URL
// whose host is a v3 onion address, with strict syntax, and GURL must parse
// it to the same scheme and host.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace molt_ai {
namespace tor {

class OnionLocationTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<OnionLocationTabHelper> {
 public:
  OnionLocationTabHelper(const OnionLocationTabHelper&) = delete;
  OnionLocationTabHelper& operator=(const OnionLocationTabHelper&) = delete;
  ~OnionLocationTabHelper() override;

 private:
  friend class content::WebContentsUserData<OnionLocationTabHelper>;

  explicit OnionLocationTabHelper(content::WebContents* web_contents);

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_
