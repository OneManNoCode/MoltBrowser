// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltShieldTabHelper — per-tab MoltShield state:
//  * Cosmetic delivery: at ReadyToCommitNavigation it sends the resources
//    MoltShieldNavigationThrottle stashed on the NavigationHandle to the
//    committing frame's CosmeticFilterAgent over a channel-associated
//    interface (ordered before the commit IPC), together with a
//    CosmeticFiltersHost endpoint scoped to that document for the generic
//    class/id pass.
//  * The blocked-request count of the tab's current page (for the
//    MoltShield settings section).
//
// Created on demand by MoltShieldNavigationThrottle::MaybeAdd.

#ifndef CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TAB_HELPER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "chrome/common/molt_ai/shield/cosmetic_filters.mojom.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "url/gurl.h"

namespace molt_ai::shield {

class MoltShieldTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<MoltShieldTabHelper>,
      public molt_shield::mojom::CosmeticFiltersHost {
 public:
  MoltShieldTabHelper(const MoltShieldTabHelper&) = delete;
  MoltShieldTabHelper& operator=(const MoltShieldTabHelper&) = delete;
  ~MoltShieldTabHelper() override;

  // Every live tab with a helper (UI thread), for the settings section.
  static std::vector<MoltShieldTabHelper*> GetAll();

  // Requests blocked on the tab's current (primary) page.
  int64_t GetBlockedCount();

  // content::WebContentsObserver:
  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override;

  // molt_shield::mojom::CosmeticFiltersHost:
  void HiddenClassIdSelectors(
      const std::vector<std::string>& classes,
      const std::vector<std::string>& ids,
      HiddenClassIdSelectorsCallback callback) override;

 private:
  friend class content::WebContentsUserData<MoltShieldTabHelper>;
  explicit MoltShieldTabHelper(content::WebContents* web_contents);

  struct HostContext {
    GURL document_url;
    content::GlobalRenderFrameHostId frame_id;
  };

  mojo::ReceiverSet<molt_shield::mojom::CosmeticFiltersHost, HostContext>
      host_receivers_;

  base::WeakPtrFactory<MoltShieldTabHelper> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace molt_ai::shield

#endif  // CHROME_BROWSER_MOLT_AI_SHIELD_MOLT_SHIELD_TAB_HELPER_H_
