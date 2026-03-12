// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"

#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"

namespace {

// The URL for the AI chat WebUI interface.
// TODO(AiChat): Register this URL as a proper chrome:// WebUI host via
// a WebUIControllerFactory and add it to chrome/common/webui_url_constants.h.
constexpr char kAiChatWebUIURL[] = "chrome://molt-ai-chat/";

}  // namespace

AiChatSidePanelWebView::AiChatSidePanelWebView(Profile* profile)
    : views::WebView(profile), profile_(profile) {
  // TODO(AiChat): Replace LoadInitialURL with proper WebUI hosting via
  // SidePanelWebUIViewT and WebUIContentsWrapperT once the
  // MoltAiChatUI WebUIController is implemented. The current approach
  // directly loads the URL, which will show an error page until the
  // chrome://molt-ai-chat/ host is registered.
  LoadInitialURL(GURL(kAiChatWebUIURL));
}

AiChatSidePanelWebView::~AiChatSidePanelWebView() = default;

BEGIN_METADATA(AiChatSidePanelWebView)
END_METADATA
