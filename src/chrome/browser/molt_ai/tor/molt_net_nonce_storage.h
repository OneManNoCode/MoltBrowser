// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltNetNonceStorageTabHelper — MoltNet's storage keyed by a page's nonce
// goes with the page, and a top-level site holds a bounded number of such
// groups at once (molt_net_nonce_storage_policy.h says why and how).
//
// Attached to every tab of a MoltNet profile (tab_helpers.cc). It sees each
// frame of the tab as it is made and as it commits a navigation, and notes,
// on the frame's page (content::PageUserData), the nonces its frames use (a
// credentialless iframe's, a fenced frame's), through their storage keys
// and their navigations' requests (committed or not), and the storage keys
// (at most kMoltNetNonceStorageKeysPerGroup a group). When the last page
// that used a nonce goes, the storage partition drops what its frames
// stored (content::DOMStorageContext::MoltDropNonce(),
// storage::QuotaManager::MoltDropNonce(), the media-license data of each
// storage key) and the network context their cookies and the network state
// kept under the nonce (network::mojom::NetworkContext::MoltDropNonce()),
// again a minute later; past a site's bound, or its own, a group is closed
// the same way, everything it could store refused until its page goes.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_H_

#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class NavigationHandle;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace molt_ai::tor {

class MoltNetNonceStorageTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<MoltNetNonceStorageTabHelper> {
 public:
  // Attaches one to `web_contents` if it belongs to a MoltNet profile.
  static void MaybeCreateForWebContents(content::WebContents* web_contents);

  MoltNetNonceStorageTabHelper(const MoltNetNonceStorageTabHelper&) = delete;
  MoltNetNonceStorageTabHelper& operator=(
      const MoltNetNonceStorageTabHelper&) = delete;
  ~MoltNetNonceStorageTabHelper() override;

  // content::WebContentsObserver:
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidRedirectNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;

 private:
  friend class content::WebContentsUserData<MoltNetNonceStorageTabHelper>;
  explicit MoltNetNonceStorageTabHelper(content::WebContents* web_contents);

  // Notes `render_frame_host`'s storage key on its page, if it has a nonce.
  void Note(content::RenderFrameHost* render_frame_host);
  // Notes the cookie partition of the URL a navigation with a nonce requests
  // (as it starts and at each redirect) on the page of the frame.
  void NoteRequest(content::NavigationHandle* navigation_handle);

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace molt_ai::tor

#endif  // CHROME_BROWSER_MOLT_AI_TOR_MOLT_NET_NONCE_STORAGE_H_
