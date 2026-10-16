// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_location_tab_helper.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/browser/infobars/confirm_infobar_creator.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/molt_ai/tor/onion_location.h"
#include "chrome/browser/molt_ai/tor/onion_util.h"
#include "chrome/browser/molt_ai/tor/tor_manager.h"
#include "chrome/browser/profiles/profile.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "components/infobars/core/confirm_infobar_delegate.h"
#include "components/infobars/core/infobar.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/common/referrer.h"
#include "net/http/http_response_headers.h"
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
      handle->IsSameDocument()) {
    return;
  }
  // A new document: forget any pending <meta> read for the previous one.
  ++document_sequence_;
  pending_meta_frame_ = content::GlobalRenderFrameHostId();
  pending_meta_url_ = GURL();
  if (handle->IsErrorPage()) {
    return;
  }
  const net::HttpResponseHeaders* headers = handle->GetResponseHeaders();
  if (HasOnionLocationHeader(headers)) {
    // The header is authoritative; <meta> elements are then ignored.
    const GURL onion_url = GetOnionLocation(handle->GetURL(), headers);
    if (onion_url.is_valid() && ShouldOffer()) {
      ShowInfoBar(onion_url);
    }
    return;
  }
  // No header: read <meta http-equiv="onion-location"> once the document
  // is parsed, if this page could offer an onion version at all.
  const GURL& url = handle->GetURL();
  if (url.is_valid() && ShouldHonorOnionLocation(url.scheme(), url.host()) &&
      ShouldOffer() && handle->GetRenderFrameHost()) {
    pending_meta_frame_ = handle->GetRenderFrameHost()->GetGlobalId();
    pending_meta_url_ = url;
  }
}

void OnionLocationTabHelper::DOMContentLoaded(
    content::RenderFrameHost* render_frame_host) {
  if (!render_frame_host || !pending_meta_frame_ ||
      render_frame_host->GetGlobalId() != pending_meta_frame_ ||
      !render_frame_host->IsInPrimaryMainFrame() ||
      render_frame_host->GetLastCommittedURL() != pending_meta_url_) {
    return;
  }
  pending_meta_frame_ = content::GlobalRenderFrameHostId();
  render_frame_host->ExecuteJavaScriptInIsolatedWorld(
      OnionLocationMetaScript(),
      base::BindOnce(&OnionLocationTabHelper::OnMetaContents,
                     weak_factory_.GetWeakPtr(), document_sequence_,
                     pending_meta_url_),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void OnionLocationTabHelper::OnMetaContents(uint64_t document_sequence,
                                            GURL page_url,
                                            base::Value script_result) {
  // The answer comes from the renderer: validate everything, CHECK nothing.
  if (document_sequence != document_sequence_) {
    return;  // The tab has moved on to another document.
  }
  std::optional<std::vector<std::string>> contents =
      OnionLocationMetaContents(script_result);
  if (!contents) {
    return;
  }
  const GURL onion_url = GetOnionLocationFromMeta(page_url, *contents);
  if (onion_url.is_valid() && ShouldOffer()) {
    ShowInfoBar(onion_url);
  }
}

bool OnionLocationTabHelper::ShouldOffer() const {
  // In regular windows, only offer it while the user is using MoltNet.
  return IsMoltNetProfile(Profile::FromBrowserContext(
             web_contents()->GetBrowserContext())) ||
         TorManager::Get()->IsRunning();
}

void OnionLocationTabHelper::ShowInfoBar(const GURL& onion_url) {
  infobars::ContentInfoBarManager* manager =
      infobars::ContentInfoBarManager::FromWebContents(web_contents());
  if (!manager) {
    return;
  }
  const bool in_moltnet = IsMoltNetProfile(
      Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
  manager->AddInfoBar(
      CreateConfirmInfoBar(std::make_unique<OnionLocationInfoBarDelegate>(
          onion_url, in_moltnet)),
      /*replace_existing=*/true);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(OnionLocationTabHelper);

}  // namespace tor
}  // namespace molt_ai
