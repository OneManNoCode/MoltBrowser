// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Asking the user before a site may use the on-device model through the
// built-in AI APIs, and remembering the answer for the browser session.
//
// The prompt is an infobar on the tab, naming the top-level origin: "Allow"
// / "Block", or "Choose a model" / "Block" when no model is downloaded yet
// ("Choose a model" opens MoltBrowser's model picker, molt://ai; a page
// never starts a download). Blink only lets create() reach the browser in
// that state with a user gesture (availability() says "downloadable"), so a
// page can't raise the prompt by itself. Requests from the same tab and
// origin while it is showing wait for the same answer.
//
// Answers are kept per profile, in memory: Allow and Block last until
// MoltBrowser restarts (the Incognito profile's until it closes). MoltNet
// windows never get here (the APIs are off there).

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_

#include "base/functional/callback.h"
#include "base/supports_user_data.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace url {
class Origin;
}  // namespace url

namespace molt_ai::web_ai {

// One profile's answers (BrowserContext user data).
class WebAIConsentStore : public base::SupportsUserData::Data {
 public:
  // Creates the store on first use.
  static WebAIConsentStore* FromBrowserContext(
      content::BrowserContext* context);

  WebAIConsentStore();
  WebAIConsentStore(const WebAIConsentStore&) = delete;
  WebAIConsentStore& operator=(const WebAIConsentStore&) = delete;
  ~WebAIConsentStore() override;

  SiteConsent Get(const url::Origin& top_level_origin) const;
  void Set(const url::Origin& top_level_origin, SiteConsent consent);

 private:
  ConsentMap answers_;
};

// Shows the prompt on |rfh|'s tab for its top-level origin, or joins the one
// already showing. |callback| runs asynchronously with the answer, or with
// kAsk if the prompt can't be shown (no infobars in this kind of window, a
// prerendered or cached page) or is closed without an answer.
void ShowWebAIConsentPrompt(content::RenderFrameHost* rfh,
                            bool needs_model,
                            base::OnceCallback<void(SiteConsent)> callback);

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONSENT_H_
