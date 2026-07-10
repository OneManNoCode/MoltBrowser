// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AGENT_SIDE_PANEL_WEB_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AGENT_SIDE_PANEL_WEB_VIEW_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "components/input/native_web_keyboard_event.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"

class Browser;
class Profile;

// AgentSidePanelWebView hosts the Agent-mode automation studio WebUI
// (chrome://molt-ai-agent/) in the browser side panel. It is registered as the
// kMoltAgent content SidePanelEntry, which shares the single content side panel
// with the kMoltAiChat entry — so opening Agent mode automatically swaps out
// the AI chat panel (and vice-versa).
class AgentSidePanelWebView : public views::WebView {
  METADATA_HEADER(AgentSidePanelWebView, views::WebView)

 public:
  explicit AgentSidePanelWebView(Browser* browser);
  AgentSidePanelWebView(const AgentSidePanelWebView&) = delete;
  AgentSidePanelWebView& operator=(const AgentSidePanelWebView&) = delete;
  ~AgentSidePanelWebView() override;

  // content::WebContentsDelegate: views::WebView is the hosted WebContents'
  // delegate but does NOT override these, so without them Cmd/Ctrl+C/V/X are
  // dropped and the OS file picker never opens inside the side panel (same
  // class of bug we fixed for the AI chat panel). Forward keyboard shortcuts
  // through the FocusManager and the file chooser through FileSelectHelper.
  bool HandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;
  void RunFileChooser(
      content::RenderFrameHost* render_frame_host,
      scoped_refptr<content::FileSelectListener> listener,
      const blink::mojom::FileChooserParams& params) override;

 private:
  const raw_ptr<Profile> profile_;
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AGENT_SIDE_PANEL_WEB_VIEW_H_
