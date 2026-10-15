// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/tor/onion_blocking_page.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/molt_ai/tor/molt_net_window.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "components/security_interstitials/content/security_interstitial_controller_client.h"
// Complete type for the (null) std::unique_ptr<SettingsPageHelper> argument.
#include "components/security_interstitials/content/settings_page_helper.h"
#include "components/security_interstitials/core/controller_client.h"
#include "components/security_interstitials/core/metrics_helper.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace molt_ai {
namespace tor {

namespace {

using security_interstitials::MetricsHelper;
using security_interstitials::SecurityInterstitialControllerClient;

std::unique_ptr<MetricsHelper> CreateMetricsHelper(const GURL& url) {
  MetricsHelper::ReportDetails details;
  details.metric_prefix = "molt_onion";
  return std::make_unique<MetricsHelper>(url, details,
                                         /*history_service=*/nullptr);
}

// "Go back" goes back (or to the new-tab page). The page's "proceed" opens a
// MoltNet window instead of proceeding here, so Proceed() is never used.
class OnionControllerClient : public SecurityInterstitialControllerClient {
 public:
  OnionControllerClient(content::WebContents* web_contents, const GURL& url)
      : SecurityInterstitialControllerClient(
            web_contents,
            CreateMetricsHelper(url),
            Profile::FromBrowserContext(web_contents->GetBrowserContext())
                ->GetPrefs(),
            g_browser_process->GetApplicationLocale(),
            GURL(chrome::kChromeUINewTabURL),
            /*settings_page_helper=*/nullptr) {}
  OnionControllerClient(const OnionControllerClient&) = delete;
  OnionControllerClient& operator=(const OnionControllerClient&) = delete;
  ~OnionControllerClient() override = default;

  // security_interstitials::ControllerClient:
  void GoBack() override { GoBackAfterNavigationCommitted(); }
  void Proceed() override {}
};

}  // namespace

// static
const security_interstitials::SecurityInterstitialPage::TypeID
    OnionBlockingPage::kTypeForTesting = &OnionBlockingPage::kTypeForTesting;

OnionBlockingPage::OnionBlockingPage(content::WebContents* web_contents,
                                     const GURL& onion_url)
    : SecurityInterstitialPage(
          web_contents,
          onion_url,
          std::make_unique<OnionControllerClient>(web_contents, onion_url)) {}

OnionBlockingPage::~OnionBlockingPage() = default;

void OnionBlockingPage::OnInterstitialClosing() {}

security_interstitials::SecurityInterstitialPage::TypeID
OnionBlockingPage::GetTypeForTesting() {
  return kTypeForTesting;
}

void OnionBlockingPage::CommandReceived(const std::string& command) {
  // Commands come from the renderer: parse, never CHECK.
  int cmd = 0;
  if (!base::StringToInt(command, &cmd)) {
    return;
  }
  switch (cmd) {
    case security_interstitials::CMD_DONT_PROCEED:
      controller()->GoBack();
      break;
    case security_interstitials::CMD_PROCEED:
      OpenInMoltNet();
      break;
    default:
      // Nothing else is offered on this page.
      break;
  }
}

void OnionBlockingPage::OpenInMoltNet() {
  content::WebContents* contents = web_contents();
  content::RenderFrameHost* frame = contents->GetPrimaryMainFrame();
  // The page is an isolated error page that web content can't script, and
  // only its own frame can send commands; on top of that, only a real click
  // or key press on it may open a MoltNet window.
  if (!frame || !frame->HasTransientUserActivation()) {
    LOG(WARNING) << "[MoltNet] onion page: ignoring open without a user "
                    "gesture";
    return;
  }
  if (opening_) {
    return;
  }
  opening_ = true;
  // Starts Tor if needed (fail-closed: no window unless routing through Tor
  // is verified) and opens |request_url()| in a new MoltNet window; a
  // failure is shown in a dialog. This tab keeps showing this page.
  MoltNetWindow::OpenForUserClick(
      contents, request_url(),
      base::BindOnce(&OnionBlockingPage::OnMoltNetOpened,
                     weak_factory_.GetWeakPtr()));
}

void OnionBlockingPage::OnMoltNetOpened(bool ok, std::string error) {
  opening_ = false;
}

void OnionBlockingPage::PopulateInterstitialStrings(
    base::DictValue& load_time_data) {
  // The shared interstitial template in its HTTPS-Only layout: the primary
  // button sends CMD_DONT_PROCEED, the secondary "proceed" button sends
  // CMD_PROCEED (after the template's anti-clickjacking delay).
  load_time_data.Set("type", "HTTPS_ONLY");
  load_time_data.Set("overridable", false);
  load_time_data.Set("hide_primary_button", false);
  load_time_data.Set("openDetails", "");
  load_time_data.Set("explanationParagraph", "");
  load_time_data.Set("finalParagraph", "");
  load_time_data.Set("optInLink", "");
  load_time_data.Set("enhancedProtectionMessage", "");

  // heading and primaryParagraph are inserted as raw HTML: escape the host.
  const std::string host =
      base::EscapeForHTML(base::UTF16ToUTF8(GetFormattedHostName()));
  load_time_data.Set("tabTitle", "Tor onion site");
  load_time_data.Set("heading", "This is a Tor onion site");
  load_time_data.Set(
      "primaryParagraph",
      "<strong>" + host +
          "</strong> can only be reached through Tor, so this window didn't "
          "load it and didn't look up its name on your network. Open it in a "
          "MoltNet window to visit it over Tor. If MoltNet isn't running, it "
          "starts Tor first, which can take up to a minute.");
  load_time_data.Set("primaryButtonText", "Go back");
  load_time_data.Set("proceedButtonText", "Open in a MoltNet window");
}

}  // namespace tor
}  // namespace molt_ai
