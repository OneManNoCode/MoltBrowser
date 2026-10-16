// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OnionLocationTabHelper — surfaces a site's Onion-Location (header or
// <meta>) as an infobar. It never redirects on its own: the user has to
// click.
//
//   * In a MoltNet window: "This site is also available as a Tor onion
//     service. [Open .onion version]" loads the onion URL in the same tab
//     (Tor Browser's ".onion available").
//   * In a regular window, only while MoltNet is running this session (so
//     sites with an onion mirror don't nag everyone): "[Open in MoltNet
//     window]" opens it in a new MoltNet window.
//
// Where the offer comes from: the response's Onion-Location header or,
// when the response has no such header, a <meta http-equiv=
// "onion-location"> in the document's <head>, read at DOMContentLoaded by a
// short script in an isolated world (OnionLocationMetaScript). Validation is
// the same for both (onion_location.h / onion_util.h): only on committed
// https top-level documents that aren't onion services themselves; the
// value must appear once and be an absolute http(s) URL whose host is a v3
// onion address, with strict syntax, and GURL must parse it to the same
// scheme and host. The renderer's answer is treated as untrusted input.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_

#include <stdint.h>

#include "base/memory/weak_ptr.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"

namespace base {
class Value;
}  // namespace base

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
  void DOMContentLoaded(content::RenderFrameHost* render_frame_host) override;

  // True iff an offer may be shown in this tab now: always in a MoltNet
  // window, in a regular one only while MoltNet's Tor is running.
  bool ShouldOffer() const;
  void ShowInfoBar(const GURL& onion_url);
  void OnMetaContents(uint64_t document_sequence,
                      GURL page_url,
                      base::Value script_result);

  // Bumped on every cross-document commit in the primary main frame, so a
  // late answer about an earlier document is dropped.
  uint64_t document_sequence_ = 0;
  // The committed document whose <meta> to read at DOMContentLoaded (set
  // only when its response had no Onion-Location header and the page may
  // offer one at all).
  content::GlobalRenderFrameHostId pending_meta_frame_;
  GURL pending_meta_url_;

  base::WeakPtrFactory<OnionLocationTabHelper> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_LOCATION_TAB_HELPER_H_
