// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// AntiRageTabHelper — per-tab WebContentsObserver that injects a small
// piece of detection JS into every http(s) page. The injected script
// listens for click events and watches for the "rage-click" pattern:
// 4+ clicks on the same element within 3 seconds. When detected, it
// overlays a dismissible chip on the page suggesting the user open
// the MoltBrowser side panel and try /simplify or /sandbox.
//
// Why purely client-side (no IPC back to native):
//   - The detector and the UI it produces are both DOM-level. There's
//     nothing useful native has to do here.
//   - Avoids a cross-process round-trip on every click.
//   - The chip auto-dismisses after 10s and self-throttles to one
//     show per 30s, so it can't grow noisy.
//
// Idempotent via window.__moltAntiRageInstalled.

#ifndef CHROME_BROWSER_MOLT_AI_ANTI_RAGE_ANTI_RAGE_TAB_HELPER_H_
#define CHROME_BROWSER_MOLT_AI_ANTI_RAGE_ANTI_RAGE_TAB_HELPER_H_

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace anti_rage {

class AntiRageTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<AntiRageTabHelper> {
 public:
  ~AntiRageTabHelper() override;

  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

 private:
  friend class content::WebContentsUserData<AntiRageTabHelper>;
  explicit AntiRageTabHelper(content::WebContents* contents);
  void InjectDetector();

  base::WeakPtrFactory<AntiRageTabHelper> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace anti_rage
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_ANTI_RAGE_ANTI_RAGE_TAB_HELPER_H_
