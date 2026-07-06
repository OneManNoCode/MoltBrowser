// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_

#include <cstdint>  // for uint32_t — required on Linux under -fmodules
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class Browser;
class Profile;

// AiChatSidePanelWebView is a views::WebView that hosts the AI chat
// WebUI interface inside the browser side panel. It loads
// chrome://molt-ai-chat/ and manages the lifecycle of the embedded
// web contents.
//
// In addition to hosting the chat, this view observes the owning
// Browser's TabStripModel and grounds the chat in the active tab:
//   - On every active-tab change it asynchronously captures the page
//     text (document.body.innerText, capped at ~5KB), then pushes
//     {url, title, text} into the chat WebUI as
//     `window.__moltSetTabContext(ctx)`. The same payload is also
//     stashed on `window.__moltLastTabContext` for race-free
//     late-binding from the chat HTML's init script.
//   - A monotonic generation counter is bumped on every active-tab
//     change so stale text-capture replies for a previously-active
//     tab are silently dropped.
class AiChatSidePanelWebView : public views::WebView,
                                public TabStripModelObserver {
  METADATA_HEADER(AiChatSidePanelWebView, views::WebView)

 public:
  // Preferred constructor — observes |browser|'s TabStripModel so the
  // chat learns about active-tab changes.
  explicit AiChatSidePanelWebView(Browser* browser);
  // Backwards-compat overload for call sites that don't have a Browser
  // pointer handy. The view still loads the chat URL but gets no
  // tab-context updates.
  explicit AiChatSidePanelWebView(Profile* profile);

  AiChatSidePanelWebView(const AiChatSidePanelWebView&) = delete;
  AiChatSidePanelWebView& operator=(const AiChatSidePanelWebView&) = delete;
  ~AiChatSidePanelWebView() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

  // content::WebContentsDelegate:
  //
  // views::WebView installs ITSELF as the hosted WebContents' delegate but
  // doesn't override the media-permission hooks, so the default
  // WebContentsDelegate implementation denies every getUserMedia() with
  // NOT_SUPPORTED. That's why the chat's mic button worked when the chat was
  // opened as a tab (Browser's delegate forwards to
  // MediaCaptureDevicesDispatcher) but always failed in the side panel.
  // Forward both hooks to MediaCaptureDevicesDispatcher, exactly like
  // Browser::RequestMediaAccessPermission does — the chrome:// scheme is
  // allowlisted for MEDIASTREAM_MIC, so the chat WebUI's mic is auto-granted
  // without a prompt on all platforms.
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(content::RenderFrameHost* render_frame_host,
                                  const url::Origin& security_origin,
                                  blink::mojom::MediaStreamType type) override;

 private:
  // Kick off the active-tab context push. Async: first ExecuteJS on
  // the active tab to grab innerText, then ExecuteJS on the chat
  // WebUI to deliver the {url, title, text} payload.
  void PushActiveTabContext();

  // Reply step: receives the captured page text and pushes the full
  // context to the chat WebUI. |generation| tags the request — if it
  // doesn't match generation_ on arrival we drop the result because
  // the active tab has changed since the capture started.
  void OnActiveTabTextCaptured(int64_t generation,
                                std::string url,
                                std::string title,
                                base::Value page_text);

  // PDF variant of the reply step: receives an AX tree snapshot and
  // flattens its kName attributes into text for the chat. Used when
  // the active tab's MIME type is application/pdf since the PDF
  // viewer's content isn't reachable via document.body.innerText.
  void OnAxTreeCaptured(int64_t generation,
                         std::string url,
                         std::string title,
                         ui::AXTreeUpdate& update);

  // Inject the {url, title, text} dict into the chat WebUI as JSON.
  void InjectContextIntoChat(const std::string& url,
                              const std::string& title,
                              const std::string& text);

  const raw_ptr<Profile> profile_;
  raw_ptr<Browser> browser_ = nullptr;  // null when constructed without one
  // Bumped on every active-tab change. Async text captures carry the
  // counter from when they were started; replies for old generations
  // are dropped on arrival.
  int64_t generation_ = 0;

  // Set in PushActiveTabContext, read in InjectContextIntoChat. Lets the
  // side-panel chat render an "Anonymous session" banner without having
  // to re-query the BrowserContext.
  bool last_pushed_is_anonymous_ = false;

  base::WeakPtrFactory<AiChatSidePanelWebView> weak_factory_{this};
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
