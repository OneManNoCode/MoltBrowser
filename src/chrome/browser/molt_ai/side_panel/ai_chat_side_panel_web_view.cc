// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"

namespace {

// The URL for the AI chat WebUI interface.
constexpr char kAiChatWebUIURL[] = "chrome://molt-ai-chat/";

// How much innerText we capture from the active tab. 5 KB is enough
// to fit a typical article opening in TinyLlama's 2K-ish token
// context window without crowding out the user's actual prompt.
constexpr int kPageTextCharCap = 5000;

}  // namespace

AiChatSidePanelWebView::AiChatSidePanelWebView(Browser* browser)
    : views::WebView(browser ? browser->profile() : nullptr),
      profile_(browser ? browser->profile() : nullptr),
      browser_(browser) {
  LoadInitialURL(GURL(kAiChatWebUIURL));
  if (browser_ && browser_->tab_strip_model()) {
    browser_->tab_strip_model()->AddObserver(this);
    PushActiveTabContext();
  }
}

AiChatSidePanelWebView::AiChatSidePanelWebView(Profile* profile)
    : views::WebView(profile), profile_(profile), browser_(nullptr) {
  LoadInitialURL(GURL(kAiChatWebUIURL));
}

AiChatSidePanelWebView::~AiChatSidePanelWebView() {
  if (browser_ && browser_->tab_strip_model())
    browser_->tab_strip_model()->RemoveObserver(this);
}

void AiChatSidePanelWebView::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed())
    PushActiveTabContext();
}

void AiChatSidePanelWebView::PushActiveTabContext() {
  if (!browser_ || !browser_->tab_strip_model())
    return;
  content::WebContents* active =
      browser_->tab_strip_model()->GetActiveWebContents();
  if (!active || !active->GetPrimaryMainFrame())
    return;

  std::string url = active->GetLastCommittedURL().spec();
  std::string title = base::UTF16ToUTF8(active->GetTitle());

  // Bump the generation; capture for the previous tab is now stale.
  int64_t my_gen = ++generation_;

  // Skip text capture for non-web schemes — chrome://, molt://, etc.
  // don't host meaningful "page content" and Chromium often refuses
  // to ExecuteJS in their isolated worlds anyway. Push the metadata
  // immediately with empty text.
  if (!active->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    InjectContextIntoChat(url, title, std::string());
    return;
  }

  // Capture document.body.innerText, capped. The cap goes inside the
  // JS so we don't ship a huge string across the IPC just to truncate
  // it here. Wrapped in a try/catch so a hostile page that overrides
  // String.prototype.slice doesn't blow up.
  std::u16string js =
      u"(function(){try{return (document.body && document.body.innerText) ? "
      u"document.body.innerText.slice(0, " +
      base::UTF8ToUTF16(base::NumberToString(kPageTextCharCap)) +
      u") : '';}catch(e){return '';}})()";

  active->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      js,
      base::BindOnce(&AiChatSidePanelWebView::OnActiveTabTextCaptured,
                     weak_factory_.GetWeakPtr(), my_gen, url, title),
      /*world_id=*/1);
}

void AiChatSidePanelWebView::OnActiveTabTextCaptured(int64_t generation,
                                                       std::string url,
                                                       std::string title,
                                                       base::Value page_text) {
  // Drop the reply if the user switched tabs while we were capturing.
  if (generation != generation_)
    return;
  std::string text = page_text.is_string() ? page_text.GetString()
                                            : std::string();
  InjectContextIntoChat(url, title, text);
}

void AiChatSidePanelWebView::InjectContextIntoChat(const std::string& url,
                                                     const std::string& title,
                                                     const std::string& text) {
  if (!web_contents() || !web_contents()->GetPrimaryMainFrame())
    return;

  // Build a JSON-safe context payload. This fork uses base::DictValue
  // (not base::Value::Dict) — same pattern as automation_script.cc.
  base::DictValue ctx;
  ctx.Set("url", url);
  ctx.Set("title", title);
  ctx.Set("text", text);

  std::string json;
  if (!base::JSONWriter::Write(ctx, &json))
    return;

  // Idempotent injection: stash on window.__moltLastTabContext so the
  // chat page can pick it up on its own init (race-free), and call
  // window.__moltSetTabContext if it's already defined.
  std::string js =
      "(function(c) {"
      "  window.__moltLastTabContext = c;"
      "  if (typeof window.__moltSetTabContext === 'function') {"
      "    try { window.__moltSetTabContext(c); } catch (e) {}"
      "  }"
      "})(" + json + ");";
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(js),
      base::DoNothing(),
      /*world_id=*/1);
}

BEGIN_METADATA(AiChatSidePanelWebView)
END_METADATA
