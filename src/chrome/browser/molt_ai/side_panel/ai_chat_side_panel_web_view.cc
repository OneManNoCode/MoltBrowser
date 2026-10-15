// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/molt_ai/side_panel/ai_chat_side_panel_web_view.h"

#include "content/public/browser/browser_context.h"

#include <map>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/pdf/common/constants.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/url_constants.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_update.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"

namespace {

// The URL for the AI chat WebUI interface.
constexpr char kAiChatWebUIURL[] = "chrome://molt-ai-chat/";
constexpr char kAiChatWebUIHost[] = "molt-ai-chat";

// How much innerText we capture from the active tab. 50 KB lets us
// hold the full visible content of long articles / docs / search
// results. The chat JS chunks this and runs a query-keyword overlap
// retrieval to pick the top-K chunks at prompt-build time so the
// LLM context window doesn't overflow.
constexpr int kPageTextCharCap = 50000;

// Walk an AX tree update and concatenate every node's accessible name.
// Used for PDF text extraction since the PDF viewer is an isolated
// <embed> whose innerText is not reachable from JS. The PDFium-side
// accessibility tree exposes the document text as kName attributes on
// the leaf nodes — that's exactly what we need for the chat to read.
std::string FlattenAxTreeText(const ui::AXTreeUpdate& update, int cap) {
  std::string out;
  out.reserve(8192);
  for (const ui::AXNodeData& node : update.nodes) {
    if (!node.HasStringAttribute(ax::mojom::StringAttribute::kName))
      continue;
    const std::string& name =
        node.GetStringAttribute(ax::mojom::StringAttribute::kName);
    if (name.empty())
      continue;
    if (!out.empty())
      out += '\n';
    out += name;
    if (static_cast<int>(out.size()) >= cap) {
      out.resize(cap);
      break;
    }
  }
  return out;
}

// Identifies the document a context push describes: the unique ID of the
// tab's last committed NavigationEntry (unique across tabs; a new one for
// every new-document or history-creating navigation). The chat handler's
// getPageContext reports the same value for the active tab, so the chat can
// tell whether the text it holds belongs to the page shown right now.
int CommittedEntryId(content::WebContents* contents) {
  content::NavigationEntry* entry =
      contents->GetController().GetLastCommittedEntry();
  return entry ? entry->GetUniqueID() : 0;
}

}  // namespace

// Follows whichever tab is active and re-grounds the chat when that tab
// navigates in place (A7). Without this the chat kept the previous page's
// text until the user switched tabs or reopened the panel.
class AiChatSidePanelWebView::ActiveTabObserver
    : public content::WebContentsObserver {
 public:
  explicit ActiveTabObserver(AiChatSidePanelWebView* owner) : owner_(owner) {}

  void SetTab(content::WebContents* tab) {
    if (web_contents() != tab) Observe(tab);
  }

  // content::WebContentsObserver:
  // A new primary page committed: push its url/title right away (this also
  // replaces the previous page's text) ...
  void PrimaryPageChanged(content::Page& page) override {
    owner_->PushActiveTabContext();
  }
  // ... and again once its document has loaded, when innerText is ready.
  void DocumentOnLoadCompletedInPrimaryMainFrame() override {
    owner_->PushActiveTabContext();
  }
  // Same-document navigations (SPA route changes, history.pushState)
  // change the content without a new primary page.
  void DidFinishNavigation(content::NavigationHandle* navigation) override {
    if (navigation->IsInPrimaryMainFrame() && navigation->HasCommitted() &&
        navigation->IsSameDocument() && !navigation->IsErrorPage()) {
      owner_->PushActiveTabContext();
    }
  }

 private:
  const raw_ptr<AiChatSidePanelWebView> owner_;
};

// Watches the chat page itself: context pushed before it finished loading
// had no page to land in, so push again once it has.
class AiChatSidePanelWebView::ChatPageObserver
    : public content::WebContentsObserver {
 public:
  ChatPageObserver(AiChatSidePanelWebView* owner,
                   content::WebContents* chat_contents)
      : content::WebContentsObserver(chat_contents), owner_(owner) {}

  // content::WebContentsObserver:
  void DocumentOnLoadCompletedInPrimaryMainFrame() override {
    owner_->PushActiveTabContext();
  }

 private:
  const raw_ptr<AiChatSidePanelWebView> owner_;
};

AiChatSidePanelWebView::AiChatSidePanelWebView(Browser* browser)
    : views::WebView(browser ? browser->profile() : nullptr),
      profile_(browser ? browser->profile() : nullptr),
      browser_(browser) {
  LoadInitialURL(GURL(kAiChatWebUIURL));
  if (browser_ && browser_->tab_strip_model()) {
    active_tab_observer_ = std::make_unique<ActiveTabObserver>(this);
    if (web_contents()) {
      chat_page_observer_ =
          std::make_unique<ChatPageObserver>(this, web_contents());
    }
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
  // Stop observing before the rest of the view is torn down.
  chat_page_observer_.reset();
  active_tab_observer_.reset();
}

void AiChatSidePanelWebView::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  // Without this override the base WebContentsDelegate denies every
  // getUserMedia() with NOT_SUPPORTED, so the chat's voice/mic button
  // could never work while hosted in the side panel. Forward to the
  // shared dispatcher — same path Browser::RequestMediaAccessPermission
  // takes for tab-hosted pages, minus the extension lookup (the chat is
  // a chrome:// WebUI, never an extension origin). chrome:// is
  // allowlisted for MEDIASTREAM_MIC in the content-settings registry,
  // so the mic is granted promptlessly.
  MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
      web_contents, request, std::move(callback), /*extension=*/nullptr);
}

bool AiChatSidePanelWebView::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->CheckMediaAccessPermission(render_frame_host, security_origin, type,
                                   /*extension=*/nullptr);
}

bool AiChatSidePanelWebView::HandleKeyboardEvent(
    content::WebContents* source,
    const input::NativeWebKeyboardEvent& event) {
  // Route unhandled shortcuts back through the FocusManager so the browser
  // Cut/Copy/Paste/SelectAll accelerators fire in the chat textarea. Without
  // this the side panel silently drops Cmd/Ctrl+C/V/X/A. Mirrors
  // SidePanelWebUIView::HandleKeyboardEvent / WebDialogView.
  return unhandled_keyboard_event_handler_.HandleKeyboardEvent(
      event, GetFocusManager());
}

void AiChatSidePanelWebView::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    scoped_refptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  // Open the OS file picker for the chat's paperclip attachment input. Same
  // path Browser::RunFileChooser takes; the base views::WebView delegate has
  // no override, so without this the picker never appears in the side panel.
  FileSelectHelper::RunFileChooser(render_frame_host, std::move(listener),
                                   params);
}

void AiChatSidePanelWebView::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (selection.active_tab_changed())
    PushActiveTabContext();
}

void AiChatSidePanelWebView::ObserveActiveTab() {
  if (!active_tab_observer_ || !browser_ || !browser_->tab_strip_model())
    return;
  active_tab_observer_->SetTab(
      browser_->tab_strip_model()->GetActiveWebContents());
}

void AiChatSidePanelWebView::PushActiveTabContext() {
  if (!browser_ || !browser_->tab_strip_model())
    return;
  ObserveActiveTab();
  content::WebContents* active =
      browser_->tab_strip_model()->GetActiveWebContents();
  if (!active || !active->GetPrimaryMainFrame())
    return;

  std::string url = active->GetLastCommittedURL().spec();
  std::string title = base::UTF16ToUTF8(active->GetTitle());
  const int entry_id = CommittedEntryId(active);
  // Stash the OTR flag on the instance so InjectContextIntoChat can
  // include it in the JSON payload — saves threading another argument
  // through every capture path.
  last_pushed_is_anonymous_ =
      active->GetBrowserContext() &&
      active->GetBrowserContext()->IsOffTheRecord();

  // Bump the generation; capture for the previous tab is now stale.
  int64_t my_gen = ++generation_;

  // Skip text capture for non-web schemes — chrome://, molt://, etc.
  // don't host meaningful "page content" and Chromium often refuses
  // to ExecuteJS in their isolated worlds anyway. Push the metadata
  // immediately with empty text.
  if (!active->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
    InjectContextIntoChat(url, title, std::string(), entry_id,
                          /*text_pending=*/false);
    return;
  }

  // A different document from the one the chat holds (tab switch, new
  // page, SPA route change): drop the previous page's text now rather
  // than when the capture below replies, so the chat can never pair the
  // old text with the new page (A7). Same document (load finished, chat
  // page reloaded): keep the text until the fresh capture replaces it.
  if (entry_id != last_injected_entry_id_ || url != last_injected_url_) {
    InjectContextIntoChat(url, title, std::string(), entry_id,
                          /*text_pending=*/true);
  }

  // PDF chat: PDFs render inside an isolated <embed> whose innerText is
  // unreachable from a content-script. Route through the accessibility
  // tree instead — PDFium publishes the document's text as kName
  // attributes on the leaf AX nodes, so a flat walk gives us the same
  // payload shape as innerText for the chat to ground on.
  if (active->GetContentsMimeType() == pdf::kPDFMimeType) {
    active->RequestAXTreeSnapshot(
        base::BindOnce(&AiChatSidePanelWebView::OnAxTreeCaptured,
                       weak_factory_.GetWeakPtr(), my_gen, url, title,
                       entry_id),
        ui::kAXModeWebContentsOnly,
        /*max_nodes=*/50000,
        /*timeout=*/base::TimeDelta(),
        content::WebContents::AXTreeSnapshotPolicy::kAll);
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
                     weak_factory_.GetWeakPtr(), my_gen, url, title,
                     entry_id),
      /*world_id=*/1);
}

void AiChatSidePanelWebView::OnAxTreeCaptured(int64_t generation,
                                                std::string url,
                                                std::string title,
                                                int entry_id,
                                                ui::AXTreeUpdate& update) {
  if (generation != generation_)
    return;
  std::string text = FlattenAxTreeText(update, kPageTextCharCap);
  // Prefix with a small marker so the chat can render a "PDF" chip
  // distinct from a normal web-page context chip.
  std::string prefixed_title = title.empty() ? std::string("PDF document")
                                              : (title + " (PDF)");
  InjectContextIntoChat(url, prefixed_title, text, entry_id,
                        /*text_pending=*/false);
}

void AiChatSidePanelWebView::OnActiveTabTextCaptured(int64_t generation,
                                                       std::string url,
                                                       std::string title,
                                                       int entry_id,
                                                       base::Value page_text) {
  // Drop the reply if the user switched tabs or the tab navigated while we
  // were capturing (every such change bumps the generation).
  if (generation != generation_)
    return;
  std::string text = page_text.is_string() ? page_text.GetString()
                                            : std::string();
  InjectContextIntoChat(url, title, text, entry_id, /*text_pending=*/false);
}

void AiChatSidePanelWebView::InjectContextIntoChat(const std::string& url,
                                                     const std::string& title,
                                                     const std::string& text,
                                                     int entry_id,
                                                     bool text_pending) {
  if (!web_contents() || !web_contents()->GetPrimaryMainFrame())
    return;
  content::RenderFrameHost* chat_frame = web_contents()->GetPrimaryMainFrame();
  // Main-world execution is only permitted (CHECKed) for WebUI-bound
  // frames, and before the chat page has committed there is no page to
  // receive the context — ChatPageObserver pushes again after it loads.
  const GURL& chat_url = chat_frame->GetLastCommittedURL();
  if (!chat_frame->IsRenderFrameLive() ||
      !chat_url.SchemeIs(content::kChromeUIScheme) ||
      chat_url.host() != kAiChatWebUIHost) {
    return;
  }

  // Build a JSON-safe context payload. This fork uses base::DictValue
  // (not base::Value::Dict) — same pattern as automation_script.cc.
  base::DictValue ctx;
  ctx.Set("url", url);
  ctx.Set("title", title);
  ctx.Set("text", text);
  // Which document |text| was read from (see CommittedEntryId), and
  // whether its text is still being captured.
  ctx.Set("nav_entry_id", entry_id);
  ctx.Set("text_pending", text_pending);
  ctx.Set("is_anonymous_session", last_pushed_is_anonymous_);

  std::string json;
  if (!base::JSONWriter::Write(ctx, &json))
    return;
  last_injected_entry_id_ = entry_id;
  last_injected_url_ = url;

  // Idempotent injection: stash on window.__moltLastTabContext so the
  // chat page can pick it up on its own init (race-free), and call
  // window.__moltSetTabContext if it's already defined. This must run in
  // the page's MAIN world: those are the page's own globals, invisible to
  // an isolated world (the previous isolated-world injection never reached
  // the chat, so the side panel had no page context at all).
  std::string js =
      "(function(c) {"
      "  window.__moltLastTabContext = c;"
      "  if (typeof window.__moltSetTabContext === 'function') {"
      "    try { window.__moltSetTabContext(c); } catch (e) {}"
      "  }"
      "})(" + json + ");";
  chat_frame->ExecuteJavaScript(base::UTF8ToUTF16(js), base::DoNothing());
}

BEGIN_METADATA(AiChatSidePanelWebView)
END_METADATA
