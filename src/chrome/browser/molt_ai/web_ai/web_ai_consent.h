// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Asking the user before a site may use the on-device model through the
// built-in AI APIs.
//
// The prompt is an infobar on the tab, naming the top-level origin: "Allow"
// / "Block", or "Choose a model" / "Block" when no model is downloaded yet
// ("Choose a model" opens MoltBrowser's model picker, molt://ai; a page
// never starts a download). Blink only lets create() reach the browser in
// that state with a user gesture (availability() says "downloadable"), so a
// page can't raise the prompt by itself. Requests from the same tab and
// origin while it is showing wait for the same answer.
//
// Allow and Block are remembered per profile and top-level origin
// (WebAISitePermissions, web_ai_site_permissions_service.h): on disk for a
// regular profile until the user removes them in molt://ai-settings or clears
// site settings, in memory for Incognito until it closes. Closing the bar
// with its close button answers nothing but counts towards pausing the
// prompt for the site (kDismissalsBeforeEmbargo, web_ai_site_permissions.h);
// a bar that goes away by itself (navigation, the tab closing, a newer bar)
// doesn't count. MoltNet windows never get here (the APIs are off there).

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_

#include "base/functional/callback.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace molt_ai::web_ai {

// Shows the prompt on |rfh|'s tab for its top-level origin, or joins the one
// already showing. |callback| runs asynchronously with the answer, or with
// kAsk if the prompt can't be shown (no infobars in this kind of window, a
// prerendered or cached page) or is closed without an answer.
void ShowWebAIConsentPrompt(content::RenderFrameHost* rfh,
                            bool needs_model,
                            base::OnceCallback<void(SiteConsent)> callback);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_
