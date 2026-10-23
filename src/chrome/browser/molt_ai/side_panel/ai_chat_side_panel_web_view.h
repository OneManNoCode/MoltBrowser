// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
#define CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_

#include <cstdint>  // for uint32_t — required on Linux under -fmodules
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/controls/webview/webview.h"

class BrowserWindowInterface;
class Profile;

// AiChatSidePanelWebView is a views::WebView that hosts the AI chat
// WebUI interface inside the browser side panel. It loads
// chrome://molt-ai-chat/ and manages the lifecycle of the embedded
// web contents.
//
// In addition to hosting the chat, this view observes the owning
// Browser's TabStripModel and grounds the chat in the active tab:
//   - On every active-tab change, and whenever the active tab itself
//     navigates (new primary page, its load completing, or a same-document
//     SPA navigation), it asynchronously captures the page text
//     (document.body.innerText, capped), then pushes {url, title, text}
//     into the chat WebUI's MAIN world as `window.__moltSetTabContext(ctx)`
//     (the page's own function; an isolated world can't see it). The same
//     payload is stashed on `window.__moltLastTabContext` for race-free
//     late-binding from the chat page's init script, and the push is
//     repeated once the chat page itself has loaded.
//   - A monotonic generation counter is bumped on every push so stale
//     text-capture replies for a previous page are silently dropped.
//   - Every payload carries nav_entry_id, the unique ID of the tab's last
//     committed NavigationEntry. When the active document changes, the
//     chat gets the new url/title with empty text at once (text_pending),
//     so it never holds the previous page's text under the new page; the
//     chat also compares nav_entry_id with getPageContext before using
//     the text.
class AiChatSidePanelWebView : public views::WebView,
                                public TabStripModelObserver {
  METADATA_HEADER(AiChatSidePanelWebView, views::WebView)

 public:
  // Preferred constructor — observes |browser|'s TabStripModel so the
  // chat learns about active-tab changes.
  explicit AiChatSidePanelWebView(BrowserWindowInterface* browser);
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

  // views::WebView installs itself as the hosted WebContents' delegate but
  // does NOT override HandleKeyboardEvent, so the default no-op drops every
  // shortcut the renderer bounces back as unhandled — including the browser
  // Cut/Copy/Paste/SelectAll command accelerators. That's why Cmd/Ctrl+C/V/X/A
  // were dead in the chat composer textarea while it lived in the side panel
  // (they work in a tab because Browser is the delegate and forwards this).
  // Mirror SidePanelWebUIView / WebDialogView: route the event through the
  // FocusManager so those accelerators fire.
  bool HandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;

  // views::WebView also does NOT override RunFileChooser, so the paperclip
  // attachment picker (<input type=file>) opened no native file dialog in the
  // side panel and the button appeared dead. Forward to FileSelectHelper
  // exactly like Browser::RunFileChooser so the OS file picker opens.
  void RunFileChooser(
      content::RenderFrameHost* render_frame_host,
      scoped_refptr<content::FileSelectListener> listener,
      const blink::mojom::FileChooserParams& params) override;

 private:
  class ActiveTabObserver;
  class ChatPageObserver;

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
                                int entry_id,
                                base::Value page_text);

  // PDF variant of the reply step: receives an AX tree snapshot and
  // flattens its kName attributes into text for the chat. Used when
  // the active tab's MIME type is application/pdf since the PDF
  // viewer's content isn't reachable via document.body.innerText.
  void OnAxTreeCaptured(int64_t generation,
                         std::string url,
                         std::string title,
                         int entry_id,
                         ui::AXTreeUpdate& update);

  // Inject the {url, title, text, nav_entry_id, text_pending} dict into the
  // chat WebUI as JSON. |entry_id| identifies the document |text| came from.
  void InjectContextIntoChat(const std::string& url,
                              const std::string& title,
                              const std::string& text,
                              int entry_id,
                              bool text_pending);

  // Retargets |active_tab_observer_| to the current active tab.
  void ObserveActiveTab();

  const raw_ptr<Profile> profile_;
  raw_ptr<BrowserWindowInterface> browser_ = nullptr;  // null when constructed without one
  // Re-pushes context when the active tab navigates (A7: previously the
  // chat kept answering about the previous page until a tab switch).
  std::unique_ptr<ActiveTabObserver> active_tab_observer_;
  // Re-pushes context once the chat page has loaded (a push before that
  // has nowhere to go).
  std::unique_ptr<ChatPageObserver> chat_page_observer_;
  // Bumped on every active-tab change. Async text captures carry the
  // counter from when they were started; replies for old generations
  // are dropped on arrival.
  int64_t generation_ = 0;

  // The document (nav_entry_id + URL) of the last context delivered to the
  // chat page; -1 until something was delivered.
  int last_injected_entry_id_ = -1;
  std::string last_injected_url_;

  // Set in PushActiveTabContext, read in InjectContextIntoChat. Lets the
  // side-panel chat render an "Anonymous session" banner without having
  // to re-query the BrowserContext.
  bool last_pushed_is_anonymous_ = false;

  // Routes keyboard events the renderer didn't consume back through the
  // FocusManager so the browser Cut/Copy/Paste/SelectAll accelerators work in
  // the chat textarea. Without it the side panel drops them.
  views::UnhandledKeyboardEventHandler unhandled_keyboard_event_handler_;

  base::WeakPtrFactory<AiChatSidePanelWebView> weak_factory_{this};
};

#endif  // CHROME_BROWSER_MOLT_AI_SIDE_PANEL_AI_CHAT_SIDE_PANEL_WEB_VIEW_H_
