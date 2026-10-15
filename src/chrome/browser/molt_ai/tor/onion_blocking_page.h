// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// OnionBlockingPage — what a regular (non-MoltNet) window shows instead of a
// .onion site:
//
//   This is a Tor onion site
//   <host> can only be reached through Tor, so this window didn't load it
//   and didn't look up its name on your network. ...
//   [Go back]  [Open in a MoltNet window]
//
// Built on Chromium's committed-interstitial machinery (the shared
// interstitial template in its HTTPS-Only Mode layout), like the HTTPS-Only
// Mode page: OnionNavigationThrottle cancels the navigation with this page's
// HTML, which commits as an error page in its own process. Web pages can't
// script it, and its buttons reach the browser only through
// SecurityInterstitialTabHelper's InterstitialCommands for the frame showing
// it. "Open in a MoltNet window" (CMD_PROCEED) additionally requires a
// transient user activation on that frame, so nothing but a real click (or
// key press) on the page opens a MoltNet window.

#ifndef CHROME_BROWSER_MOLT_AI_TOR_ONION_BLOCKING_PAGE_H_
#define CHROME_BROWSER_MOLT_AI_TOR_ONION_BLOCKING_PAGE_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "components/security_interstitials/content/security_interstitial_page.h"
#include "url/gurl.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace tor {

class OnionBlockingPage
    : public security_interstitials::SecurityInterstitialPage {
 public:
  static const SecurityInterstitialPage::TypeID kTypeForTesting;

  // |onion_url| is the http(s) URL whose host is in .onion.
  OnionBlockingPage(content::WebContents* web_contents, const GURL& onion_url);
  OnionBlockingPage(const OnionBlockingPage&) = delete;
  OnionBlockingPage& operator=(const OnionBlockingPage&) = delete;
  ~OnionBlockingPage() override;

  // security_interstitials::SecurityInterstitialPage:
  void OnInterstitialClosing() override;
  void CommandReceived(const std::string& command) override;
  TypeID GetTypeForTesting() override;

 protected:
  // security_interstitials::SecurityInterstitialPage:
  void PopulateInterstitialStrings(base::DictValue& load_time_data) override;

 private:
  void OpenInMoltNet();
  void OnMoltNetOpened(bool ok, std::string error);

  // True while a MoltNet window is being opened for this page (Tor may take
  // a while to start); further clicks are ignored meanwhile.
  bool opening_ = false;
  base::WeakPtrFactory<OnionBlockingPage> weak_factory_{this};
};

}  // namespace tor
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_TOR_ONION_BLOCKING_PAGE_H_
