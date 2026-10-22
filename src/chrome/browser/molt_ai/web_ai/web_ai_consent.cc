// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_consent.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/infobars/confirm_infobar_creator.h"
#include "chrome/common/webui_url_constants.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/infobars/core/confirm_infobar_delegate.h"
#include "components/infobars/core/infobar.h"
#include "components/url_formatter/elide_url.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace molt_ai::web_ai {

namespace {

const char kConsentStoreKey[] = "molt_web_ai_consent";

void AnswerLater(base::OnceCallback<void(SiteConsent)> callback,
                 SiteConsent consent) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), consent));
}

class WebAIConsentInfoBarDelegate : public ConfirmInfoBarDelegate {
 public:
  WebAIConsentInfoBarDelegate(const url::Origin& origin, bool needs_model)
      : origin_(origin), needs_model_(needs_model) {}
  WebAIConsentInfoBarDelegate(const WebAIConsentInfoBarDelegate&) = delete;
  WebAIConsentInfoBarDelegate& operator=(const WebAIConsentInfoBarDelegate&) =
      delete;
  // Closed without an answer: replaced, expired on navigation, tab closed.
  ~WebAIConsentInfoBarDelegate() override { Answer(SiteConsent::kAsk); }

  const url::Origin& origin() const { return origin_; }
  bool needs_model() const { return needs_model_; }

  void AddCallback(base::OnceCallback<void(SiteConsent)> callback) {
    callbacks_.push_back(std::move(callback));
  }

  // ConfirmInfoBarDelegate:
  infobars::InfoBarDelegate::InfoBarIdentifier GetIdentifier() const override {
    return MOLT_WEB_AI_CONSENT_INFOBAR_DELEGATE;
  }
  std::u16string GetMessageText() const override {
    const std::u16string site = url_formatter::FormatOriginForSecurityDisplay(
        origin_, url_formatter::SchemeDisplay::OMIT_HTTP_AND_HTTPS);
    return needs_model_
               ? site + u" wants to use on-device AI. Choose a model to "
                        u"download first. It runs on this device; nothing "
                        u"is sent anywhere."
               : site + u" wants to use the AI model on this device. It "
                        u"runs locally; nothing is sent anywhere.";
  }
  int GetButtons() const override { return BUTTON_OK | BUTTON_CANCEL; }
  std::u16string GetButtonLabel(InfoBarButton button) const override {
    if (button == BUTTON_CANCEL) {
      return u"Block";
    }
    return needs_model_ ? u"Choose a model" : u"Allow";
  }
  bool Accept() override {
    content::WebContents* contents =
        infobars::ContentInfoBarManager::WebContentsFromInfoBar(infobar());
    if (contents) {
      WebAIConsentStore::FromBrowserContext(contents->GetBrowserContext())
          ->Set(origin_, SiteConsent::kAllowed);
      if (needs_model_) {
        // MoltBrowser's own model picker; the user downloads from there.
        contents->OpenURL(
            content::OpenURLParams(GURL(chrome::kChromeUIMoltAIURL),
                                   content::Referrer(),
                                   WindowOpenDisposition::NEW_FOREGROUND_TAB,
                                   ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                   /*is_renderer_initiated=*/false),
            /*navigation_handle_callback=*/{});
      }
    }
    Answer(contents ? SiteConsent::kAllowed : SiteConsent::kAsk);
    return true;
  }
  bool Cancel() override {
    content::WebContents* contents =
        infobars::ContentInfoBarManager::WebContentsFromInfoBar(infobar());
    if (contents) {
      WebAIConsentStore::FromBrowserContext(contents->GetBrowserContext())
          ->Set(origin_, SiteConsent::kBlocked);
    }
    Answer(contents ? SiteConsent::kBlocked : SiteConsent::kAsk);
    return true;
  }
  void InfoBarDismissed() override { Answer(SiteConsent::kAsk); }
  // One prompt per tab: a newer one (another origin, or with/without a
  // model) replaces the old one, whose requests get kAsk.
  bool EqualsDelegate(infobars::InfoBarDelegate* delegate) const override {
    return delegate->GetIdentifier() == GetIdentifier();
  }

 private:
  void Answer(SiteConsent consent) {
    std::vector<base::OnceCallback<void(SiteConsent)>> callbacks;
    callbacks.swap(callbacks_);
    for (auto& callback : callbacks) {
      AnswerLater(std::move(callback), consent);
    }
  }

  const url::Origin origin_;
  const bool needs_model_;
  std::vector<base::OnceCallback<void(SiteConsent)>> callbacks_;
};

}  // namespace

// static
WebAIConsentStore* WebAIConsentStore::FromBrowserContext(
    content::BrowserContext* context) {
  auto* store =
      static_cast<WebAIConsentStore*>(context->GetUserData(kConsentStoreKey));
  if (!store) {
    auto owned = std::make_unique<WebAIConsentStore>();
    store = owned.get();
    context->SetUserData(kConsentStoreKey, std::move(owned));
  }
  return store;
}

WebAIConsentStore::WebAIConsentStore() = default;
WebAIConsentStore::~WebAIConsentStore() = default;

SiteConsent WebAIConsentStore::Get(const url::Origin& top_level_origin) const {
  if (top_level_origin.opaque()) {
    return SiteConsent::kAsk;
  }
  return answers_.Get(top_level_origin.Serialize());
}

void WebAIConsentStore::Set(const url::Origin& top_level_origin,
                            SiteConsent consent) {
  if (top_level_origin.opaque()) {
    return;
  }
  answers_.Set(top_level_origin.Serialize(), consent);
}

void ShowWebAIConsentPrompt(content::RenderFrameHost* rfh,
                            bool needs_model,
                            base::OnceCallback<void(SiteConsent)> callback) {
  content::WebContents* contents =
      rfh ? content::WebContents::FromRenderFrameHost(rfh) : nullptr;
  infobars::ContentInfoBarManager* manager =
      contents ? infobars::ContentInfoBarManager::FromWebContents(contents)
               : nullptr;
  // Only the page the tab shows may ask: not a prerendered page, one in the
  // back/forward cache, or a window without infobars (apps, DevTools).
  if (!manager || !rfh->IsActive() ||
      rfh->GetOutermostMainFrame() != contents->GetPrimaryMainFrame()) {
    AnswerLater(std::move(callback), SiteConsent::kAsk);
    return;
  }
  const url::Origin origin =
      rfh->GetOutermostMainFrame()->GetLastCommittedOrigin();
  if (origin.opaque()) {
    AnswerLater(std::move(callback), SiteConsent::kAsk);
    return;
  }
  for (infobars::InfoBar* infobar : manager->infobars()) {
    if (infobar->delegate()->GetIdentifier() !=
        infobars::InfoBarDelegate::MOLT_WEB_AI_CONSENT_INFOBAR_DELEGATE) {
      continue;
    }
    auto* showing =
        static_cast<WebAIConsentInfoBarDelegate*>(infobar->delegate());
    if (showing->origin() == origin && showing->needs_model() == needs_model) {
      showing->AddCallback(std::move(callback));
      return;
    }
  }
  auto delegate =
      std::make_unique<WebAIConsentInfoBarDelegate>(origin, needs_model);
  delegate->AddCallback(std::move(callback));
  // If the infobar can't be added, the delegate's destructor answers kAsk.
  manager->AddInfoBar(CreateConfirmInfoBar(std::move(delegate)),
                      /*replace_existing=*/true);
}

}  // namespace molt_ai::web_ai
