// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// ConsentKillerTabHelper — per-tab WebContentsObserver that injects
// a rule-based GDPR / cookie-modal dismisser as soon as the main
// frame finishes loading. Two tiers:
//   1. Curated CSS selectors for the most-deployed CMP vendors
//      (OneTrust, Cookiebot, TrustArc, Quantcast, Didomi, CookieYes,
//      Osano) plus generic aria-label / data-testid patterns.
//   2. Text-based fallback that scans visible <button> + role=button
//      elements for "Reject all" / "Decline all" / "Only necessary"
//      labels.
// Together these handle the long tail of consent modals without
// a per-site rule list or a cloud lookup. The killer is idempotent
// (sets window.__moltConsentKilled after first hit), and re-runs
// at 600ms + 1800ms + 4000ms after page load to catch late-injected
// CMPs.

#ifndef CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
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
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

 private:
  friend class content::WebContentsUserData<ConsentKillerTabHelper>;
  explicit ConsentKillerTabHelper(content::WebContents* contents);
  void InjectKiller();

  base::WeakPtrFactory<ConsentKillerTabHelper> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace consent
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_CONSENT_CONSENT_KILLER_TAB_HELPER_H_
