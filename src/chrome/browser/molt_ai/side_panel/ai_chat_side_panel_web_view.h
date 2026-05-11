// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class Browser;
class Profile;

// AiChatSidePanelWebView is a views::WebView that hosts the AI chat
// WebUI interface inside the browser side panel. It loads the
// chrome://molt-ai-chat/ URL and manages the lifecycle of the embedded
// web contents.
//
// In addition to hosting the chat, this view observes the owning
// Browser's TabStripModel and pushes the active tab's URL + title into
// the chat WebUI as `window.__moltSetTabContext({url, title})` so the
// chat can ground its answers in "the page the user is looking at".
// The chat HTML/JS opts into this contract by defining the function;
// when undefined the injected JS is a harmless no-op.
class AiChatSidePanelWebView : public views::WebView,
                                public TabStripModelObserver {
  METADATA_HEADER(AiChatSidePanelWebView, views::WebView)

 public:
  // Preferred constructor — observes |browser|'s TabStripModel so the
  // chat learns about active-tab changes.
  explicit AiChatSidePanelWebView(Browser* browser);
  // Backwards-compat overload for call sites that don't have a Browser
  // pointer handy (e.g. tests). The view still loads the chat URL but
  // gets no tab-context updates.
  explicit AiChatSidePanelWebView(Profile* profile);

  AiChatSidePanelWebView(const AiChatSidePanelWebView&) = delete;
  AiChatSidePanelWebView& operator=(const AiChatSidePanelWebView&) = delete;
  ~AiChatSidePanelWebView() override;

  // TabStripModelObserver:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

 private:
  // Push the active tab's URL + title into the chat WebUI.
  void PushActiveTabContext();

  const raw_ptr<Profile> profile_;
  raw_ptr<Browser> browser_ = nullptr;  // null when constructed without one
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
