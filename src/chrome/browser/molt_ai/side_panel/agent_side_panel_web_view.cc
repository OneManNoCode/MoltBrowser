// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/side_panel/agent_side_panel_web_view.h"

#include <utility>

#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"

namespace {
// Local literal so this file does NOT include webui_url_constants.h (editing
// that header triggers a ~2h recompile). The chrome://molt-ai-agent/ WebUI and
// its molt:// scheme rewrite are already registered elsewhere.
constexpr char kAgentWebUIURL[] = "chrome://molt-ai-agent/";
}  // namespace

AgentSidePanelWebView::AgentSidePanelWebView(Browser* browser)
    : views::WebView(browser ? browser->profile() : nullptr),
      profile_(browser ? browser->profile() : nullptr) {
  LoadInitialURL(GURL(kAgentWebUIURL));
}

AgentSidePanelWebView::~AgentSidePanelWebView() = default;

bool AgentSidePanelWebView::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  return unhandled_keyboard_event_handler_.HandleKeyboardEvent(
      event, GetFocusManager());
}

void AgentSidePanelWebView::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                   params);
}

BEGIN_METADATA(AgentSidePanelWebView)
END_METADATA
