// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_

#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"

class Profile;

// AiChatSidePanelWebView is a views::WebView that hosts the AI chat
// WebUI interface inside the browser side panel. It loads the
// chrome://molt-ai-chat/ URL and manages the lifecycle of the embedded
// web contents.
//
// TODO(AiChat): Once the WebUI controller for chrome://molt-ai-chat/ is
// implemented, this class should be updated to use SidePanelWebUIViewT
// (following the pattern from ReadLaterSidePanelWebView) for proper
// integration with the side panel WebUI infrastructure.
class AiChatSidePanelWebView : public views::WebView {
  METADATA_HEADER(AiChatSidePanelWebView, views::WebView)

 public:
  explicit AiChatSidePanelWebView(Profile* profile);
  AiChatSidePanelWebView(const AiChatSidePanelWebView&) = delete;
  AiChatSidePanelWebView& operator=(const AiChatSidePanelWebView&) = delete;
  ~AiChatSidePanelWebView() override;

 private:
  const raw_ptr<Profile> profile_;
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
