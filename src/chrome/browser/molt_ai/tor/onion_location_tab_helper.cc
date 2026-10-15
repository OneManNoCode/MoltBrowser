// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_location_tab_helper.h"

#include <memory>
#include <string>

#include "chrome/browser/infobars/confirm_infobar_creator.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/molt_ai/tor/onion_location.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/infobars/core/confirm_infobar_delegate.h"
#include "components/infobars/core/infobar.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"

namespace molt_ai {
namespace tor {

namespace {

class OnionLocationInfoBarDelegate : public ConfirmInfoBarDelegate {
 public:
  // |in_moltnet|: the tab is in a MoltNet window (open in place) rather than
  // a regular one (open in a new MoltNet window).
  OnionLocationInfoBarDelegate(const GURL& onion_url, bool in_moltnet)
      : onion_url_(onion_url), in_moltnet_(in_moltnet) {}
  OnionLocationInfoBarDelegate(const OnionLocationInfoBarDelegate&) = delete;
  OnionLocationInfoBarDelegate& operator=(const OnionLocationInfoBarDelegate&) =
      delete;
  ~OnionLocationInfoBarDelegate() override = default;

  // ConfirmInfoBarDelegate:
  infobars::InfoBarDelegate::InfoBarIdentifier GetIdentifier() const override {
    return MOLT_ONION_LOCATION_INFOBAR_DELEGATE;
  }
  std::u16string GetMessageText() const override {
    return in_moltnet_
               ? u"This site is also available as a Tor onion service."
               : u"This site is also available as a Tor onion service. Open "
                 u"it in a MoltNet window?";
  }
  int GetButtons() const override { return BUTTON_OK; }
  std::u16string GetButtonLabel(InfoBarButton button) const override {
    return in_moltnet_ ? u"Open .onion version" : u"Open in MoltNet window";
  }
  bool Accept() override {
    content::WebContents* contents =
        infobars::ContentInfoBarManager::WebContentsFromInfoBar(infobar());
    if (!contents) {
      return true;
    }
    if (in_moltnet_) {
      // Same tab, through Tor. No referrer: the onion site needn't learn
      // which clearnet page sent the user.
      contents->OpenURL(
          content::OpenURLParams(onion_url_, content::Referrer(),
                                 WindowOpenDisposition::CURRENT_TAB,
                                 ui::PAGE_TRANSITION_LINK,
                                 /*is_renderer_initiated=*/false),
          /*navigation_handle_callback=*/{});
    } else {
      MoltNetWindow::OpenForUserClick(contents, onion_url_);
    }
    return true;  // Close the infobar.
  }
  // One Onion-Location infobar per tab: a newer one replaces the old one.
  bool EqualsDelegate(infobars::InfoBarDelegate* delegate) const override {
    return delegate->GetIdentifier() == GetIdentifier();
  }

 private:
  const GURL onion_url_;
  const bool in_moltnet_;
};

}  // namespace

OnionLocationTabHelper::OnionLocationTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<OnionLocationTabHelper>(*web_contents) {}

OnionLocationTabHelper::~OnionLocationTabHelper() = default;

void OnionLocationTabHelper::DidFinishNavigation(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame() || !handle->HasCommitted() ||
      handle->IsSameDocument() || handle->IsErrorPage()) {
    return;
  }
  const GURL onion_url =
      GetOnionLocation(handle->GetURL(), handle->GetResponseHeaders());
  if (!onion_url.is_valid()) {
    return;
  }
  const bool in_moltnet = IsMoltNetProfile(
      Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
  // In regular windows, only offer it while the user is using MoltNet.
  if (!in_moltnet && !TorManager::Get()->IsRunning()) {
    return;
  }
  infobars::ContentInfoBarManager* manager =
      infobars::ContentInfoBarManager::FromWebContents(web_contents());
  if (!manager) {
    return;
  }
  manager->AddInfoBar(
      CreateConfirmInfoBar(std::make_unique<OnionLocationInfoBarDelegate>(
          onion_url, in_moltnet)),
      /*replace_existing=*/true);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(OnionLocationTabHelper);

}  // namespace tor
}  // namespace molt_ai
