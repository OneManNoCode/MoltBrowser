// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"

#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
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

}  // namespace

AiChatSidePanelWebView::AiChatSidePanelWebView(Browser* browser)
    : views::WebView(browser ? browser->profile() : nullptr),
      profile_(browser ? browser->profile() : nullptr),
      browser_(browser) {
  LoadInitialURL(GURL(kAiChatWebUIURL));
  if (browser_ && browser_->tab_strip_model()) {
    browser_->tab_strip_model()->AddObserver(this);
    // Push the initial context as soon as the chat WebUI is ready.
    // The injection is idempotent — if window.__moltSetTabContext
    // isn't defined yet, the JS sets window.__moltLastTabContext for
    // the page to read on its own init.
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
  if (!browser_ || !browser_->tab_strip_model() || !web_contents() ||
      !web_contents()->GetPrimaryMainFrame()) {
    return;
  }
  content::WebContents* active =
      browser_->tab_strip_model()->GetActiveWebContents();
  if (!active)
    return;

  // Build a JSON-safe context payload. This fork uses base::DictValue
  // rather than base::Value::Dict — see automation_script.cc for the
  // same pattern.
  base::DictValue ctx;
  ctx.Set("url", active->GetLastCommittedURL().spec());
  ctx.Set("title", base::UTF16ToUTF8(active->GetTitle()));

  std::string json;
  if (!base::JSONWriter::Write(ctx, &json))
    return;

  // Idempotent injection: stash on window.__moltLastTabContext so the
  // page can pick it up on init, and also call __moltSetTabContext if
  // it's already defined. Either path is a harmless no-op when the
  // page hasn't opted in.
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
